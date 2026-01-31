
#include "fundamental/algorithm/range_set.hpp"
#include "fundamental/basic/log.h"
#include <condition_variable>
#include <gtest/gtest.h>
#include <memory>
#include <thread>

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
