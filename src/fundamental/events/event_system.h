
#pragma once
#include "event.h"
#include "eventpp/eventdispatcher.h"
#include "eventpp/eventqueue.h"
#include <functional>
#include <future>
#include <memory>
#include <set>

namespace Fundamental
{
using EQ = eventpp::EventQueue<EventType, void(const EventPointerType&), EventPolicy>;
using EventHandleType =
    decltype(std::declval<EQ>().appendListener(std::declval<EventType>(), std::declval<EventCallbackType>()));
//---------------------------------------------------------------//
struct InternalNotifyQueuedEvent : Fundamental::Event {
    // an event must contain 'kEventType' field;
    constexpr inline static std::size_t kEventType =
        StringsHash(0, "InternalNotifyQueuedEvent", "InternalNotifyQueuedEvent", "internal") | 1;

    InternalNotifyQueuedEvent() : Event(kEventType) {
    }
};

class EventSystem {
public:
    EventSystem()          = default;
    virtual ~EventSystem() = default;

    bool IsIdle() const;
    /*
     * maxProcessEventNums [int] maximum  proceed events' nums in one tick invocation
     * maxProcessTimeMsec [int] maximum proceed time msec int one tick invocation
     * this function must be called in this module inited thread
     * return the proceed events nums
     */
    std::size_t EventsTick(std::size_t maxProcessEventNums  = static_cast<std::size_t>(~0),
                           std::uint32_t maxProcessTimeMsec = 20);

    /*
     * throw std::invalid_argument when the eventHash is existed
     */
    void RegisterEvent(std::size_t eventHash);

    /*
     * append an event listener
     */
    EventHandleType AddEventListener(std::size_t eventHash, const EventCallbackType& listener);
    /*
     *  cache event into syncer
     * events will be actually dispatched when EventsTick is called
     */
    template <typename EventDataType, typename... Args>
    decltype(auto) DispatcherEvent(Args&&... args);

    /*
     *
     * dispatch an event immediatelly
     * events can be  stopped by Event.StopImmediatePropagation();
     */
    template <typename EventDataType, typename... Args>
    decltype(auto) DispatcherImmediateEvent(Args&&... args);

protected:
    EQ m_syncer;
    std::mutex m_mutex;
    std::set<std::size_t> m_hashDic;
};

/*
 *
 * dispatch an event immediatelly
 * events can be  stopped by Event.StopImmediatePropagation();
 */

template <typename EventDataType, typename... Args>
inline decltype(auto) EventSystem::DispatcherImmediateEvent(Args&&... args) {
    return m_syncer.dispatch(std::make_shared<EventDataType>(std::forward<Args>(args)...));
}

/*
 *  cache event into syncer
 * events will be actually dispatched when EventsTick is called
 */

template <typename EventDataType, typename... Args>
inline decltype(auto) EventSystem::DispatcherEvent(Args&&... args) {
    EventSystem::template DispatcherImmediateEvent<InternalNotifyQueuedEvent>();
    return m_syncer.enqueue(std::make_shared<EventDataType>(std::forward<Args>(args)...));
}

class SignalBrokenType {
public:
    SignalBrokenType() = default;
    bool is_broken() const {
        return is_broken_;
    }
    SignalBrokenType(bool has_broken) : is_broken_(has_broken) {
    }

private:
    const bool is_broken_ = false;
};

template <typename Prototype, typename PoliciesType>
class SignalBase;

// callbacklist wapper class
template <typename PoliciesType, typename ReturnType, typename... Args>
class SignalBase<ReturnType(Args...), PoliciesType> {
public:
    using ConnectionType = eventpp::CallbackList<ReturnType(Args...), PoliciesType>;
    using Callback_ =
        typename eventpp::internal_::SelectCallback<PoliciesType,
                                                    eventpp::internal_::HasTypeCallback<PoliciesType>::value,
                                                    std::function<ReturnType(Args...)>>::Type;
    using HandleType = typename eventpp::CallbackList<ReturnType(Args...), PoliciesType>::Handle;
    class SignalHandle {
    public:
        SignalHandle() = default;
        SignalHandle(HandleType handle, const std::shared_ptr<ConnectionType>& connections) noexcept :
        w_connections_(connections), handle_(handle) {
        }
        ~SignalHandle() {
            auto c = w_connections_.lock();
            if (c) c->remove(handle_);
        }

