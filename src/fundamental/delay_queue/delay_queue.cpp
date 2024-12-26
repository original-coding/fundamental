#include "delay_queue.h"

#include <list>
#include <set>
namespace Fundamental {

Timer::Timer() {
    Reset();
}
void Timer::Reset() {
    std::scoped_lock lock(m_timePointMutex);
    m_previousTime = std::chrono::high_resolution_clock::now();
}

template <Timer::TimeScale TimeScaleValue>
double Timer::GetDuration() const {
    auto currentTime = std::chrono::high_resolution_clock::now();

    std::chrono::high_resolution_clock::time_point previousTime;
    {
        std::scoped_lock lock(m_timePointMutex);
        previousTime = m_previousTime;
    }

    auto elapsedTime = std::chrono::duration_cast<std::chrono::duration<double>>(currentTime - previousTime);

    if constexpr (TimeScaleValue == TimeScale::Millisecond) {
        return elapsedTime.count() * 1000;
    } else if constexpr (TimeScaleValue == TimeScale::Second) {
        return elapsedTime.count();
    } else {
        static_assert("TimeScale is not allowed !");
        return 0.0;
    }
}

// Explicit template initializations
template double Timer::GetDuration<Timer::TimeScale::Second>() const;
template double Timer::GetDuration<Timer::TimeScale::Millisecond>() const;

std::string Timer::GetTimeStr(const char* format) {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return ToTimeStr(t, format);
}

std::string Timer::ToTimeStr(std::time_t t, const char* format) {
    char timeStr[64] = { 0 };

#if TARGET_PLATFORM_LINUX || TARGET_PLATFORM_WINDOWS
    struct tm tmTemp;
    #if TARGET_PLATFORM_LINUX
    ::localtime_r(&t, &tmTemp);
    #else
    ::localtime_s(&tmTemp, &t);
    #endif
    std::strftime(timeStr, 64, format, &tmTemp);
#endif

    return timeStr;
}

using HandleType = DelayQueue::HandleType;

namespace details {
struct DelayTaskSession {
    void SetNextTimeout(std::int64_t currentTimePoint) {
        nextTimeout = currentTimePoint + interval;
    }

    DelayQueue::TaskType task;
    std::int64_t interval    = 0;
    std::int64_t nextTimeout = 0;
    bool bSingle             = false;
    bool bWorking            = false;
    bool bAutoRleased        = true;
};

} // namespace details

struct DelayQueue::Imp {
    inline HandleType AddDelayTask(std::int64_t intervalMs, const TaskType& task, bool isSingle = false,
                                   bool autoManager = true) {
        if (intervalMs < 0 || !task) return DelayQueue::kInvalidHandle;
        if (stateCb) stateCb();
        std::scoped_lock<std::mutex> locker(dataMutex);
        auto* session = new details::DelayTaskSession { task, intervalMs, 0, isSingle, false, autoManager };
        taskStorage.emplace(session);
        return reinterpret_cast<HandleType>(session);
    }

    inline bool StartDelayTask(HandleType handle) {
        if (stateCb) stateCb();
        std::scoped_lock<std::mutex> locker(dataMutex);
        if (!ValidateInternal(handle)) return false;
        auto* session = Cast(handle);
        if (session->bWorking) return true;
        session->bWorking = true;
        session->SetNextTimeout(Timer::GetTimeNow<std::chrono::milliseconds, std::chrono::steady_clock>());
        processingTasks.insert(std::make_pair(session->nextTimeout, handle));
        return true;
    }

    inline bool StopDelayTask(HandleType handle) {
        if (stateCb) stateCb();
        std::scoped_lock<std::mutex> locker(dataMutex);
        if (!ValidateInternal(handle)) return false;
        auto* session = Cast(handle);
        if (!session->bWorking) return true;
        session->bWorking = false;
        processingTasks.erase(std::make_pair(session->nextTimeout, handle));
        if (session->bAutoRleased) DoRemoveHandleInternal(handle);
        return true;
    }

    inline bool RestartDelayTask(HandleType handle) {
        std::scoped_lock<std::mutex> locker(dataMutex);
        if (!ValidateInternal(handle)) return false;
        auto* session = Cast(handle);
        processingTasks.erase(std::make_pair(session->nextTimeout, handle));
        session->bWorking = true;
        session->SetNextTimeout(Timer::GetTimeNow<std::chrono::milliseconds, std::chrono::steady_clock>());
        processingTasks.insert(std::make_pair(session->nextTimeout, handle));
        return true;
    }

    inline void RemoveDelayTask(HandleType handle) {
        if (handle == DelayQueue::kInvalidHandle) return;
        if (stateCb) stateCb();
        std::scoped_lock<std::mutex> locker(dataMutex);
        if (!ValidateInternal(handle)) return;
        DoRemoveHandleInternal(handle);
    }

    inline bool Validate(HandleType handle) {
        std::scoped_lock<std::mutex> locker(dataMutex);
        return ValidateInternal(handle);
    }

