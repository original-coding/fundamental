#pragma once
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
#include "fundamental/delay_queue/delay_queue.h"
#include "fundamental/events/event_process.h"
#include "fundamental/events/event_system.h"
#include "fundamental/thread_pool/thread_pool.h"
#include <atomic>
#include <type_traits>
namespace Fundamental
{

struct ApplicationInterface {
    constexpr ApplicationInterface() = default;
    virtual ~ApplicationInterface();
    virtual bool Load(int argc, char** argv);
    virtual bool Init();
    virtual void Tick();
    virtual void Exit();
};

class Application : public EventsHandlerNormal, public Singleton<Application> {
public:
    Signal<void(int, char**)> loadStarted;
    Signal<void(bool)> loadFinished;
    Signal<void()> initStarted;
    Signal<void(bool)> initFinished;
    Signal<void()> loopStarted;
    Signal<void()> loopFinished;
    Signal<void()> exitStarted;
    Signal<void()> exitFinished;

public:
    void OverlayApplication(std::shared_ptr<ApplicationInterface>&& newImp);
    bool Load(int argc, char** argv) {
        loadStarted.Emit(argc, argv);
        bool ret = imp ? imp->Load(argc, argv) : true;
        loadFinished.Emit(ret);
        return ret;
    }
    bool Init() {
        initStarted.Emit();
        bool ret = imp ? imp->Init() : true;
        initFinished.Emit(ret);
        return ret;
    }
    void Loop() {
        loopStarted.Emit();
        bRunning.exchange(true);
        while (bRunning) {
            if (imp) imp->Tick();
            Tick();
        }
        loopFinished.Emit();
    }
    void Exit() {
        if (bRunning) {
            PostProcessEvent([this]() {
                bool expectedValue = true;
                if (!bRunning.compare_exchange_strong(expectedValue, false)) return;
                exitStarted.Emit();
                if (imp) imp->Exit();
                exitFinished.Emit();
            });
            WakeUp();
        }
    }
    [[nodiscard]] bool IsRunning() const noexcept {
        return bRunning.load(std::memory_order::memory_order_relaxed);
    }

protected:
    std::shared_ptr<ApplicationInterface> imp;
    std::atomic_bool bRunning = false;
};
} // namespace Fundamental