
#include "fundamental/algorithm/range_set.hpp"
#include "fundamental/basic/log.h"
#include <condition_variable>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
using namespace Fundamental;

TEST(test_algorithm, range) {
    algorithm::range_set<std::size_t> set;
    {
        auto ret = set.range_emplace(1, 2);
        EXPECT_TRUE(ret.second && ret.first != set.end());
    }
    {
        auto ret = set.range_emplace(2, 6);
        EXPECT_TRUE(ret.second && ret.first != set.end());
        EXPECT_EQ(ret.first->low,1);
        EXPECT_EQ(ret.first->up,6);
    }
    EXPECT_TRUE(set.range_emplace(10, 15).second);
    EXPECT_EQ(set.size(), 2);
    EXPECT_FALSE(set.range_emplace(1, 5).second);
    EXPECT_TRUE(set.range_remove(1, 2));
    EXPECT_FALSE(set.range_remove(1, 2));
    EXPECT_TRUE(set.range_remove(11, 12));
    EXPECT_EQ(set.size(), 3);
}

int main(int argc, char* argv[]) {
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel         = Fundamental::LogLevel::debug;
    options.logFormat            = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    options.logOutputProgramName = "test";
    options.logOutputPath        = "output";
    Fundamental::Logger::Initialize(std::move(options));
    ::testing::InitGoogleTest(&argc, argv);
    auto ret = RUN_ALL_TESTS();
    return ret;
}
