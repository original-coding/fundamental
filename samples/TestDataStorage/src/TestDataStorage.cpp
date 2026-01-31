
#include "fundamental/basic/log.h"
#include "fundamental/data_storage/data_storage.hpp"
#include "fundamental/delay_queue/delay_queue.h"
#include <condition_variable>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
using namespace Fundamental;
DelayQueue queue;
bool exit_flag = false;
std::mutex mutex;
std::condition_variable cv;
void WakeUp() {
    std::scoped_lock<std::mutex> locker(mutex);
    cv.notify_one();
}
TEST(data_storage_test, basic) {
    memory_storage storage(&queue);
    std::string test_table = "table";
    std::string test_key   = "key";
    std::string test_key2  = "key2";
    std::string test_data  = "data";
    storage_config test_config;
    test_config.expired_time_msec = 40;
    storage.expired_signal().Connect([](std::string_view table, std::string_view key) -> Fundamental::SignalBrokenType {
        FINFO("table:{} key:{} is expired", table, key);
        return Fundamental::SignalBrokenType(false);
    });
    EXPECT_TRUE(storage.persist_data(test_table, test_key, test_data, test_config));
    EXPECT_TRUE(storage.persist_data(test_table, test_key2, test_data, test_config));
    EXPECT_TRUE(storage.update_key_expired_time(test_table, test_key, -15));
    EXPECT_TRUE(storage.update_key_expired_time(test_table, test_key2, 15));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(storage.table_size(test_table) == 1);
    EXPECT_TRUE(storage.has_key(test_table, test_key2));
    EXPECT_TRUE(!storage.has_key(test_table, test_key));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(storage.table_size(test_table) == 0);
}

TEST(data_storage_test, set_test) {
    memory_storage<void> storage(&queue);
    std::string test_table = "table";
    std::string test_key   = "key";
    std::string test_key2  = "key2";
    storage_config test_config;
    test_config.expired_time_msec = 15;
    storage.expired_signal().Connect([](std::string_view table, std::string_view key) -> Fundamental::SignalBrokenType {
        FINFO("table:{} key:{} is expired", table, key);
        return Fundamental::SignalBrokenType(false);
    });
    EXPECT_TRUE(storage.persist_data(test_table, test_key, test_config));
    EXPECT_TRUE(storage.persist_data(test_table, test_key2, test_config));
    EXPECT_TRUE(storage.update_key_expired_time(test_table, test_key, -5));
    EXPECT_TRUE(storage.update_key_expired_time(test_table, test_key2, 5));
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
    EXPECT_TRUE(storage.table_size(test_table) == 1);
    EXPECT_TRUE(storage.has_key(test_table, test_key2));
    EXPECT_TRUE(!storage.has_key(test_table, test_key));
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
    EXPECT_TRUE(storage.table_size(test_table) == 0);
}

TEST(data_storage_test, shared_ptr_remove_test) {
    struct TestItem {
        TestItem(int& ref) : ref(ref) {
            ++ref;
        }
        ~TestItem() {
            --ref;
            FINFO("release Test item");
        }
        int& ref;
    };

    memory_storage<std::shared_ptr<TestItem>> storage(&queue);
    std::string test_table = "table";
    std::string test_key   = "key";
    std::int32_t ref       = 0;
    {
        storage_config test_config;

        auto data                     = std::make_shared<TestItem>(ref);
        test_config.remove_cb         = [data]() { FINFO("data removed"); };
        test_config.expired_time_msec = 5;
        storage.expired_signal().Connect(
            [](std::string_view table, std::string_view key) -> Fundamental::SignalBrokenType {
                FINFO("table:{} key:{} is expired", table, key);
                return Fundamental::SignalBrokenType(false);
            });
        EXPECT_TRUE(storage.persist_data(test_table, test_key, std::move(data), test_config));
    }

    EXPECT_TRUE(storage.table_size(test_table) == 1);
    EXPECT_EQ(ref, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    EXPECT_TRUE(storage.table_size(test_table) == 0);
    EXPECT_EQ(ref, 0);
}

TEST(data_storage_test, test_iterator) {

    memory_storage<std::int32_t> storage(&queue);
    std::string test_table = "table";
    std::int32_t test_cnt  = 10;
    std::set<std::int32_t> dic;
    {
        storage_config test_config;
        for (std::int32_t i = 0; i < test_cnt; ++i) {
            dic.insert(i);
            test_config.expired_time_msec = 0;
            EXPECT_TRUE(storage.persist_data(test_table, std::to_string(i), i, test_config));
        }
    }
    {
        auto copy = dic;
        auto iter = storage.begin();
        while (iter != storage.end()) {
            if (iter->first == test_table) break;
        }
        EXPECT_TRUE(iter != storage.end());
        auto& data_table = iter->second;
        for (auto& data : data_table) {
            EXPECT_TRUE(data.first == std::to_string(data.second.data));
            EXPECT_TRUE(copy.erase(data.second.data) == 1);
        }
        EXPECT_TRUE(copy.empty());
    }

    {
        auto copy = dic;
        auto iter = storage.cbegin();
        while (iter != storage.cend()) {
            if (iter->first == test_table) break;
        }
        EXPECT_TRUE(iter != storage.cend());
        auto& data_table = iter->second;
        for (auto& data : data_table) {
            EXPECT_TRUE(data.first == std::to_string(data.second.data));
            EXPECT_TRUE(copy.erase(data.second.data) == 1);
        }
        EXPECT_TRUE(copy.empty());
    }

    {
        auto copy = dic;
        auto iter = storage.find(test_table);
        EXPECT_TRUE(iter != storage.end());
        auto& data_table = iter->second;
        for (auto& data : data_table) {
            EXPECT_TRUE(data.first == std::to_string(data.second.data));
            EXPECT_TRUE(copy.erase(data.second.data) == 1);
        }
        EXPECT_TRUE(copy.empty());
    }
    {
        const auto & ref=storage;
        auto copy = dic;
        auto iter = ref.find(test_table);
        static_assert(std::is_same_v<decltype(iter),decltype(storage.cend())>);
        EXPECT_TRUE(iter != storage.cend());
        auto& data_table = iter->second;
        for (auto& data : data_table) {
            EXPECT_TRUE(data.first == std::to_string(data.second.data));
            EXPECT_TRUE(copy.erase(data.second.data) == 1);
        }
        EXPECT_TRUE(copy.empty());
    }
}

int main(int argc, char* argv[]) {
    queue.SetStateChangedCallback([]() { WakeUp(); });
    std::thread t([&]() {
        while (!exit_flag) {
            queue.HandleEvent();
            auto next_timeout = queue.GetNextTimeoutMsec();
            if (next_timeout < 0) next_timeout = 1;
            std::unique_lock<std::mutex> locker(mutex);
            cv.wait_for(locker, std::chrono::milliseconds(next_timeout), [&]() { return !exit_flag; });
        }
    });
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel         = Fundamental::LogLevel::debug;
    options.logFormat            = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    options.logOutputProgramName = "test";
    options.logOutputPath        = "output";
    Fundamental::Logger::Initialize(std::move(options));
    ::testing::InitGoogleTest(&argc, argv);

    auto ret  = RUN_ALL_TESTS();
    exit_flag = true;
    WakeUp();
    t.join();
    return ret;
}
