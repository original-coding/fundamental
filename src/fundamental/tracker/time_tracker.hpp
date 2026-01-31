#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace Fundamental {

template <typename T, typename... Types>
struct is_one_of : std::disjunction<std::is_same<T, Types>...> {};
template <typename T>
inline constexpr bool check_support_chrono_v =
    is_one_of<T, std::chrono::nanoseconds, std::chrono::microseconds, std::chrono::milliseconds, std::chrono::seconds,
              std::chrono::minutes, std::chrono::hours>::value;

template <typename ChronoTimeType_>
struct TimeTrackerUnit {
    using ChronoTimeType = ChronoTimeType_;
    static std::string_view Unit() {
        static_assert(check_support_chrono_v<ChronoTimeType>, "unsupport type for cplusplus 17");
        if constexpr (std::is_same_v<ChronoTimeType_, std::chrono::nanoseconds>) {
            return "ns";
        } else if constexpr (std::is_same_v<ChronoTimeType_, std::chrono::microseconds>) {
            return "us";
        } else if constexpr (std::is_same_v<ChronoTimeType_, std::chrono::milliseconds>) {
            return "ms";
        } else if constexpr (std::is_same_v<ChronoTimeType_, std::chrono::seconds>) {
            return "s";
        } else if constexpr (std::is_same_v<ChronoTimeType_, std::chrono::minutes>) {
            return "min";
        } else if constexpr (std::is_same_v<ChronoTimeType_, std::chrono::hours>) {
            return "h";
        } else {
            return "";
        }
    }
};

using TimeTrackerMsgOutputer = std::function<void(const std::string_view&)>;

template <typename ChronoTimeType_ = std::chrono::milliseconds, typename StringType = std::string_view>
struct TimeTracker {
    using ChronoTimeType = ChronoTimeType_;
    using UnitHelper     = TimeTrackerUnit<ChronoTimeType_>;
    explicit TimeTracker(StringType tag, StringType msg, std::int64_t warningThreshold = 0, bool enable_debug = false,
                         TimeTrackerMsgOutputer outputer = nullptr) :
    warningThreshold(warningThreshold), enable_debug(enable_debug), outputer(outputer), tag(tag), msg(msg) {
        ReStartTracker();
    }
    ~TimeTracker() {
        StopTracker();
    }
    void ReStartTracker() {
        m_previousTime = std::chrono::steady_clock::now();
        need_print_.exchange(true);
        if (enable_debug) {
            std::stringstream ss;
            ss << "timetracker [start " << tag << " threshold:" << warningThreshold << " " << UnitHelper::Unit() << "] "
               << msg;
            // c++20 can return a string_view
            auto pMsg = ss.str();
            if (outputer)
                outputer(pMsg);
            else {
                std::cout << pMsg << std::endl;
            }
        }
    }

    void StopTracker() {
        bool expected = true;
        if (need_print_.compare_exchange_strong(expected, false)) {
            auto elapsedTime       = std::chrono::steady_clock::now() - m_previousTime;
            std::int64_t time_diff = std::chrono::duration_cast<ChronoTimeType>(elapsedTime).count();
            double elapsedTimeSec  = std::chrono::duration_cast<std::chrono::duration<double>>(elapsedTime).count();
            std::string print_str;
            if (time_diff > warningThreshold) {
                std::stringstream ss;
                ss << "timetracker [warn " << tag << " " << time_diff << UnitHelper::Unit() << "{" << elapsedTimeSec
                   << TimeTrackerUnit<std::chrono::seconds>::Unit() << "}"
                   << ">" << warningThreshold << UnitHelper::Unit() << "] " << msg;
                // c++20 can return a string_view
                print_str = ss.str();
            } else if (enable_debug) {
                std::stringstream ss;
                ss << "timetracker [finish " << tag << " " << time_diff << UnitHelper::Unit() << "{" << elapsedTimeSec
                   << TimeTrackerUnit<std::chrono::seconds>::Unit() << "}"
                   << "<" << warningThreshold << UnitHelper::Unit() << "] " << msg;
                // c++20 can return a string_view
                print_str = ss.str();
            }
            if (!print_str.empty()) {
                if (outputer)
                    outputer(print_str);
                else {
                    std::cout << print_str << std::endl;
                }
            }
        }
    }
    const std::int64_t warningThreshold   = 0;
    const bool enable_debug               = false;
    const TimeTrackerMsgOutputer outputer = nullptr;
    std::chrono::steady_clock::time_point m_previousTime;
    std::atomic_bool need_print_ = true;
    StringType tag;
    StringType msg;
};
template <typename ChronoTimeType_ = std::chrono::milliseconds>
using STimeTracker = TimeTracker<ChronoTimeType_, std::string>;
} // namespace Fundamental
#if (defined(DEBUG) && !defined(NDEBUG))
    #pragma message("#####################ENABLE TIME TRACKER FOR DEBUG MODE####################")
#elif defined(FORCE_TIME_TRACKER)
    #pragma message("#####################FORCE TIME TRACKER ####################")
#endif

#if (defined(DEBUG) && !defined(NDEBUG)) || defined(FORCE_TIME_TRACKER)
    #define DeclareTimeTacker(type, name, tag, msg, threshold, enable_debug, outputer)                                 \
        type name(tag, msg, threshold, enable_debug, outputer)
    #define RestartTimeTracker(name) name.ReStartTracker()
    #define StopTimeTracker(name)    name.StopTracker()
#else
    #define DeclareTimeTacker(type, name, tag, msg, threshold, enable_debug, outputer) (void)0
    #define RestartTimeTracker(name)                                                   (void)0
    #define StopTimeTracker(name)                                                      (void)0
#endif