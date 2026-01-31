
#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"
#include "fundamental/events/event_system.h"
#include <condition_variable>
#include <fstream>
#include <future>
#include <memory>
static Fundamental::EventSystem* g_nativeEventSystem = nullptr;
static std::unique_ptr<std::thread> s_nativeLoopThread;
static bool g_exitFlag = false;

namespace Events
{
struct NativeNotifyEvents : Fundamental::Event {
    NativeNotifyEvents(const char* _msg, std::size_t _index) : Event(0), msg(_msg), index(_index) {
    }
    std::string msg;
    std::size_t index;
};

struct NativeCallEvents : Fundamental::Event {
    // an event must contain 'kEventType' field;
    constexpr inline static std::size_t kEventType = 1;
    NativeCallEvents(const char* _msg, std::size_t _index) : Event(1), msg(_msg), index(_index) {
    }
    std::string msg;
    std::size_t index;
};
} // namespace Events
static void TestSignal();
static void TestAutoDisconnect();
static void NativeLoop();
static void NativeBindEventListener();
static void StressTesting(std::size_t eventNum);
int main(int argc, char* argv[]) {
    if (1) {
        TestAutoDisconnect();

        return 0;
    }
    TestSignal();
    // init native
    g_nativeEventSystem = new Fundamental::EventSystem();
    NativeBindEventListener();
    { // test disaptcher directly
        g_nativeEventSystem->DispatcherImmediateEvent<Events::NativeCallEvents>("test", 0);
        g_nativeEventSystem->DispatcherImmediateEvent<Events::NativeNotifyEvents>("test", 0);
    }

    s_nativeLoopThread = std::make_unique<std::thread>(NativeLoop);

    std::size_t index = 0;
    while (index < 50) {
        g_nativeEventSystem->DispatcherImmediateEvent<Events::NativeCallEvents>("test", index);
        g_nativeEventSystem->DispatcherEvent<Events::NativeCallEvents>("test", index);
        ++index;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    g_exitFlag = true;
    s_nativeLoopThread->join();
    delete g_nativeEventSystem;
    StressTesting(1000000);
    Fundamental::Signal<void()> test;
    test.Connect([]() { FINFO("simple signal"); });
    test.Emit();
    Fundamental::Signal<void(int)> testInt;
    testInt.Connect([](int index) { FINFO("int singnal {}", index); });

    testInt.Connect([](int index) { FINFO("test multi int singnal {}", index); });
    auto h = testInt.Connect([](int index) { FINFO("test remove int singnal {}", index); });
    std::thread([&]() {
        int cnt = 10;
        while (cnt > 0) {
            --cnt;
            if (cnt == 5) {
                FWARN("remove handle");
                testInt.DisConnect(h);
            }
            testInt.Emit(cnt);
        }
    }).join();
    return 0;
}

void NativeLoop() {

    while (!g_exitFlag) {
        g_nativeEventSystem->EventsTick();
        // do some logic handler
        std::this_thread::yield();
    }
}

void TestSignal() {
    std::size_t cnt = 0;
    Fundamental::Signal<void()> s1;
    auto handle1 = s1.Connect([&]() { ++cnt; });
    Fundamental::Signal<Fundamental::SignalBrokenType()> s2;
    auto handle2 = s2.Connect([&]() -> Fundamental::SignalBrokenType {
        if (cnt <= 2)
            return Fundamental::SignalBrokenType(false);
        else {
            FASSERT(cnt <= 3);
            return Fundamental::SignalBrokenType(true);
        }
    });

    auto s3      = std::make_shared<Fundamental::Signal<bool()>>();
    auto handle3 = s3->Connect([&]() -> bool { return cnt <= 3; });
    while (cnt < 5) {
        s1.Emit();
        s2.Emit();
        s3->Emit();
    }
    FASSERT(handle1);
    FASSERT(!handle2);
    FASSERT(handle3);
    s3.reset();
    FASSERT(!handle3);
}

void TestAutoDisconnect() {
    std::size_t cnt = 0;
    Fundamental::Signal<void()> s1;
    std::shared_ptr<bool> token = std::make_shared<bool>();
    s1.Connect(token, [&, s_token = token]() mutable {
        ++cnt;
        if (cnt < 3) {
            FINFO("auto disconnect normal");
        }
        if (cnt == 3) {
            FINFO("auto disconnect test");
            s_token.reset();
        }
        if (cnt > 3) {
            FASSERT_ACTION(false, throw std::runtime_error("should not reach"));
        }
    });
    token.reset();
    {
        auto signal_guard = s1.GuardConnect([]() { FINFO("from guard"); });
        s1.Emit();
    }

    s1.Emit();
    s1.Emit();
    s1.Emit();
}

void NativeBindEventListener() {
    {
        using BindEventType  = Events::NativeNotifyEvents;
        auto eventListenFunc = [](const Fundamental::EventPointerType& ptr) {
            auto raw = static_cast<const BindEventType*>(ptr.get());
            // we need to check event type and event size
            FINFO("recv(NativeNotifyEvents):{} index:{}", raw->msg, raw->index);
        };
        g_nativeEventSystem->AddEventListener(0, eventListenFunc);
    }
    {
        using BindEventType  = Events::NativeCallEvents;
        auto eventListenFunc = [](const Fundamental::EventPointerType& ptr) {
            auto raw = static_cast<const BindEventType*>(ptr.get());
            // we need to check event type and event size
            FINFO("recv(NativeCallEvents):{} index:{}", raw->msg, raw->index);
        };
        g_nativeEventSystem->AddEventListener(1, eventListenFunc);
    }
}
void StressTesting(std::size_t eventNum) {
    // init native
    g_nativeEventSystem = new Fundamental::EventSystem();
    std::size_t count   = 0;
    std::condition_variable exitCV;
    std::mutex syncMutex;
    {
        auto eventListenFunc = [&](const Fundamental::EventPointerType& ptr) {
            ++count;
            if (count == eventNum) {
                std::scoped_lock<std::mutex> locker(syncMutex);
                exitCV.notify_one();
            }
        };
        g_nativeEventSystem->AddEventListener(0, eventListenFunc);
    }
    g_exitFlag         = false;
    s_nativeLoopThread = std::make_unique<std::thread>(NativeLoop);
    std::size_t index  = 0;
    FINFO("start stress testing eventNum={}", eventNum);
    auto startTimeMsec = Fundamental::Timer::GetTimeNow();
    while (index < eventNum) {
        g_nativeEventSystem->DispatcherEvent<Events::NativeNotifyEvents>("test", index);
        ++index;
    }
    {
        std::unique_lock<std::mutex> locker(syncMutex);
        exitCV.wait(locker);
    }
    FINFO("start stress testing eventNum={} cots [{}ms]", eventNum, Fundamental::Timer::GetTimeNow() - startTimeMsec);
    g_exitFlag = true;
    s_nativeLoopThread->join();
    delete g_nativeEventSystem;
}
