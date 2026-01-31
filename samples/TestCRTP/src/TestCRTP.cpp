
#ifdef FORCE_TIME_TRACKER
    #undef FORCE_TIME_TRACKER
#endif
#define FORCE_TIME_TRACKER 1

#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
#include "fundamental/tracker/time_tracker.hpp"
#include <chrono>
#include <iostream>
#include <memory>

#include "benchmark/benchmark.h"
#define TEST_CAST_IMP(a, b) a##b
#define TEST_CAST(a, b)     TEST_CAST_IMP(a, b)
#define DECLARE_FUNC                                                                                                   \
    virtual void TEST_CAST(test, __LINE__)() {                                                                         \
    }
namespace TestVirtual {
class Base {
public:
    DECLARE_FUNC
    virtual void Add(size_t num)  = 0;
    virtual size_t Result() const = 0;
    DECLARE_FUNC
    Base() {
    }
    virtual ~Base() {
    }
};

class Derived final : public Base {
public:
    void Add(size_t num) override {
        benchmark::DoNotOptimize(result += num);
    }
    size_t Result() const override {
        return result;
    }

private:
    size_t result = 0;
};
} // namespace TestVirtual

namespace TestCRTP {
template <class derived>
class Base {
public:
    using derived_ = derived;

public:
    DECLARE_FUNC
    void Add(size_t num) {
        imp().Add(num);
    }
    size_t Result() const {
        return imp().Result();
    }
    DECLARE_FUNC
    Base() {
    }

protected:
    derived& imp() {
        return *(static_cast<derived*>(this));
    };
    const derived& imp() const {
        return *(static_cast<const derived*>(this));
    };
};

class Derived final : public Base<Derived> {
public:
    void Add(size_t num) {
        benchmark::DoNotOptimize(result += num);
    }
    size_t Result() {
        return result;
    }

private:
    size_t result = 0;
};
} // namespace TestCRTP

class DerivedBenchmark : public benchmark::Fixture {
public:
    TestVirtual::Derived d;
    TestCRTP::Derived d2;
    void SetUp(const ::benchmark::State& state) override {
    }

    void TearDown(const ::benchmark::State& state) override {
        FDEBUGS << "result:" << d.Result();
        FDEBUGS << "result:" << d2.Result();
    }
};

BENCHMARK_F(DerivedBenchmark, virtual_test)
(benchmark::State& state) {
    for (auto _ : state) {
        d.Add(1);
    }
};
BENCHMARK_F(DerivedBenchmark, crtp_test)
(benchmark::State& state) {
    for (auto _ : state) {
        d2.Add(1);
    }
};
int main(int argc, char* argv[]) {
    char arg0_default[] = "benchmark";
    char* args_default  = arg0_default;
    if (!argv) {
        argc = 1;
        argv = &args_default;
    }
    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}