        SignalHandle(SignalHandle&& other) noexcept :
        w_connections_(std::move(other.w_connections_)), handle_(std::move(other.handle_)) {
        }
        SignalHandle& operator=(SignalHandle&& other) noexcept {
            release();
            w_connections_ = std::move(other.w_connections_);
            handle_        = std::move(other.handle_);
            return *this;
        }

        SignalHandle(const SignalHandle&)            = delete;
        SignalHandle& operator=(const SignalHandle&) = delete;

    private:
        void release() {
            if (!handle_) return;
            auto c = w_connections_.lock();
            if (c) c->remove(handle_);
            w_connections_.reset();
            handle_.reset();
        }

    private:
        std::weak_ptr<ConnectionType> w_connections_;
        HandleType handle_;
    };
    using GuardType = SignalHandle;

    struct SignalFuture {
        HandleType handle;
        std::promise<void> p;
    };

public:
    /// @brief bind excutors for this signal
    /// @param callback  the excutor function
    /// @param append_mode true means the excutor will be append to the process deque tail,false means the excutor
    /// will be add to the process deque top
    /// @return the handle which can be used to cancel the excutor binding
    HandleType Connect(const Callback_& callback, bool append_mode = true) const;
    template <typename T>
    HandleType Connect(std::weak_ptr<T> token, const Callback_& callback, bool append_mode = true) const {
        std::shared_ptr<HandleType> reserve_handle = std::make_shared<HandleType>();
        auto auto_disconnect_cb = [callback, token, this, reserve_handle](Args... args) -> ReturnType {
            auto ptr = token.lock();
            if (!ptr) {
                DisConnect(*reserve_handle);
                if constexpr (!std::is_void_v<ReturnType>) {
                    return ReturnType {};
                } else {
                    return;
                }
            }
            if constexpr (std::is_void_v<ReturnType>) {
                callback(std::forward<Args>(args)...);
            } else {
                return callback(std::forward<Args>(args)...);
            }
        };
        *reserve_handle = Connect(auto_disconnect_cb, append_mode);
        return *reserve_handle;
    }
    template <typename T>
    HandleType Connect(std::shared_ptr<T> token, const Callback_& callback, bool append_mode = true) const {
        return Connect(std::weak_ptr<T>(token), callback, append_mode);
    }

    GuardType GuardConnect(const Callback_& callback, bool append_mode = true) const {
        return GuardType(Connect(callback, append_mode), _connections);
    }

    std::shared_ptr<SignalFuture> MakeSignalFuture() const {
        auto ret    = std::make_shared<SignalFuture>();
        ret->handle = Connect([ret, this](Args...) -> void {
            DisConnect(ret->handle);
            ret->p.set_value();
        });
        return ret;
    }
    bool DisConnect(HandleType handle) const;
    void Emit(Args... args) const;
    void operator()(Args... args) const;
    bool operator!() const {
        return _connections->empty();
    }
    operator bool() const {
        return !_connections->empty();
    }

private:
    std::shared_ptr<ConnectionType> _connections = std::make_shared<ConnectionType>();
};

template <typename PoliciesType, typename ReturnType, typename... Args>
inline typename SignalBase<ReturnType(Args...), PoliciesType>::HandleType SignalBase<
    ReturnType(Args...),
    PoliciesType>::Connect(const Callback_& callback, bool append_mode) const {
    if (append_mode) {
        return _connections->append(callback);
    } else {
        return _connections->prepend(callback);
    }
}

template <typename PoliciesType, typename ReturnType, typename... Args>
inline bool SignalBase<ReturnType(Args...), PoliciesType>::DisConnect(HandleType handle) const {
    return _connections->remove(handle);
}

template <typename PoliciesType, typename ReturnType, typename... Args>
inline void SignalBase<ReturnType(Args...), PoliciesType>::Emit(Args... args) const {
    operator()(std::forward<Args>(args)...);
}

template <typename PoliciesType, typename ReturnType, typename... Args>
inline void SignalBase<ReturnType(Args...), PoliciesType>::operator()(Args... args) const {
    _connections->broken_call(std::forward<Args>(args)...);
}

template <typename Prototype_, typename Policies_ = eventpp::DefaultPolicies>
class Signal : public SignalBase<Prototype_, Policies_> {};

} // namespace Fundamental
