#include "event_system.h"
#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"

namespace Fundamental {
bool EventSystem::IsIdle() const {
    return m_syncer.emptyQueue();
}

std::size_t EventSystem::EventsTick(std::size_t maxProcessEventNums, std::uint32_t maxProcessTimeMsec) {
    auto startTimeMsec      = Fundamental::Timer::GetTimeNow();
    std::size_t proceedNums = 0;
    while (maxProcessTimeMsec != 0) {
        if (!m_syncer.processOne()) break;
        --maxProcessTimeMsec;
        ++proceedNums;
        if (maxProcessTimeMsec > 0 && (Fundamental::Timer::GetTimeNow() - startTimeMsec >
                                       static_cast<decltype(startTimeMsec)>(maxProcessTimeMsec))) {
            break;
        }
    }
    return proceedNums;
}

void EventSystem::RegisterEvent(std::size_t eventHash) {
    std::lock_guard<std::mutex> locker(m_mutex);
    if (m_hashDic.insert(eventHash).second) {
        throw std::invalid_argument(StringFormat("{} is existed", eventHash));
    }
}

EventHandleType EventSystem::AddEventListener(std::size_t eventHash, const EventCallbackType& listener) {
    return m_syncer.appendListener(eventHash, listener);
}

} // namespace Fundamental
