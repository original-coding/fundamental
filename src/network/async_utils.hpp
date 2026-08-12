//
// @author lightning1993 <469953258@qq.com> 2026/08
//
// 网络层公共异步工具：规范（docs/asio-async-standards.md）的实现支撑。
// 全部为 header-only，独立小工具，不做框架级抽象。
//
#pragma once

#include "network/use_asio.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace network
{

// =============================================================================
// 生命周期状态机（对应规范 §2）
// 四状态：closed → starting → started → stopping → closed
// 状态迁移必须通过 try_transition（CAS），保证多线程下只有一次合法迁移、迁移幂等。
// =============================================================================

enum class connection_state : std::int8_t
{
    closed,
    starting,
    started,
    stopping,
};

inline const char* to_string(connection_state s) noexcept
{
    switch (s)
    {
    case connection_state::closed:   return "closed";
    case connection_state::starting: return "starting";
    case connection_state::started:  return "started";
    case connection_state::stopping: return "stopping";
    default:                         return "unknown";
    }
}

/// CAS 迁移：当前值等于 expected 才迁移到 desired；失败返回 false（调用方应直接返回，保证幂等）。
template <typename State>
inline bool try_transition(std::atomic<State>& state, State expected, State desired)
{
    return state.compare_exchange_strong(expected, desired);
}

/// closed → starting（开始连接/握手）
inline bool try_start(std::atomic<connection_state>& state)
{
    return try_transition(state, connection_state::closed, connection_state::starting);
}

/// starting → started（连接完全建立，可收发数据）
inline bool try_mark_started(std::atomic<connection_state>& state)
{
    return try_transition(state, connection_state::starting, connection_state::started);
}

/// started/starting → stopping（发起关闭；已关闭或关闭中则失败——重复关闭幂等）
inline bool try_close(std::atomic<connection_state>& state)
{
    return try_transition(state, connection_state::started, connection_state::stopping) ||
           try_transition(state, connection_state::starting, connection_state::stopping);
}

/// stopping → closed（关闭序列收尾）
inline bool try_mark_closed(std::atomic<connection_state>& state)
{
    return try_transition(state, connection_state::stopping, connection_state::closed);
}

// =============================================================================
// 线程亲和性投递（对应规范 §1.2、§3.2）
// safe_post / safe_dispatch：weak 提升成功才执行（对象已销毁则跳过，不会 UAF）。
// post_keepalive / dispatch_keepalive：调用方已持有强引用，投递期间保活。
// 回调签名统一为 void(const std::shared_ptr<T>&)，入参即提升成功后的强引用。
// =============================================================================

/// 投递到 executor（支持 executor 或 io_context）；对象已销毁则任务静默跳过。任意线程可调用。
template <typename Executor, typename T, typename Fn>
inline void safe_post(Executor&& executor, const std::weak_ptr<T>& weak, Fn&& fn)
{
    asio::post(std::forward<Executor>(executor), [weak, fn = std::forward<Fn>(fn)]() mutable {
        auto strong = weak.lock();
        if (!strong) return;
        fn(strong);
    });
}

/// 同 safe_post，但调用者已在 io 线程时立即执行（io 线程内部继续处理用）。
template <typename Executor, typename T, typename Fn>
inline void safe_dispatch(Executor&& executor, const std::weak_ptr<T>& weak, Fn&& fn)
{
    asio::dispatch(std::forward<Executor>(executor), [weak, fn = std::forward<Fn>(fn)]() mutable {
        auto strong = weak.lock();
        if (!strong) return;
        fn(strong);
    });
}

/// 投递到 executor（支持 executor 或 io_context），任务执行期间持有强引用保活对象。任意线程可调用。
template <typename Executor, typename T, typename Fn>
inline void post_keepalive(Executor&& executor, const std::shared_ptr<T>& strong, Fn&& fn)
{
    asio::post(std::forward<Executor>(executor), [strong, fn = std::forward<Fn>(fn)]() mutable { fn(strong); });
}

/// 同 post_keepalive，但调用者已在 io 线程时立即执行。
template <typename Executor, typename T, typename Fn>
inline void dispatch_keepalive(Executor&& executor, const std::shared_ptr<T>& strong, Fn&& fn)
{
    asio::dispatch(std::forward<Executor>(executor), [strong, fn = std::forward<Fn>(fn)]() mutable { fn(strong); });
}

/// 阻塞式投递：在 executor 的 io 线程上执行 fn，执行完成才返回。
/// - 已在 io 线程：立即内联执行（不会自我死锁）
/// - 其他线程：投递到 io 线程并阻塞等待完成
/// 契约：调用期间 fn 引用的对象必须存活（典型场景：析构函数体内做兜底清理，
/// 此时析构尚未返回，对象及其成员仍然有效；weak 提升在析构期间必然失败，
/// 因此这里必须传裸 this，由阻塞等待保证安全）。
/// fn 抛出的异常会原样传播到调用线程。
template <typename Executor, typename Fn>
inline void post_and_wait(Executor&& executor, Fn&& fn)
{
    std::promise<void> promise;
    auto future = promise.get_future();
    // dispatch：io 线程内直接执行（promise 立即置值，随后的 future.get() 直接返回），
    // 其他线程投递到 io 线程后阻塞等待。无需依赖 executor 的 running_in_this_thread。
    asio::dispatch(std::forward<Executor>(executor),
                   [fn = std::forward<Fn>(fn), promise = std::move(promise)]() mutable {
                       try
                       {
                           fn();
                           promise.set_value();
                       }
                       catch (...)
                       {
                           promise.set_exception(std::current_exception());
                       }
                   });
    future.get();
}

// =============================================================================
// 串行化写队列（对应规范 §3.4）
// 同一 socket 至多一个在途 async_write；pop 只在完成回调中执行。
// 使用方式：构造时提供"发起写"的 launcher（把数据和完成回调交给 asio::async_write），
// 之后任意线程 push 即可。keepalive 必须持有所属对象的强引用（通常是
// [self = shared_from_this()] 之类的守卫），保证完成回调执行时 writer 及其所属对象存活。
// 错误路径：停止发送并调用 error_handler（应由此触发关闭序列），失败的包保留在队列中，
// 由关闭序列的 clear() 统一丢弃。
// =============================================================================

class serialized_writer : private asio::noncopyable
{
public:
    /// 发起一次写。data 由 launcher 复制持有（如捕获进 asio 操作），completion 由 writer 提供，
    /// launcher 必须原样交给 asio::async_write 作为完成回调。
    using write_launcher_t = std::function<void(const std::shared_ptr<std::string>& data,
                                                network_io_handler_t completion)>;
    using error_handler_t  = std::function<void(std::error_code)>;
    using keepalive_t      = std::function<void()>;

    explicit serialized_writer(asio::any_io_executor executor, write_launcher_t launcher, keepalive_t keepalive = {});
    ~serialized_writer() = default;

    /// 入队并发送（无在途写时立即发起）。任意线程可调用；实际入队与发送在 io 线程执行。
    void push(std::shared_ptr<std::string> data);

    /// 写失败时的回调（io 线程执行）。应由此触发统一关闭序列。
    void set_error_handler(error_handler_t handler);

    /// 丢弃所有 pending。必须在 io 线程调用（通常在关闭序列中）。
    /// 同时清空 keepalive/error_handler：二者常驻强引用所属对象（如 relay 自环），
    /// 不清则 close 后对象永不析构（ASAN 泄漏）。在途 push/写完成回调各自持有副本，
    /// 清空本 writer 成员不影响它们执行。
    void clear();

    std::size_t pending() const noexcept { return queue_.size(); }
    bool        writing() const noexcept { return writing_; }
    bool        empty() const noexcept { return queue_.empty() && !writing_; }

private:
    void do_push(std::shared_ptr<std::string> data); // io 线程
    void do_write();                                 // io 线程
    void complete(std::error_code ec);               // io 线程（完成回调入口）

    asio::any_io_executor executor_;
    write_launcher_t      launcher_;
    keepalive_t           keepalive_;
    error_handler_t       error_handler_;
    std::deque<std::shared_ptr<std::string>> queue_;
    bool writing_ = false;
};

inline serialized_writer::serialized_writer(asio::any_io_executor executor,
                                            write_launcher_t launcher,
                                            keepalive_t keepalive) :
executor_(std::move(executor)), launcher_(std::move(launcher)), keepalive_(std::move(keepalive)) {}

inline void serialized_writer::push(std::shared_ptr<std::string> data)
{
    if (!data || data->empty()) return;
    // keepalive 随任务持有所属对象，保证 io 线程执行 do_push 时 this 存活
    asio::post(executor_, [this, keepalive = keepalive_, data = std::move(data)]() mutable {
        do_push(std::move(data));
    });
}

inline void serialized_writer::do_push(std::shared_ptr<std::string> data)
{
    queue_.push_back(std::move(data));
    if (!writing_) do_write();
}

inline void serialized_writer::do_write()
{
    if (queue_.empty())
    {
        writing_ = false;
        return;
    }
    writing_ = true;
    auto front = queue_.front();
    launcher_(front, [this, keepalive = keepalive_](std::error_code ec, std::size_t) {
        if (ec)
        {
            writing_ = false;
            if (error_handler_) error_handler_(ec);
            return;
        }
        queue_.pop_front(); // pop 只在完成回调中执行
        do_write();
    });
}

inline void serialized_writer::set_error_handler(error_handler_t handler)
{
    error_handler_ = std::move(handler);
}

inline void serialized_writer::clear()
{
    queue_.clear();
    writing_ = false;
    keepalive_ = {};
    error_handler_ = {};
}

// =============================================================================
// pending 请求清理（对应规范 §4.1、§5.2）
// 关闭序列中把 pending 容器整体搬出（防止回调内重入修改原容器），再逐项以错误码回掉。
// =============================================================================

template <typename PendingContainer, typename FailFn>
inline std::size_t fail_pending(PendingContainer& pending, FailFn&& fail_one)
{
    PendingContainer tmp;
    tmp.swap(pending);
    std::size_t count = 0;
    for (auto& item : tmp)
    {
        fail_one(item);
        ++count;
    }
    return count;
}

} // namespace network
