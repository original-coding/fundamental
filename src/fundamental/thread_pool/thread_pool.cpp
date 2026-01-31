/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "thread_pool.h"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
#include <exception>

namespace Fundamental
{

bool ThreadPool::InitThreadPool(const ThreadPoolConfig& config) {
    auto expected = false;
    if (!has_alread_configed.compare_exchange_strong(expected, true)) {
        FERR("thread pool has already configed or has call spawn before init pool, please check your code");
        return false;
    }
    config_ = config;
    if (config_.ilde_wait_time_ms == 0) config_.ilde_wait_time_ms = config_.kDefaultIdleWaitTimeMsec;
    return true;
}

bool ThreadPool::InThreadPool() {
    std::scoped_lock<std::mutex> lock(m_workersMutex);
    return std::any_of(m_workers.begin(), m_workers.end(), [](const decltype(m_workers)::value_type& t) -> bool {
        return t.second->get_id() == std::this_thread::get_id();
    });
}

ThreadPool::~ThreadPool() {
    Join();
}

void ThreadPool::JoinAll() {
    ProducerPool().Join();
    ConsumerPool().Join();
    BlockTimePool().Join();
    LongTimePool().Join();
    DefaultPool().Join();
    ScalingManagerInstance().Join();
}

std::size_t ThreadPool::Count() const {
    std::scoped_lock<std::mutex> lock(m_workersMutex);
    return m_workers.size();
}

std::size_t ThreadPool::PendingTasks() const {
    std::scoped_lock<std::mutex> lock(m_tasksMutex);
    return m_tasks.size();
}

std::size_t ThreadPool::ProcessingTasks() const {
    return processing_cnt.load(std::memory_order::memory_order_relaxed) +
           waiting_cnt.load(std::memory_order::memory_order_relaxed);
}

void ThreadPool::Spawn(int count) {
    if (m_joining || count < 1) return;
    auto expected_value = false;
    if (has_alread_configed.compare_exchange_strong(expected_value, true)) {
        FDEBUG("we recommend you to call InitThreadPool before call Spawn to reserve work threads");
        config_.min_work_threads_num = count;
    }
    std::scoped_lock<std::mutex> lock(m_workersMutex);
    std::int32_t left_threads =
        config_.max_threads_limit > 0 ? static_cast<std::int32_t>(config_.max_threads_limit - m_workers.size()) : count;
    count = count > left_threads ? left_threads : count;
    if (m_workers.size() + count < config_.min_work_threads_num) {
        count += static_cast<decltype(count)>(config_.min_work_threads_num - m_workers.size());
    }
    if (count == 0) return;

    while (count-- > 0) {
        m_workers[spawn_cnt] = std::make_unique<std::thread>(std::bind(&ThreadPool::Run, this, spawn_cnt));
        ++spawn_cnt;
    }
}

std::size_t ThreadPool::Join() {
    FASSERT_ACTION(!InThreadPool(), throw std::runtime_error("try not call thread_pool' Join() in itself"),
                   "try not call thread pool join in itself");
    auto expected = false;
    if (!m_joining.compare_exchange_strong(expected, true)) return 0;
    {
        std::scoped_lock<std::mutex> locker(m_tasksMutex);
        m_condition.notify_all();
        m_task_update_cv.notify_all();
    }
    std::size_t ret = 0;
    {
        decltype(m_workers) tmp;
        {
            std::scoped_lock<std::mutex> lock(m_workersMutex);
            ret = m_workers.size();
            std::swap(tmp, m_workers);
        }

        for (auto& w : tmp)
            if (w.second->joinable()) w.second->join();
        tmp.clear();
    }
    { // clear all tasks
        std::scoped_lock<std::mutex> locker(m_tasksMutex);
        while (!m_tasks.empty())
            m_tasks.pop();
    }

    return ret;
}

bool ThreadPool::WaitAllTaskFinished() const {
    std::unique_lock<std::mutex> lock(m_tasksMutex);
    while (!m_joining) {
        if (m_task_update_cv.wait_for(lock, std::chrono::milliseconds(20),
                                      [&]() -> bool { return m_tasks.empty() && 0 == ProcessingTasks(); }))
            return true;
    }
    return false;
}

bool ThreadPool::WaitIdleThread(std::size_t need_num) const {
    if (need_num == 0) return true;
    std::unique_lock<std::mutex> lock(m_tasksMutex);
    if (config_.max_threads_limit == 0) return true;
    if (config_.max_threads_limit > 0 && need_num > config_.max_threads_limit) return false;
    while (!m_joining) {
        if (m_task_update_cv.wait_for(lock, std::chrono::milliseconds(20), [&]() -> bool {
                auto current_task = ProcessingTasks();
                return m_tasks.size() == 0 && (need_num + current_task <= config_.max_threads_limit);
            }))
            return true;
    }
    return false;
}

void ThreadPool::Run(std::size_t index) {
    std::string thread_name = "thp_" + std::to_string(type) + "_" + std::to_string(index);
    Fundamental::Utils::SetThreadName(thread_name);
    bool is_timeout = false;
    // we won't auto release reserved threads
    std::int64_t ilde_wait_max_time_ms = index >= config_.min_work_threads_num ? config_.ilde_wait_time_ms : 0;
    while (RunOne(ilde_wait_max_time_ms, is_timeout)) {
        // reset thread name
        Fundamental::Utils::SetThreadName(thread_name);
        if (is_timeout && config_.enable_auto_scaling) {
            // remove current work thread
            std::unique_ptr<std::thread> op_thread;
            {
                std::scoped_lock<std::mutex> lock(m_workersMutex);
                auto iter = m_workers.find(index);
                if (iter != m_workers.end()) {
                    op_thread = std::move(iter->second);
                    m_workers.erase(iter);
                }
            }
            // may join has been called
            if (!op_thread) return;
            // let default pool excute join operation
            ScalingManagerInstance().Schedule<false>(clock_t::now(), [op_thread = std::move(op_thread)]() mutable {
                if (op_thread && op_thread->joinable()) op_thread->join();
            });
            break;
        }
    }
}

void ThreadPool::PrepareWorkers(std::size_t current_task_nums) {
    if (!config_.enable_auto_scaling) return;
    auto pending_wait_size = waiting_threads.load();
    if (pending_wait_size < current_task_nums) Spawn(static_cast<std::int32_t>(current_task_nums - pending_wait_size));
}

bool ThreadPool::RunOne(std::int64_t idle_wait_time_ms, bool& is_timeout) {
    // increase cnt for finished process tasks
    ++waiting_threads;
    // reset timeout flag
    is_timeout = false;
    ThreadPool::Task local_task_cache;
    auto task = Dequeue(idle_wait_time_ms, is_timeout, local_task_cache);
    // decrease cnt for start process tasks
    --waiting_threads;
    if (is_timeout) return true;
    if (task.func) {
        auto expected = ThreadPoolTaskStatus::ThreadTaskWaitting;
        // task maybe cancelled
        if (!task.status->status.compare_exchange_strong(expected, ThreadPoolTaskStatus::ThreadTaskRunning))
            return true;
        Fundamental::ScopeGuard g(
            [&]() {
                processing_cnt.fetch_sub(1, std::memory_order::memory_order_relaxed);
                std::scoped_lock<std::mutex> lock(m_tasksMutex);
                m_task_update_cv.notify_all();
            },
            [&]() { processing_cnt.fetch_add(1, std::memory_order::memory_order_relaxed); });
        task.func->operator()();

        task.status->status.exchange(ThreadPoolTaskStatus::ThreadTaskDone);
        return true;
    }
    return false;
}

ThreadPool::Task ThreadPool::Dequeue(std::int64_t idle_wait_time_ms, bool& is_timeout, ThreadPool::Task& cache_task) {
    std::unique_lock<std::mutex> lock(m_tasksMutex);
    while (true) {
        // check cache task
        if (cache_task.func || !m_tasks.empty()) {
            // there is a  task which can be execute
            if (m_joining) break;

            if (cache_task.func) {
                // choose a earlier one
                if (cache_task.time > m_tasks.top().time) {
                    ThreadPool::Task swap_task = m_tasks.top();
                    m_tasks.pop();
                    std::swap(cache_task, swap_task);
                    m_tasks.push(std::move(swap_task));
                }
            } else {
                // cache new task
                cache_task = m_tasks.top();
                waiting_cnt.fetch_add(1, std::memory_order::memory_order_relaxed);
                m_tasks.pop();
            }
            // we should execute this task now
            if (cache_task.time <= clock_t::now()) {

                ThreadPool::Task task = std::move(cache_task);
                waiting_cnt.fetch_sub(1, std::memory_order::memory_order_relaxed);
                return task;
            }
            // wait until this task get  expired
            m_condition.wait_until(lock, cache_task.time);

        } else {
            m_task_update_cv.notify_all();
            if (m_joining) break;
            if (idle_wait_time_ms > 0) {
                auto status = m_condition.wait_for(lock, std::chrono::milliseconds(idle_wait_time_ms));
                is_timeout  = status == std::cv_status::timeout;
                if (is_timeout) break;
            } else {
                m_condition.wait(lock);
            }
        }
    }
    return ThreadPool::Task();
}
} // namespace Fundamental
