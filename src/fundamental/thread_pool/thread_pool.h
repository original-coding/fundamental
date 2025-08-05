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
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional> //bind
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
namespace Fundamental
{
using clock_t = std::chrono::steady_clock;

// see ThreadPool::Instance for more details

enum ThreadPoolType : std::int32_t
{
    ShortTimeThreadPool = 0, // reserve at least one thread to recycle glocbal resources
    LongTimeThreadPool  = 1,
    BlockTimeThreadPool = 2, // has no thread nums limit
    PrallelThreadPool   = 3, // init with external config
    ProducerThreadPool  = 4,
    ConsumerThreadPool  = 5
};

enum ThreadPoolTaskStatus : std::uint32_t
{
    ThreadTaskWaitting,
    ThreadTaskRunning,
    ThreadTaskDone,
    ThreadTaskCancelled
};

struct ThreadPoolTaskStatusSyncerWarpper {
    std::atomic<ThreadPoolTaskStatus> status = ThreadTaskWaitting;
};

template <typename ResultType>
struct ThreadPoolTaskToken {
    bool CancelTask() {
        auto expected = ThreadPoolTaskStatus::ThreadTaskWaitting;
        return status->status.compare_exchange_strong(expected, ThreadPoolTaskStatus::ThreadTaskCancelled);
    }
    std::shared_ptr<ThreadPoolTaskStatusSyncerWarpper> status = std::make_shared<ThreadPoolTaskStatusSyncerWarpper>();
    std::future<ResultType> resultFuture;
};

struct ThreadPoolConfig {
    static std::size_t normal_thread_num_limit() {
        return std::thread::hardware_concurrency() * 2;
    }
    static constexpr std::int64_t kDefaultIdleWaitTimeMsec = 100;
    bool enable_auto_scaling                               = true;
    // 0 means no limit
    std::size_t max_threads_limit    = 0;
    std::size_t min_work_threads_num = 0;
    std::int64_t ilde_wait_time_ms   = kDefaultIdleWaitTimeMsec;
};
class ThreadPool final {
    struct ExecutorBase {
        virtual ~ExecutorBase() = default;
        virtual void execute() const {
        }
        virtual void operator()() {
        }
    };

    template <typename _Callable>
    struct Executor : public ExecutorBase {
        Executor(_Callable&& f) : func(std::move(f)) {
        }
        Executor(Executor&& other) : func(std::move(other.func)) {
        }
        void operator()() override {
            func();
        }
        Executor(const Executor& other)            = delete;
        Executor& operator=(const Executor& other) = delete;
        Executor& operator=(Executor&& other) {
            func = std::move(other.func);
            return *this;
        }
        _Callable func;
    };

    struct Task {
        clock_t::time_point time;
        std::shared_ptr<ExecutorBase> func;
        std::shared_ptr<ThreadPoolTaskStatusSyncerWarpper> status;
        bool operator>(const Task& other) const {
            return time > other.time;
        }
        bool operator<(const Task& other) const {
            return time < other.time;
        }
    };

public:
    template <std::int32_t Index = ShortTimeThreadPool>
    static ThreadPool& Instance() {
        // Init handles joining on cleanup
        if constexpr (Index == ShortTimeThreadPool) {
            static ThreadPool* instance = new ThreadPool(1, 0, Index);
            return *instance;
        } else if constexpr (Index == BlockTimeThreadPool || Index == LongTimeThreadPool) {
            static ThreadPool* instance = new ThreadPool(0, 0, Index);
            return *instance;
        } else if constexpr (Index == PrallelThreadPool) {
            static ThreadPool* instance = new ThreadPool(Index);
            return *instance;
        } else {
            static ThreadPool* instance = new ThreadPool(0, ThreadPoolConfig::normal_thread_num_limit(), Index);
            return *instance;
        }
    }
    // always reserve an thread
    static ThreadPool& DefaultPool() {
        return Instance<ShortTimeThreadPool>();
    }
    static ThreadPool& LongTimePool() {
        return Instance<LongTimeThreadPool>();
    }
    // has no thread nums limit
    static ThreadPool& BlockTimePool() {
        return Instance<BlockTimeThreadPool>();
    }
    static ThreadPool& PrallelTaskPool() {
        return Instance<PrallelThreadPool>();
    }
    static ThreadPool& ProducerPool() {
        return Instance<ProducerThreadPool>();
    }
    static ThreadPool& ConsumerPool() {
        return Instance<ConsumerThreadPool>();
    }
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&)                 = delete;
    ThreadPool& operator=(ThreadPool&&)      = delete;
    // return the current number of worker threads
    std::size_t Count() const;
    std::size_t PendingTasks() const;
    // return the number of currently executing tasks
    std::size_t ProcessingTasks() const;
    void Spawn(int count = 1);
    void Spawn(std::size_t count = 1) {
        Spawn(static_cast<std::int32_t>(count));
    }
    /// @brief  join all work threads
    /// @return joined thread nums
    std::size_t Join();
    // wait for all tasks to complete execution
    bool WaitAllTaskFinished() const;
    //
    bool WaitIdleThread(std::size_t need_num) const;
    template <typename _Callable, typename... _Args>
    auto Enqueue(_Callable&& f, _Args&&... args) -> ThreadPoolTaskToken<std::invoke_result_t<_Callable, _Args...>> {
        return Schedule<true>(clock_t::now(), std::forward<_Callable>(f), std::forward<_Args>(args)...);
    }

