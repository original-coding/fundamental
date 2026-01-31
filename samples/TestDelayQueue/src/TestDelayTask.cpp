

#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"
#include "fundamental/thread_pool/thread_utils.hpp"

#include <condition_variable>
#include <fstream>
#include <future>
#include <memory>
#if 1
static Fundamental::DelayQueue* g_delayQueue = nullptr;
static std::unique_ptr<std::thread> s_nativeLoopThread;
static bool g_exitFlag = false;
static std::condition_variable exitCV;
static std::mutex syncMutex;

void WakeUp() {
    std::scoped_lock<std::mutex> locker(syncMutex);
    exitCV.notify_one();
}
static void NativeLoop();
static void TestApi();
static void StressTesting(std::size_t eventNum);
int main(int argc, char* argv[]) {
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::debug;
    options.logFormat    = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    Fundamental::Logger::Initialize(std::move(options));
    // init native
    g_delayQueue = new Fundamental::DelayQueue();
    {
        auto startTimeMsec = Fundamental::Timer::GetTimeNow();
        std::size_t count  = 0;
        while (count++ < 100000)
            g_delayQueue->HandleEvent();
        FINFO("call HandleEvent 100000 times with no task costs [", Fundamental::Timer::GetTimeNow() - startTimeMsec,
              "ms] \n");
    }

    TestApi();
    g_exitFlag = true;
    s_nativeLoopThread->join();
    delete g_delayQueue;
    StressTesting(1000000);
    return 0;
}

void NativeLoop() {
    while (!g_exitFlag) {
        g_delayQueue->HandleEvent();
        // do some logic handler
        std::this_thread::yield();
    }
}

void TestApi() {
    Fundamental::DelayQueue::HandleType cycleHandle;
    std::atomic<int> joinCnt = 0;
    std::size_t cycleCnt     = 0;
    std::size_t cycleCnt2    = 0;
    FINFO("start {}", Fundamental::Timer::GetTimeStr());
    auto loopHandle = g_delayQueue->AddDelayTask(10, [&]() {
        ++cycleCnt;
        if (cycleCnt == 100) {
            joinCnt.fetch_add(1);
            WakeUp();
        }
        if (cycleCnt > 100) FINFO("{} not stop cycle:{}", Fundamental::Timer::GetTimeStr(), cycleCnt);
    });
    auto handle     = g_delayQueue->AddDelayTask(
        1000,
        [&]() {
            FINFO("{} single:{}", Fundamental::Timer::GetTimeStr(), cycleCnt);
            joinCnt.fetch_add(1);
            WakeUp();
        },
        true);
    cycleHandle = g_delayQueue->AddDelayTask(
        11,
        [&]() {
            ++cycleCnt2;
            FINFO("{} cycle:{}", Fundamental::Timer::GetTimeStr(), cycleCnt2);
            if (cycleCnt2 == 100) {
                joinCnt.fetch_add(1);
                WakeUp();
                g_delayQueue->StopDelayTask(cycleHandle);
                // add exit sync task
                auto tempHandle = g_delayQueue->AddDelayTask(
                    1001,
                    [&]() {
                        joinCnt.fetch_add(1);
                        WakeUp();
                    },
                    true);
                g_delayQueue->StartDelayTask(tempHandle);
            }
        },
        false, false);
    g_delayQueue->StartDelayTask(loopHandle);
    g_delayQueue->StartDelayTask(handle);
    g_delayQueue->StartDelayTask(cycleHandle);
    FASSERT(g_delayQueue->IsWorking(loopHandle));
    FASSERT(g_delayQueue->IsWorking(handle));
    FASSERT(g_delayQueue->IsWorking(cycleHandle));

    s_nativeLoopThread = std::make_unique<std::thread>(NativeLoop);
    std::unique_lock<std::mutex> locker(syncMutex);
    exitCV.wait(locker, [&]() -> bool { return joinCnt.load() >= 4; });
    FASSERT(g_delayQueue->IsWorking(loopHandle));
    g_delayQueue->StopDelayTask(loopHandle);
    FINFO("STOP");
    FASSERT(!g_delayQueue->IsWorking(loopHandle));

    FASSERT(g_delayQueue->Validate(cycleHandle));
    g_delayQueue->RemoveDelayTask(cycleHandle);
    FASSERT(!g_delayQueue->Validate(cycleHandle));
    FASSERT(!g_delayQueue->Validate(handle));
}

void StressTesting(std::size_t eventNum) {
    // init native
    g_delayQueue                       = new Fundamental::DelayQueue();
    std::atomic<std::size_t> finishCnt = 0;
    std::size_t targetCnt              = eventNum;
    FINFO("start {}", Fundamental::Timer::GetTimeStr());
    g_exitFlag         = false;
    s_nativeLoopThread = std::make_unique<std::thread>(NativeLoop);
    auto threadCnt     = Fundamental::hardware_concurrency();
    auto delayTask     = [&]() {
        ++finishCnt;
        if (finishCnt.load() >= targetCnt) WakeUp();
    };
    auto startTimeMsec = Fundamental::Timer::GetTimeNow();
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < threadCnt; ++i)
        threads.emplace_back(std::thread([&]() {
            while (finishCnt.load() <= targetCnt) {
                auto tempHandle = g_delayQueue->AddDelayTask(0, delayTask, true);
                g_delayQueue->StartDelayTask(tempHandle);
                std::this_thread::yield();
            }
        }));
    {
        std::unique_lock<std::mutex> locker(syncMutex);
        exitCV.wait(locker, [&]() -> bool { return finishCnt.load() >= targetCnt; });
    }

    FINFO("process {} single delay tasks costs [{}ms]", targetCnt, Fundamental::Timer::GetTimeNow() - startTimeMsec);

    FINFO("finish {}", Fundamental::Timer::GetTimeStr());
    g_exitFlag = true;
    for (auto& t : threads)
        t.join();
    s_nativeLoopThread->join();
    delete g_delayQueue;
}
#else
int main() {
    return 0;
}
#endif
