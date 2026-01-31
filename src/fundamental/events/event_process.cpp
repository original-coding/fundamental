
#include "event_process.h"
#include "event_system.h"
#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"
#include <iostream>
#include <unordered_map>
namespace Fundamental {

EventsHandler::EventsHandler() : pEventSystem(new Fundamental::EventSystem), pDelayQueue(new Fundamental::DelayQueue) {
    {
        using RegisterEventType = Fundamental::EngineProcessEvent;
        auto func               = [](const Fundamental::EventPointerType& event) {
            auto* ptr = Fundamental::ConstCastEvent<RegisterEventType>(event);
            if (ptr->task) ptr->task();
        };
        pEventSystem->AddEventListener(RegisterEventType::kEventType, func);
    }
    // wakeup when new process  event is enqueued
    {
        using RegisterEventType = Fundamental::InternalNotifyQueuedEvent;
        auto func               = [=](const Fundamental::EventPointerType& event) { WakeUp(); };
        pEventSystem->AddEventListener(RegisterEventType::kEventType, func);
    }
    // wakeup when timeout status changed
    pDelayQueue->SetStateChangedCallback([=]() { WakeUp(); });
}

EventsHandler::~EventsHandler() {
    if (pDelayQueue) delete pDelayQueue;
    if (pEventSystem) delete pEventSystem;
}

void EventsHandler::Tick() {
    // handle queued event
    pEventSystem->EventsTick();
    // handle delay task
    pDelayQueue->HandleEvent();

    if (pEventSystem->IsIdle()) {
        auto waitTimeMsec = pDelayQueue->GetNextTimeoutMsec();
        if (waitTimeMsec > 10 || waitTimeMsec <= 0) waitTimeMsec = 10;
        Wait(waitTimeMsec);
    }
}

void EventsHandler::WakeUp() {
    WakeUpImp();
}

Fundamental::EventSystem* EventsHandler::EventSystem() {
    return pEventSystem;
}

Fundamental::DelayQueue* EventsHandler::DelayQueue() {
    return pDelayQueue;
}

void EventsHandler::PostProcessEvent(const std::function<void()>& event) {
    pEventSystem->DispatcherEvent<Fundamental::EngineProcessEvent>(event);
}

void EventsHandlerNormal::WakeUpImp() {
    std::scoped_lock<std::mutex> locker(notifyMutex);
    cv.notify_one();
}

void EventsHandlerNormal::Wait(std::int64_t timeMsec) {
    std::unique_lock<std::mutex> locker(notifyMutex);
    if (timeMsec > 0) {
        cv.wait_for(locker, std::chrono::milliseconds(timeMsec));
    } else {
        cv.wait(locker);
    }
}

} // namespace Fundamental