
#pragma once
#include "event_system.h"
#include <condition_variable>
#include <mutex>
#include <string>
namespace Fundamental {

//---------------------------------------------------------------//
struct EngineProcessEvent : Fundamental::Event {
    using TaskType = std::function<void()>;
    // an event must contain 'kEventType' field;
    constexpr inline static std::size_t kEventType =
        StringsHash(0, "EngineProcessEvent", "EngineProcessEvent", "Interface") | 1;

    EngineProcessEvent(const TaskType& task) : Event(kEventType), task(task) {
    }
    TaskType task;
};

class DelayQueue;
class EventSystem;
// EventsHandler will add a listener for Events::EngineProcessEvent by default
class EventsHandler {
public:
    EventsHandler();
    virtual ~EventsHandler();
    // post call events to the events tick thread
    virtual void PostProcessEvent(const std::function<void()>& event);
    // update internal status
    virtual void Tick();
    void WakeUp();
    Fundamental::EventSystem* EventSystem();
    Fundamental::DelayQueue* DelayQueue();

protected:
    virtual void WakeUpImp()                 = 0;
    virtual void Wait(std::int64_t timeMsec) = 0;

protected:
    Fundamental::EventSystem* pEventSystem = nullptr;
    Fundamental::DelayQueue* pDelayQueue   = nullptr;
};

class EventsHandlerNormal : public EventsHandler {
public:
    void WakeUpImp() override;
    void Wait(std::int64_t timeMsec) override;

protected:
    std::mutex notifyMutex;
    std::condition_variable cv;
};

} // namespace Fundamental