#pragma once
#include "fundamental/basic/error_code.hpp"
#include "fundamental/basic/utils.hpp"
#include "readerwritercircularbuffer.h"

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <thread>

namespace Fundamental
{
// only one thread is supported as the producer and one thread as the consumer.
class step_task_queue_executor : NonCopyable {
public:
    // 100ms
    constexpr static std::int64_t kTimeOutUsec = 100000;

public:
    enum class executor_errors : std::int32_t
    {
        success                = 0,
        canceled               = 1,
        throw_std_exception    = 2,
        throw_unknow_exception = 3,
    };

    class executor_category : public std::error_category, public Fundamental::Singleton<executor_category> {
    public:
        const char* name() const noexcept override {
            return "executor";
        }
        std::string message(int value) const override {
            switch (static_cast<executor_errors>(value)) {
            case executor_errors::success: return "success";
            case executor_errors::canceled: return "operation canceled";
            case executor_errors::throw_std_exception: return "throw std::exception";
            case executor_errors::throw_unknow_exception: return "throw unknow exception";
            default: return "executor_error";
            }
        }
    };

    inline static std::error_code make_error_code(executor_errors e) {
        return std::error_code(static_cast<int>(e), executor_category::Instance());
    }

public:
    using task_t = std::function<error_code()>;
    step_task_queue_executor(std::size_t max_ele_nums = 32) : data(max_ele_nums) {
    }
    ~step_task_queue_executor() {
        call_phase_check();
    }
    // this function will block the calling thread
    void run() {
        if (running_flag.test_and_set()) {
            throw std::runtime_error("try not call run twice or call run after  join called");
        }
        has_started.exchange(true);
        run_future.set_value();
        Fundamental::ScopeGuard release_g([&]() {
            has_finished.exchange(true);
            running_flag.clear();
        });
        try {
            task_t t;
            while (true) {
                t        = nullptr;
                auto ret = data.wait_dequeue_timed(t, kTimeOutUsec);
                if (!ret) {
                    // request aborted with none task
                    if (is_request_aborted) break;
                    continue;
                }
                if (is_request_aborted) {
                    // a valid task will be skipped
                    if (t) {
                        final_ec = error_code(make_error_code(executor_errors::canceled));
                    }
                    break;
                }
                // normally exit
                if (!t) {
                    break;
                }
                final_ec = t();
                if (final_ec) {
                    break;
                }
            }
        } catch (const std::exception& e) {
            final_ec = error_code(make_error_code(executor_errors::throw_std_exception), e.what());
        } catch (...) {
            final_ec = error_code(make_error_code(executor_errors::throw_unknow_exception));
        }
    }

    bool push_task(task_t&& task) {
        while (true) {
            if (is_request_aborted || has_finished) {
                break;
            }

            auto ret = data.wait_enqueue_timed(std::forward<task_t>(task), kTimeOutUsec);
            if (ret) return true;
        }
        return false;
    }
    bool push_task(const task_t& task) {
        while (true) {
            if (is_request_aborted || has_finished) {
                break;
            }
            auto ret = data.wait_enqueue_timed(task, kTimeOutUsec);
            if (ret) return true;
        }
        return false;
    }

    void abort() {
        if (has_finished) return;
        is_request_aborted.exchange(true);
    }
    // you should make sure that you have called the run function
    error_code join() {
        run_future.get_future().wait();
        {
            auto expected_value = false;
            // join has called
            if (!has_joined.compare_exchange_strong(expected_value, true)) {
                throw std::runtime_error("try not call join twice");
            }
        }
        if (has_finished) return final_ec;
        // push a invalid task
        push_task(nullptr);
        while (running_flag.test_and_set()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return final_ec;
    }

private:
    void call_phase_check() {
        if (has_started) return;
        if (!has_joined) {
            join();
        }
    }

private:
    std::promise<void> run_future;
    error_code final_ec;
    std::atomic_bool is_request_aborted = false;
    std::atomic_bool has_joined         = false;
    std::atomic_bool has_started        = false;
    std::atomic_bool has_finished       = false;
    std::atomic_flag running_flag       = ATOMIC_FLAG_INIT;

    Fundamental::BlockingReaderWriterCircularBuffer<task_t> data;
};
} // namespace Fundamental