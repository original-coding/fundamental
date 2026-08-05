#pragma once

#include <asio.hpp>
#include <atomic>
#include <cstdint>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fundamental/application/application.hpp"
#include "fundamental/basic/utils.hpp"
#include "fundamental/events/event_system.h"
#include "fundamental/thread_pool/thread_utils.hpp"

namespace network
{
/// A pool of io_context objects.
class io_context_pool : public Fundamental::Singleton<io_context_pool> {
private:
    std::shared_ptr<Fundamental::Signal<void(std::error_code /*ec*/, int /*signo*/)>> notify_sys_signal_storage;

public:
    inline static std::size_t s_excutorNums = 0;
    Fundamental::Signal<void(std::error_code /*ec*/, int /*signo*/)>& notify_sys_signal;

public:
    /// Construct the io_context pool.
    io_context_pool();
    ~io_context_pool();
    /// Run all io_context objects in the pool.
    void start();

    /// Stop all io_context objects in the pool. 幂等；阻塞直到所有 io_context 排空并停止。
    /// 序列：唤醒 wait_stop → 取消已登记定时器 → 释放 work guard → 等待全部 io_context 停止。
    void stop();

    /// 阻塞当前线程，直到 stop() 被调用。必须由非 io 线程调用（io 线程内调用会死锁）。
    void wait_stop();

    /// Get an io_context to use.
    asio::io_context& get_io_context();

    /// Whether the current thread is one of the pool's io threads.
    bool running_in_io_thread() const;

    /// Register a timer so it will be cancelled when the pool stops.
    /// 定时器析构时必须调用 unreg_timer；停止后池会统一取消所有已登记定时器。
    void reg_timer(asio::steady_timer& timer);
    void unreg_timer(asio::steady_timer& timer);

    /// Register an object so its release callback is invoked when the pool stops.
    /// key 通常传对象的 this 指针；对象停止时必须调用 unreg_object（否则池停止时会再次调用其回调）。
    /// 停止开始后注册会立即调用回调（对象必须马上释放）。
    void reg_object(const void* key, std::function<void()> on_stop);
    void unreg_object(const void* key);

private:
    io_context_pool(const io_context_pool&)            = delete;
    io_context_pool& operator=(const io_context_pool&) = delete;
    typedef std::shared_ptr<asio::io_context> io_context_ptr;
    typedef asio::executor_work_guard<asio::io_context::executor_type> io_context_work;

    void cancel_registered_timers();
    void drive_registered_objects();
    void wake_wait_stop();

    /// The pool of io_contexts.
    std::vector<io_context_ptr> io_contexts_;

    /// The work that keeps the io_contexts running.
    std::list<io_context_work> work_;

    /// The next io_context to use for a connection.
    std::atomic<std::size_t> next_io_context_{0};

    /// Idempotent stop guard.
    std::atomic_bool stopping_ = false;

    /// The thread id of each io_context's run thread (filled by each io thread itself).
    std::vector<std::thread::id> thread_ids_;

    /// Registered timers; cancelled uniformly when the pool stops.
    std::mutex timers_mutex_;
    std::unordered_set<asio::steady_timer*> timers_;

    /// Registered objects (key = object pointer); released uniformly when the pool stops.
    /// 保证"池停 → 所有对象必然被 stop"，与顶层对象的 make_guard（exit 信号）互补。
    std::mutex objects_mutex_;
    std::unordered_map<const void*, std::function<void()>> objects_;

    /// The timer used by wait_stop() (created/cancelled only on io_contexts_[0]'s thread).
    std::unique_ptr<asio::steady_timer> wait_stop_timer_;

    /// The signal_set is used to register for process termination notifications.
    asio::signal_set signals_;
};
inline void init_io_context_pool(std::size_t work_threads = Fundamental::hardware_concurrency()) {
    network::io_context_pool::s_excutorNums = work_threads;
    network::io_context_pool::Instance().start();
    Fundamental::Application::Instance().exitStarted.Connect([&]() { network::io_context_pool::Instance().stop(); });
    network::io_context_pool::Instance().notify_sys_signal.Connect(
        [](std::error_code code, std::int32_t signo) { Fundamental::Application::Instance().Exit(); });
}

} // namespace network
