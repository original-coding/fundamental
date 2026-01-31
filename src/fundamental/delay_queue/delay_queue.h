#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

namespace Fundamental
{
class Timer {

public:
    // Type of time scales
    enum class TimeScale : std::uint8_t
    {
        Second = 0,
        Millisecond
    };

public:
    // Start timer immediately
    Timer();

    // Starts /resets the high resolution timer.
    void Reset();

    // Get duration from start time to now.
    template <TimeScale TimeScaleValue = TimeScale::Second>
    double GetDuration() const;

    // GetTimestamp from 1970
    template <class ChronoTimeType = std::chrono::milliseconds, typename ClockType = std::chrono::steady_clock>
    inline static std::int64_t GetTimeNow() {
        auto timePoint = ClockType::now();
        return std::chrono::duration_cast<ChronoTimeType>(timePoint.time_since_epoch()).count();
    }

    static std::string GetTimeStr(const char* format = "%F %T");
    // t should be convert to seconds
    static std::string ToTimeStr(std::time_t t, const char* format = "%F %T");

private:
    std::chrono::steady_clock::time_point m_previousTime;
    mutable std::mutex m_timePointMutex;
};
namespace details
{
struct DelayTaskSession;
}
class DelayQueue {
    struct Imp;

public:
    class Handle_ : public std::weak_ptr<details::DelayTaskSession> {
    private:
        using super = std::weak_ptr<details::DelayTaskSession>;

    public:
        using super::super;

        operator bool() const noexcept {
            return !expired();
        }
    };
    using HandleType = Handle_;
    using TaskType   = std::function<void()>;

public:
    /*
     * add a delay task
     * intervalMs:task delay proceed time delay msec
     * task : function that will be proceed when task expire
     * isSingle : the task will be proceed once by once when isSingle is false
     * autoManager : when delay task is stopped, it will be released
     * Notice! task won't start automatically
     */
    HandleType AddDelayTask(std::int64_t intervalMs,
                            const TaskType& task,
                            bool isSingle    = false,
                            bool autoManager = true);
    /*
     * start a delay task
     * return false when handle is invalid
     */
    bool StartDelayTask(HandleType handle);
    /*
     * stop a delay task
     * return false when handle is invalid
     */
    bool StopDelayTask(HandleType handle);
    /*
     * update a task interval
     * return false when handle is invalid
     */
    bool UpdateTaskInterval(HandleType handle, std::int64_t intervalMs);
    /*
     * modify a task with timepoint update
     * return false when the handle is invalid or the task is not working
     */
    bool ModifyTaskNextExpiredTimepoint(HandleType handle, std::int64_t modify_value_ms);
    /*
     * restart a delay task
     * return false when handle is invalid
     */
    bool RestartDelayTask(HandleType handle);
    /*
     * remove a delay task
     * you must release the delay task when you add a task with the option 'autoManager==false'
     */
    void RemoveDelayTask(HandleType handle);
    /*
     * check a handle is valid
     * return false when the handle is invalid
     */
    bool Validate(HandleType handle);
    /*
     * check a delay task is working
     * return false when the handle is invalid or not working
     */
    bool IsWorking(HandleType handle);

    /*
     * get net timeout msec
     */
    std::int64_t GetNextTimeoutMsec() const;
    /*
     * fllush delay task status
     */
    void HandleEvent();

    /*
     * this callback will be called when delay queue has been modified
     */
    void SetStateChangedCallback(const std::function<void()>& cb);

public:
    DelayQueue();
    ~DelayQueue();
    // disable copy
    DelayQueue(const DelayQueue&)            = delete;
    DelayQueue& operator=(const DelayQueue&) = delete;

private:
    Imp* pImp = nullptr;
};
} // namespace Fundamental