

#include "fundamental/basic/allocator.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
#include <chrono>
#include <iostream>
#include <list>
#include <memory>
#include <string>

#include "benchmark/benchmark.h"

template <std::size_t blockSize>
struct TestObject {
    std::int32_t x;
    std::uint8_t data[blockSize];
    TestObject() {
        benchmark::DoNotOptimize(data[1] = 0);
    }
    ~TestObject() {
        benchmark::DoNotOptimize(data[blockSize - 1] = 1);
    }
};

template <std::size_t blockSize>
static void TestNormal(benchmark::State& state) {
    std::list<TestObject<blockSize>> l;
    for (auto _ : state) {
        for (std::int64_t i = 0; i < state.range(0); ++i) {
            l.emplace_back();
        }
        while (!l.empty())
            l.pop_front();
    }
}

template <std::size_t blockSize>
static void TestPool(benchmark::State& state) {
    std::list<TestObject<blockSize>, Fundamental::ThreadUnSafeObjectPoolAllocator<TestObject<blockSize>>> l;
    for (auto _ : state) {
        for (std::int64_t i = 0; i < state.range(0); ++i) {
            l.emplace_back();
        }
        while (!l.empty())
            l.pop_front();
    }
}

BENCHMARK_TEMPLATE(TestNormal, 1024)->RangeMultiplier(16)->Range(1, 256);
BENCHMARK_TEMPLATE(TestPool, 1024)->RangeMultiplier(16)->Range(1, 256);

BENCHMARK_TEMPLATE(TestNormal, 4096)->Arg(1)->Arg(10);
BENCHMARK_TEMPLATE(TestPool, 4096)->Arg(1)->Arg(10);

BENCHMARK_TEMPLATE(TestNormal, 4096 * 4)->Arg(1)->Arg(10);
BENCHMARK_TEMPLATE(TestPool, 4096 * 4)->Arg(1)->Arg(10);

BENCHMARK_TEMPLATE(TestNormal, 4096 * 16)->Arg(1)->Arg(10);
BENCHMARK_TEMPLATE(TestPool, 4096 * 16)->Arg(1)->Arg(10);

BENCHMARK_TEMPLATE(TestNormal, 4096 * 128)->Arg(1)->Arg(10);
BENCHMARK_TEMPLATE(TestPool, 4096 * 128)->Arg(1)->Arg(10);

BENCHMARK_TEMPLATE(TestNormal, 4096 * 512)->Arg(1)->Arg(10)->Arg(100);
BENCHMARK_TEMPLATE(TestPool, 4096 * 512)->Arg(1)->Arg(10)->Arg(100);

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