    template <typename _Callable, typename... _Args>
    auto Schedule(clock_t::duration delay, _Callable&& f, _Args&&... args)
        -> ThreadPoolTaskToken<std::invoke_result_t<_Callable, _Args...>> {
        return Schedule<true>(clock_t::now() + delay, std::forward<_Callable>(f), std::forward<_Args>(args)...);
    }

    template <bool prepare_worker, typename _Callable, typename... _Args>
    auto Schedule(clock_t::time_point time, _Callable&& f, _Args&&... args)
        -> ThreadPoolTaskToken<std::invoke_result_t<_Callable, _Args...>> {

        using ResultType = std::invoke_result_t<_Callable, _Args...>;
        auto promise     = std::make_shared<std::promise<ResultType>>();
        auto task = [func = std::forward<_Callable>(f), args_tuple = std::make_tuple(std::forward<_Args>(args)...),
                     promise]() mutable {
            try {
                if constexpr (std::is_same_v<ResultType, void>) {
                    std::apply(func, std::move(args_tuple));
                    promise->set_value();
                } else {
                    promise->set_value(std::apply(func, std::move(args_tuple)));
                }
            } catch (const std::exception& e) {
                promise->set_exception(std::make_exception_ptr(e));
            }
        };
        ThreadPoolTaskToken<ResultType> result;
        result.resultFuture = std::move(promise->get_future());
        std::unique_lock<std::mutex> lock(m_tasksMutex);

        m_tasks.push(
            { time, std::make_shared<Executor<std::decay_t<decltype(task)>>>(std::move(task)), result.status });
        m_condition.notify_one();
        [[maybe_unused]] auto current_size = m_tasks.size();
        lock.unlock();
        if constexpr (prepare_worker) {
            PrepareWorkers(current_size);
        }
        return result;
    }

    bool InitThreadPool(const ThreadPoolConfig& config);
    bool InThreadPool();
    ThreadPool(std::int32_t type = ThreadPoolType::ShortTimeThreadPool) : type(type) {
    }
    ~ThreadPool();
    static void JoinAll();

protected:
    ThreadPool(std::size_t min_tread_num,
               std::size_t max_thread_num,
               std::int32_t type = ThreadPoolType::ShortTimeThreadPool) : type(type) {
        has_alread_configed.exchange(true);
        config_.max_threads_limit    = max_thread_num;
        config_.min_work_threads_num = min_tread_num;
        // reserve minimal work threads
        if (config_.min_work_threads_num > 0LU) Spawn(static_cast<std::int32_t>(config_.min_work_threads_num));
    }
    Task Dequeue(std::int64_t idle_wait_time_ms, bool& is_timeout); // returns null function if joining
    void Run(std::size_t index);
    void PrepareWorkers(std::size_t current_task_nums);
    bool RunOne(std::int64_t idle_wait_time_ms, bool& is_timeout);

private:
    std::atomic_bool has_alread_configed = false;
    ThreadPoolConfig config_;
    std::size_t spawn_cnt = 0;
    const std::int32_t type;
    std::atomic<std::size_t> waiting_threads = 0;
    std::map<std::size_t, std::unique_ptr<std::thread>> m_workers;
    std::atomic<bool> m_joining { false };

    std::priority_queue<Task, std::deque<Task>, std::greater<Task>> m_tasks;
    std::atomic<std::size_t> processing_cnt = 0;

    mutable std::mutex m_tasksMutex, m_workersMutex;
    mutable std::condition_variable m_condition, m_task_update_cv;
};

} // namespace Fundamental
