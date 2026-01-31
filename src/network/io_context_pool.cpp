#include "io_context_pool.hpp"
#include "fundamental/application/application.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
#include "fundamental/thread_pool/thread_pool.h"

#include <future>
#include <stdexcept>
#include <thread>
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

    auto& threadpool = Fundamental::ThreadPool::BlockTimePool();
    // enqueue all of the io_contexts run task
    for (std::size_t i = 0; i < io_contexts_.size(); ++i) {
        threadpool.Spawn(1);
        threadpool.Enqueue([this, i] {
            Fundamental::Utils::SetThreadName(Fundamental::StringFormat("io_loop_{}", i));
            try {
                io_contexts_[i]->run();
            } catch (const std::exception& e) {
                FERR("asio context err:{}", e.what());
                FASSERT(false, "asio context error please check your code logic,maybe some memory access error exist");
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
    FDEBUG("try stop io context pool");
    // wait all other task finished
    work_.clear();
    std::error_code ec;
    signals_.cancel(ec);
}

asio::io_context& io_context_pool::get_io_context() {
    // Use a round-robin scheme to choose the next io_context to use.
    asio::io_context& io_context = *io_contexts_[next_io_context_];
    ++next_io_context_;
    if (next_io_context_ == io_contexts_.size()) next_io_context_ = 0;
    return io_context;
}

} // namespace network
