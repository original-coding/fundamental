
#ifndef FORCE_TIME_TRACKER
    #define FORCE_TIME_TRACKER 1
#endif

#include "fundamental/basic/log.h"
#include "fundamental/tracker/time_tracker.hpp"
#include <iostream>

using Fundamental::TimeTracker;
void TestBasic()
{
    std::string tag = "basic";
    {
        using Type = TimeTracker<std::chrono::nanoseconds>;
        DeclareTimeTacker(Type, t, tag, "test", 10, true, nullptr);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    {
        using Type = TimeTracker<std::chrono::microseconds>;
        DeclareTimeTacker(Type, t, tag, "test", 10, true, nullptr);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    {
        using Type = TimeTracker<std::chrono::milliseconds>;
        DeclareTimeTacker(Type, t, tag, "test", 10, true, nullptr);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    {
        using Type = TimeTracker<std::chrono::seconds>;
        DeclareTimeTacker(Type, t, tag, "test", 10, true, nullptr);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    {
        using Type = TimeTracker<std::chrono::minutes>;
        DeclareTimeTacker(Type, t, tag, "test", 10, true, nullptr);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    {
        using Type = TimeTracker<std::chrono::hours>;
        DeclareTimeTacker(Type, t, tag, "test", 10, true, nullptr);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    {
        using Type = TimeTracker<std::chrono::hours>;
        DeclareTimeTacker(Type, t, tag, "test", 10, false, nullptr);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void TestOutputer()
{
    std::string tag = "outputer";
    {
        using Type = TimeTracker<std::chrono::seconds>;
        DeclareTimeTacker(Type, t, tag, "test", 10, true, [](const std::string_view& s) {
            std::cout << "outputer ->>>>>>" << s << std::endl;
        });
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void TestReset()
{
    std::string tag = "reset";
    {
        using Type = TimeTracker<std::chrono::milliseconds>;
        DeclareTimeTacker(Type, t, tag, "test", 10, true, nullptr);
        std::this_thread::sleep_for(Type::ChronoTimeType(1));
        StopTimeTracker(t);
        RestartTimeTracker(t);
        std::this_thread::sleep_for(Type::ChronoTimeType(11));
        StopTimeTracker(t);
        std::cout << "leave scope:should not print any more" << std::endl;
    }
}

int main(int argc, char** argv)
{
    TestBasic();
    TestOutputer();
    TestReset();

    return 0;
}
