
#include "fundamental/basic/log.h"

#include "fundamental/thread_pool/thread_pool.h"
#include "fundamental/thread_pool/thread_utils.hpp"

#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
//windows platform start slower a new thread 
#if TARGET_PLATFORM_WINDOWS
    #define TEST_DELAY_TIME_MS 500
    #define TEST_THREAD_CNT 100
#else
    #define TEST_DELAY_TIME_MS 40
    #define TEST_THREAD_CNT    10000
#endif
using namespace Fundamental;
TEST(thread_pool_test, default_action) {
    ThreadPoolConfig config;
    config.max_threads_limit    = 2;
    config.min_work_threads_num = 0;
    config.enable_auto_scaling  = true;
    config.ilde_wait_time_ms    = 20;
    {
        auto& pool = ThreadPool::DefaultPool();
        EXPECT_FALSE(pool.InitThreadPool(config));
        pool.Spawn(2);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(ThreadPoolConfig::kDefaultIdleWaitTimeMsec + TEST_DELAY_TIME_MS));
        EXPECT_EQ(pool.Count(), 1);
    }
    {
        auto& pool = ThreadPool::Instance<10000>();
        pool.Spawn(ThreadPoolConfig::normal_thread_num_limit() + 1);
        EXPECT_EQ(pool.Count(), ThreadPoolConfig::normal_thread_num_limit());
        std::this_thread::sleep_for(
            std::chrono::milliseconds(ThreadPoolConfig::kDefaultIdleWaitTimeMsec + TEST_DELAY_TIME_MS));
        EXPECT_EQ(pool.Count(), 0);
    }
    {
        auto& pool = ThreadPool::BlockTimePool();
        pool.Spawn(ThreadPoolConfig::normal_thread_num_limit() + 2);
        EXPECT_EQ(pool.Count(), ThreadPoolConfig::normal_thread_num_limit() + 2);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(ThreadPoolConfig::kDefaultIdleWaitTimeMsec + TEST_DELAY_TIME_MS));
        EXPECT_EQ(pool.Count(), ThreadPoolConfig::normal_thread_num_limit() + 2);
    }
    // test nocopyable data support
    {
        auto data1 = std::make_unique<std::int32_t>(0);
        auto data2 = std::make_unique<std::int32_t>(1);
        ThreadPool::DefaultPool().Enqueue(
            [data1 = std::move(data1)](std::unique_ptr<std::int32_t> data2) mutable -> void {}, std::move(data2));
    }
}
#if 1
TEST(thread_pool_test, basic) {
    ThreadPool pool;
    ThreadPoolConfig config;
    config.max_threads_limit    = 2;
    config.min_work_threads_num = 1;
    config.enable_auto_scaling  = true;
    config.ilde_wait_time_ms    = 20;
    EXPECT_TRUE(pool.InitThreadPool(config));
    EXPECT_FALSE(pool.InitThreadPool(config));
    pool.Spawn(10);
    EXPECT_EQ(pool.Count(), 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(TEST_DELAY_TIME_MS));
    // threads should be released by idle
    EXPECT_EQ(pool.Count(), config.min_work_threads_num);
}

TEST(thread_pool_test, test_scaling_join) {
    ThreadPool pool;
    ThreadPoolConfig config;
    config.max_threads_limit    = 2;
    config.min_work_threads_num = 0;
    config.enable_auto_scaling  = true;
    config.ilde_wait_time_ms    = 20;
    pool.InitThreadPool(config);
    pool.Spawn(10);
    EXPECT_EQ(pool.Count(), 2);
    EXPECT_EQ(pool.Join(), config.max_threads_limit);
}

TEST(thread_pool_test, test_auto_scaling) {
    ThreadPool pool;
    ThreadPoolConfig config;
    config.max_threads_limit    = Fundamental::hardware_concurrency();
    config.min_work_threads_num = 0;
    config.enable_auto_scaling  = true;
    config.ilde_wait_time_ms    = 2;
    pool.InitThreadPool(config);
    auto taskFunc        = [&]() {};
    std::size_t test_cnt = 1000;
    std::size_t index    = 0;
    while (index < test_cnt) {
        ++index;
        pool.Enqueue(taskFunc);
    }
    // wait task finished
    while (pool.PendingTasks() != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(TEST_DELAY_TIME_MS));
    // threads should be released by idle
    EXPECT_EQ(pool.Count(), 0);
}

