#include "io_context_pool.hpp"
#include "fundamental/application/application.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
#include "fundamental/thread_pool/thread_pool.h"

#include <future>
#include <stdexcept>
#include <thread>

#ifdef _MSC_VER
#include <windows.h>
#endif
namespace network
{

io_context_pool::io_context_pool() :
notify_sys_signal_storage(new Fundamental::Signal<void(std::error_code /*ec*/, int /*signo*/)>()),
notify_sys_signal(*notify_sys_signal_storage), io_contexts_ { io_context_ptr(new asio::io_context) },
next_io_context_(0), signals_(*io_contexts_[0]) {
    work_.push_back(asio::make_work_guard(*io_contexts_[0]));
    // WARRNING: this context pool works as a singleton,so we
    //  should release all resource reference before our application is exited
    //  when the static resource will be recycled, this action avoids error access
    //  to no-static objects
}

io_context_pool::~io_context_pool() {
    stop();
}

void io_context_pool::start() {
    // Give all the io_contexts work to do so that their run() functions will not
    // exit until they are explicitly stopped.
    for (std::size_t i = 0; i < s_excutorNums; ++i) {
        io_contexts_.emplace_back(io_context_ptr(new asio::io_context));
        work_.push_back(asio::make_work_guard(*io_contexts_.back()));
    }
    thread_ids_.resize(io_contexts_.size());

    auto& threadpool = Fundamental::ThreadPool::BlockTimePool();
    // enqueue all of the io_contexts run task
    for (std::size_t i = 0; i < io_contexts_.size(); ++i) {
        threadpool.Spawn(1);
        threadpool.Enqueue([this, i] {
            thread_ids_[i] = std::this_thread::get_id();
            Fundamental::Utils::SetThreadName(Fundamental::StringFormat("io_loop_{}", i));
#ifdef _MSC_VER
            SetThreadUILanguage(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
#endif
            // io 线程不允许被异常打死：handler/回调链已逐层隔离（Signal/解析/序列化兜底），
            // 此处仅作最后防线——记录 + 重启 run 尝试恢复，而不是 FASSERT 终止进程
            for (;;) {
                try {
                    io_contexts_[i]->run();
                    break; // 正常退出（stop 或排空）
                } catch (const std::exception& e) {
                    FERR("asio context err:{}", e.what());
                    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 防热旋
                    try {
                        io_contexts_[i]->restart();
                    } catch (...) {
                    }
                }
            }
        });
    }
    auto init_f = Fundamental::ThreadPool::DefaultPool().Enqueue([&]() {
        // init dns resolve thread
        for (std::size_t i = 0; i < io_contexts_.size(); ++i) {
            Fundamental::Utils::SetThreadName(Fundamental::StringFormat("dns_solve_{}", i));
            ::asio::ip::tcp::resolver resolver(*io_contexts_[i]);
            std::promise<void> init_p;
            resolver.async_resolve(
                "localhost", "9000",
                [&](const std::error_code&, const ::asio::ip::tcp::resolver::results_type&) { init_p.set_value(); });
            init_p.get_future().get();
        }
    });
    init_f.resultFuture.get();
    // Register to handle the signals that indicate when the server should exit.
    // It is safe to register for the same signal multiple times in a program,
    // provided all registration for the specified signal is made through Asio.
    signals_.add(SIGINT);
    signals_.add(SIGTERM);
#if defined(SIGQUIT)
    signals_.add(SIGQUIT);
#endif // defined(SIGQUIT)
    signals_.async_wait([s = notify_sys_signal_storage](std::error_code ec, int signo) {
        FERR("recv signo:{} msg:{}", signo, ec.message());
        if (ec) return;
        s->Emit(std::move(ec), signo);
    });
}

void io_context_pool::stop() {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) return; // 幂等
    FDEBUG("try stop io context pool");

    // 1. 唤醒所有 wait_stop() 阻塞方（wait_stop_timer_ 只在 io_contexts_[0] 线程上操作）
    wake_wait_stop();

    // 2. 驱动所有已登记对象释放（其 release_obj 内部 post，安全）：
    //    必须发生在等待 io 排空之前——否则关闭序列尚未入队，io 排空时对象还挂着
    drive_registered_objects();

    // 3. 统一取消已登记定时器：解决"stop 后仍有定时器存活导致 io_context 无法退出"的问题
    cancel_registered_timers();

    // 4. 释放 work guard：io_context 处理完剩余任务（含第 2 步入队的关闭序列）后 run() 自然返回
    work_.clear();
    std::error_code ec;
    signals_.cancel(ec);

    // 5. 阻塞等待所有 io_context 排空并停止（优雅退出）
    for (std::size_t i = 0; i < io_contexts_.size(); ++i) {
        while (!io_contexts_[i]->stopped()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        thread_ids_[i] = std::thread::id{};
    }
}

void io_context_pool::wait_stop() {
    if (stopping_.load()) return; // 已停止或正在停止
    if (running_in_io_thread()) {
        FERR("wait_stop must not be called in io thread, will cause deadlock");
        return;
    }
    if (io_contexts_.empty()) return;

    std::promise<void> promise;
    auto future = promise.get_future();

    // wait_stop_timer_ 只在 io_contexts_[0] 的线程上创建/取消（asio2 同款模式）
    asio::post(*io_contexts_[0], [this, promise = std::move(promise)]() mutable {
        if (this->wait_stop_timer_) {
            this->wait_stop_timer_->cancel(); // 返回取消的等待数，无需 error_code
            this->wait_stop_timer_.reset();
        }
        this->wait_stop_timer_ = std::make_unique<asio::steady_timer>(*this->io_contexts_[0]);
        this->reg_timer(*this->wait_stop_timer_); // 兜底：即使 stop 走其他路径，也会被统一取消
        this->wait_stop_timer_->expires_after((std::chrono::nanoseconds::max)());
        this->wait_stop_timer_->async_wait([promise = std::move(promise)](const std::error_code&) mutable {
            promise.set_value();
        });
    });

    // 阻塞直到 stop() 取消该定时器
    future.get();
}

asio::io_context& io_context_pool::get_io_context() {
    // Use a round-robin scheme to choose the next io_context to use.
    std::size_t index = next_io_context_.fetch_add(1, std::memory_order_relaxed);
    return *io_contexts_[index % io_contexts_.size()];
}

bool io_context_pool::running_in_io_thread() const {
    std::thread::id curr_tid = std::this_thread::get_id();
    for (std::thread::id id : thread_ids_) {
        if (id == curr_tid) return true;
    }
    return false;
}

void io_context_pool::reg_timer(asio::steady_timer& timer) {
    std::lock_guard<std::mutex> locker(timers_mutex_);
    timers_.insert(&timer);
}

void io_context_pool::unreg_timer(asio::steady_timer& timer) {
    std::lock_guard<std::mutex> locker(timers_mutex_);
    timers_.erase(&timer);
}

void io_context_pool::reg_object(const void* key, std::function<void()> on_stop) {
    if (!key || !on_stop) return;
    std::lock_guard<std::mutex> locker(objects_mutex_);
    objects_[key] = std::move(on_stop);
}

void io_context_pool::unreg_object(const void* key) {
    std::lock_guard<std::mutex> locker(objects_mutex_);
    objects_.erase(key);
}

void io_context_pool::drive_registered_objects() {
    std::unordered_map<const void*, std::function<void()>> objects;
    {
        std::lock_guard<std::mutex> locker(objects_mutex_);
        objects.swap(objects_);
    }
    for (auto& [key, on_stop] : objects) {
        (void)key;
        if (on_stop) on_stop();
    }
}

void io_context_pool::cancel_registered_timers() {
    std::unordered_set<asio::steady_timer*> timers;
    {
        std::lock_guard<std::mutex> locker(timers_mutex_);
        timers.swap(timers_);
    }
    for (asio::steady_timer* timer : timers) {
        timer->cancel(); // 返回取消的等待数，无需 error_code
    }
}

void io_context_pool::wake_wait_stop() {
    asio::post(*io_contexts_[0], [this]() {
        if (this->wait_stop_timer_) {
            this->wait_stop_timer_->cancel(); // 返回取消的等待数，无需 error_code
            this->wait_stop_timer_.reset();
        }
    });
}

} // namespace network
