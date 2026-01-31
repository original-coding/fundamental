
#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"
#include "step_task_queue_executor.hpp"

#include <gtest/gtest.h>
TEST(step_task_queue_executor, basic) {
    Fundamental::step_task_queue_executor executor(1);
    std::thread t([&]() { executor.run(); });
    auto now = Fundamental::Timer::GetTimeNow();
    executor.push_task([]() -> Fundamental::error_code {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return {};
    });
    // this call will not block for too long
    executor.push_task([]() -> Fundamental::error_code { return {}; });
    EXPECT_GE(now + 2, Fundamental::Timer::GetTimeNow());
    // this task will block for 10 msec
    executor.push_task([]() -> Fundamental::error_code { return {}; });
    EXPECT_LE(now + 10, Fundamental::Timer::GetTimeNow());
    std::this_thread::sleep_for(std::chrono::microseconds(Fundamental::step_task_queue_executor::kTimeOutUsec));
    EXPECT_TRUE(executor.push_task([]() -> Fundamental::error_code { return {}; }));
    EXPECT_TRUE(!executor.join());
    t.join();
}

TEST(step_task_queue_executor, abort_test) {
    Fundamental::step_task_queue_executor executor(2);
    std::thread t([&]() { executor.run(); });
    std::size_t push_cnt               = 2;
    std::atomic<std::size_t> final_num = 0;
    auto task_func                     = [&]() -> Fundamental::error_code {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        final_num.fetch_add(1);
        return {};
    };

    for (std::size_t i = 0; i < push_cnt; ++i) {
        executor.push_task(task_func);
    }
    std::thread abort_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        executor.abort();
    });
    auto v = executor.join();
    EXPECT_EQ(v.value(), static_cast<std::int32_t>(Fundamental::step_task_queue_executor::executor_errors::canceled));
    EXPECT_LE(final_num.load(), 1);
    t.join();
    abort_thread.join();
}

TEST(step_task_queue_executor, error_code_test) {

    { // test phase abort following task
        Fundamental::step_task_queue_executor executor;
        std::thread t([&]() { executor.run(); });
        executor.push_task([&]() -> Fundamental::error_code {
            return Fundamental::error_code(std::make_error_code(std::errc::bad_file_descriptor));
        });
        executor.push_task([&]() -> Fundamental::error_code {
            return Fundamental::error_code(std::make_error_code(std::errc::already_connected));
        });
        EXPECT_EQ(executor.join().value(), static_cast<std::int32_t>(std::errc::bad_file_descriptor));
        t.join();
    }
    { // test std::exception
        Fundamental::step_task_queue_executor executor;
        std::thread t([&]() { executor.run(); });
        std::string error_msg = "test";
        executor.push_task([&]() -> Fundamental::error_code {
            throw std::runtime_error(error_msg);
            return Fundamental::error_code(std::make_error_code(std::errc::bad_file_descriptor));
        });
        auto v = executor.join();
        EXPECT_EQ(v.value(), static_cast<std::int32_t>(
                                 Fundamental::step_task_queue_executor::executor_errors::throw_std_exception));
        EXPECT_EQ(v.details(), error_msg);
        t.join();
    }
    { // test normal
        Fundamental::step_task_queue_executor executor;
        std::thread t([&]() { executor.run(); });
        executor.push_task([&]() -> Fundamental::error_code {
            throw 1;
            return Fundamental::error_code(std::make_error_code(std::errc::bad_file_descriptor));
        });
        auto v = executor.join();
        EXPECT_EQ(v.value(), static_cast<std::int32_t>(
                                 Fundamental::step_task_queue_executor::executor_errors::throw_unknow_exception));
        EXPECT_EQ(v.details(), "");
        t.join();
    }
}

int main(int argc, char** argv) {

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}