TEST(thread_pool_test, test_join_exception) {
    ThreadPool pool;
    ThreadPoolConfig config;
    config.max_threads_limit = Fundamental::hardware_concurrency();
    // 0 means no limit
    config.min_work_threads_num = 1;
    config.enable_auto_scaling  = true;
    config.ilde_wait_time_ms    = 10;
    pool.InitThreadPool(config);
    auto taskFunc        = [&]() { EXPECT_TRUE(pool.InThreadPool()); };
    std::size_t test_cnt = 100;
    std::size_t index    = 0;
    while (index < test_cnt) {
        ++index;
        pool.Enqueue(taskFunc);
    }
    pool.Enqueue([&]() { EXPECT_ANY_THROW(pool.Join()); });
    while (pool.PendingTasks() != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pool.Join();
}

TEST(thread_pool_test, test_wait_all_task_finished) {

    ThreadPoolConfig config;
    config.max_threads_limit = 10;
    // 0 means no limit
    config.min_work_threads_num = 1;
    config.enable_auto_scaling  = true;
    config.ilde_wait_time_ms    = 2;
    std::size_t test_cnt        = 100;
    {
        ThreadPool pool;
        pool.InitThreadPool(config);
        std::size_t index                   = 0;
        std::atomic<std::size_t> finish_cnt = 0;
        while (index < test_cnt) {
            ++index;
            pool.Enqueue([&]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                ++finish_cnt;
            });
        }
        EXPECT_TRUE(pool.WaitAllTaskFinished());
        EXPECT_EQ(finish_cnt.load(), test_cnt);
        EXPECT_TRUE(pool.PendingTasks() == 0);
    }
}
TEST(thread_pool_test, test_wait_idle_thread) {

    ThreadPoolConfig config;
    config.max_threads_limit = 10;
    // 0 means no limit
    config.min_work_threads_num = 1;
    config.enable_auto_scaling  = true;
    config.ilde_wait_time_ms    = 2000;
    std::size_t test_cnt        = 20;
    {
        ThreadPool pool;
        pool.InitThreadPool(config);
        std::size_t index                   = 0;
        std::atomic<std::size_t> finish_cnt = 0;
        while (index < test_cnt) {
            ++index;
            pool.Enqueue([&, index = index]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(2 + index * 2));
                ++finish_cnt;
            });
        }
        for (std::size_t i = 1; i < config.max_threads_limit; ++i) {
            EXPECT_TRUE(pool.WaitIdleThread(i));
            EXPECT_TRUE(pool.PendingTasks() == 0);
            EXPECT_LE((pool.ProcessingTasks() + i), config.max_threads_limit);
        }
    }
}

TEST(thread_pool_test, test_finish_wait) {

    ThreadPoolConfig config;
    config.max_threads_limit = 10;
    // 0 means no limit
    config.min_work_threads_num = 1;
    config.enable_auto_scaling  = true;
    config.ilde_wait_time_ms    = 2;
    std::size_t test_cnt        = 1000;
    {
        ThreadPool pool;
        pool.InitThreadPool(config);

        std::size_t index                   = 0;
        std::atomic<std::size_t> finish_cnt = 0;
        while (index < test_cnt) {
            ++index;
            pool.Schedule(std::chrono::milliseconds(1), [&]() { ++finish_cnt; });
        }
        pool.Join();
        EXPECT_TRUE(pool.PendingTasks() == 0);
        EXPECT_LE(finish_cnt.load(), test_cnt);
    }
    {
        ThreadPool pool;
        pool.InitThreadPool(config);
        std::size_t index                   = 0;
        std::atomic<std::size_t> finish_cnt = 0;
        while (index < test_cnt) {
            ++index;
            pool.Schedule(std::chrono::milliseconds(1), [&]() { ++finish_cnt; });
        }
        EXPECT_TRUE(pool.WaitAllTaskFinished());
        pool.Join();
        EXPECT_TRUE(pool.PendingTasks() == 0);
        EXPECT_EQ(finish_cnt.load(), test_cnt);
    }
}

TEST(thread_pool_test, thread_sig_test) {
    std::size_t test_cnt = TEST_THREAD_CNT;
    while (test_cnt-- > 0) {
        std::thread t([]() { std::this_thread::sleep_for(std::chrono::microseconds(10)); });
        t.join();
    }
}

TEST(thread_pool_test, thread_pool_sig_test) {
    std::size_t test_cnt = TEST_THREAD_CNT;
    while (test_cnt-- > 0) {
        ThreadPool::DefaultPool()
            .Enqueue([]() { std::this_thread::sleep_for(std::chrono::microseconds(10)); })
            .resultFuture.get();
    }
}

TEST(thread_pool_test, thread_pool_multi_test) {
    ThreadPoolConfig config;
    config.max_threads_limit = Fundamental::hardware_concurrency();
    // 0 means no limit
    config.min_work_threads_num = 0;
    config.enable_auto_scaling  = true;
    config.ilde_wait_time_ms    = 2;
    std::size_t test_cnt        = TEST_THREAD_CNT;
    ThreadPool pool;
    pool.InitThreadPool(config);
    pool.Spawn(config.max_threads_limit);
    for (std::size_t i = 0; i < test_cnt; ++i) {
        pool.Enqueue([]() { std::this_thread::sleep_for(std::chrono::microseconds(10)); });
    }
    pool.WaitAllTaskFinished();
}

TEST(thread_pool_test, thread_sig_test_empty) {
    std::size_t test_cnt = TEST_THREAD_CNT;
    while (test_cnt-- > 0) {
        std::thread t([]() {});
        t.join();
    }
}

TEST(thread_pool_test, thread_pool_sig_test_empty) {
    std::size_t test_cnt = TEST_THREAD_CNT;
    while (test_cnt-- > 0) {
        ThreadPool::DefaultPool().Enqueue([]() {}).resultFuture.get();
    }
}

TEST(thread_pool_test, thread_pool_multi_test_empty) {
    ThreadPoolConfig config;
    config.max_threads_limit = Fundamental::hardware_concurrency();
    // 0 means no limit
    config.min_work_threads_num = 0;
    config.enable_auto_scaling  = true;
    config.ilde_wait_time_ms    = 2;
    std::size_t test_cnt        = TEST_THREAD_CNT;
    ThreadPool pool;
    pool.InitThreadPool(config);
    pool.Spawn(config.max_threads_limit);
    for (std::size_t i = 0; i < test_cnt; ++i) {
        pool.Enqueue([]() {});
    }
    pool.WaitAllTaskFinished();
}
#endif
int main(int argc, char** argv) {
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel         = Fundamental::LogLevel::debug;
    options.logFormat            = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    options.logOutputProgramName = "test";
    options.logOutputPath        = "output";
    Fundamental::Logger::Initialize(std::move(options));
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
