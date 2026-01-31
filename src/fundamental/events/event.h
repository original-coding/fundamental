#pragma once
#include <cassert>
#include <functional>
#include <memory>
#include <set>

#include "fundamental/basic/string_utils.hpp"

namespace Fundamental {
using EventType = std::size_t;
struct Event {
    explicit Event(EventType type) : eventType(type) {
    }

    virtual ~Event() {
    }
    const EventType eventType;
};

using EventPointerType  = std::shared_ptr<Event>;
using EventCallbackType = std::function<void(const EventPointerType&)>;

struct EventPolicy {
    static EventType getEvent(const EventPointerType& event) {
        return event->eventType;
    }
};

template <typename TargetType>
auto ConstCastEvent(const Fundamental::EventPointerType& event) {
    using RawType = std::decay_t<TargetType>;
    assert(event.get() && RawType::kEventType == event->eventType);
    return reinterpret_cast<const RawType*>(event.get());
}

template <typename TargetType>
auto CastEvent(const Fundamental::EventPointerType& event) {
    using RawType = std::decay_t<TargetType>;
    return const_cast<RawType*>(ConstCastEvent<RawType>(event));
}
} // namespace Fundamental