    inline bool IsWorking(HandleType handle) {
        std::scoped_lock<std::mutex> locker(dataMutex);
        if (!ValidateInternal(handle)) return false;
        auto* session = Cast(handle);
        return session->bWorking;
    }

    inline std::int64_t GetNextTimeoutMsec() {
        std::scoped_lock<std::mutex> locker(dataMutex);
        if (processingTasks.empty()) {
            return static_cast<std::int64_t>(~0);
        }
        return processingTasks.begin()->first -
               Timer::GetTimeNow<std::chrono::milliseconds, std::chrono::steady_clock>();
    }

    inline bool UpdateTaskInterval(HandleType handle, std::int64_t intervalMs) {
        std::scoped_lock<std::mutex> locker(dataMutex);
        if (!ValidateInternal(handle)) return false;
        auto* session     = Cast(handle);
        session->interval = intervalMs;
        return true;
    }

    inline void HandleEvent() {

        std::list<HandleType> expiredHandles;
        std::list<TaskType> expiredTasks;
        auto now = Timer::GetTimeNow<std::chrono::milliseconds, std::chrono::steady_clock>();
        {
            // take all  expired tasks
            std::lock_guard<std::mutex> locker(dataMutex);
            auto iter = processingTasks.begin();
            while (iter != processingTasks.end()) {
                if (iter->first <= now) {
                    // copy all expired handles
                    auto handle = iter->second;
                    expiredHandles.emplace_back(handle);
                    ++iter;
                } else {
                    break;
                }
            }
            processingTasks.erase(processingTasks.begin(), iter);
            while (!expiredHandles.empty()) {
                auto& handle  = expiredHandles.front();
                auto* session = Cast(handle);

                expiredTasks.emplace_back(session->task);
                if (session->bSingle) session->bWorking = false;
                if (session->bWorking) { // push back task for next cycle
                    session->SetNextTimeout(Timer::GetTimeNow<std::chrono::milliseconds, std::chrono::steady_clock>());
                    processingTasks.insert(std::make_pair(session->nextTimeout, handle));
                } else {
                    // release session
                    if (session->bAutoRleased) ReleaseSessionStorageInternal(session);
                }
                expiredHandles.pop_front();
            }
        }
        // process delay task with no mutex
        for (auto& i : expiredTasks)
            i();
    }
    // internal
    inline details::DelayTaskSession* Cast(HandleType handle) {
        return reinterpret_cast<details::DelayTaskSession*>(handle);
    }

    inline bool ValidateInternal(HandleType handle) {
        if (handle == DelayQueue::kInvalidHandle) return false;
        if (taskStorage.find(Cast(handle)) == taskStorage.end()) return false;
        return true;
    }

    inline void DoRemoveHandleInternal(HandleType handle) {
        auto* session = Cast(handle);
        processingTasks.erase(std::make_pair(session->nextTimeout, handle));
        ReleaseSessionStorageInternal(session);
    }

    inline void ReleaseSessionStorageInternal(details::DelayTaskSession* session) {
        taskStorage.erase(session);
        delete session;
    }
    std::function<void()> stateCb = nullptr;
    // data field
    std::mutex dataMutex;
    std::set<details::DelayTaskSession*> taskStorage;
    std::set<std::pair<std::int64_t, HandleType>> processingTasks;
};

HandleType DelayQueue::AddDelayTask(std::int64_t intervalMs, const TaskType& task, bool isSingle, bool autoManager) {
    return pImp->AddDelayTask(intervalMs, task, isSingle, autoManager);
}

bool DelayQueue::StartDelayTask(HandleType handle) {
    return pImp->StartDelayTask(handle);
}

bool DelayQueue::StopDelayTask(HandleType handle) {
    return pImp->StopDelayTask(handle);
}

bool DelayQueue::UpdateTaskInterval(HandleType handle, std::int64_t intervalMs) {
    return pImp->UpdateTaskInterval(handle, intervalMs);
}

bool DelayQueue::RestartDelayTask(HandleType handle) {
    return pImp->RestartDelayTask(handle);
}

void DelayQueue::RemoveDelayTask(HandleType handle) {
    pImp->RemoveDelayTask(handle);
}

bool DelayQueue::Validate(HandleType handle) {
    return pImp->Validate(handle);
}

bool DelayQueue::IsWorking(HandleType handle) {
    return pImp->IsWorking(handle);
}

std::int64_t DelayQueue::GetNextTimeoutMsec() const {
    return pImp->GetNextTimeoutMsec();
}

void DelayQueue::HandleEvent() {
    pImp->HandleEvent();
}

void DelayQueue::SetStateChangedCallback(const std::function<void()>& cb) {
    pImp->stateCb = cb;
}

DelayQueue::DelayQueue() : pImp(new Imp) {
}

DelayQueue::~DelayQueue() {
    delete pImp;
}

} // namespace Fundamental