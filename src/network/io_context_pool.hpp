#pragma once

#include <asio.hpp>
#include <cstdint>
#include <list>
#include <memory>
#include <stdexcept>
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

    /// Stop all io_context objects in the pool.
    void stop();

    /// Get an io_context to use.
    asio::io_context& get_io_context();

private:
    io_context_pool(const io_context_pool&)            = delete;
    io_context_pool& operator=(const io_context_pool&) = delete;
    typedef std::shared_ptr<asio::io_context> io_context_ptr;
    typedef asio::executor_work_guard<asio::io_context::executor_type> io_context_work;

    /// The pool of io_contexts.
    std::vector<io_context_ptr> io_contexts_;

    /// The work that keeps the io_contexts running.
    std::list<io_context_work> work_;

    /// The next io_context to use for a connection.
    std::size_t next_io_context_;
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
