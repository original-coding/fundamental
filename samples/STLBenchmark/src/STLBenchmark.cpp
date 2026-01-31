

#include <chrono>
#include <iostream>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>

#include "benchmark/benchmark.h"
#include "fundamental/basic/random_generator.hpp"

static void TestRandom(benchmark::State& state) {
    auto g = Fundamental::DefaultNumberGenerator<std::size_t>();
    std::set<std::size_t> container;
    for (auto _ : state) {
        benchmark::DoNotOptimize(g());
    }
}


static void TestSet(benchmark::State& state) {
    auto g = Fundamental::DefaultNumberGenerator<std::size_t>();
    std::set<std::size_t> container;
    for (auto _ : state) {
        auto v = g();
        container.insert(v);
        benchmark::DoNotOptimize(container.count(v));
    }
}

static void TestHashSet(benchmark::State& state) {
    auto g = Fundamental::DefaultNumberGenerator<std::size_t>();
    std::unordered_set<std::size_t> container;
    for (auto _ : state) {
        auto v = g();
        container.insert(v);
        benchmark::DoNotOptimize(container.count(v));
    }
}
BENCHMARK(TestRandom)->Threads(1);
BENCHMARK(TestSet)->Iterations(20000000)->Threads(1);
BENCHMARK(TestHashSet)->Iterations(20000000)->Threads(1);
BENCHMARK(TestSet)->Iterations(2000000)->Threads(1);
BENCHMARK(TestHashSet)->Iterations(2000000)->Threads(1);
BENCHMARK(TestSet)->Iterations(200000)->Threads(1);
BENCHMARK(TestHashSet)->Iterations(200000)->Threads(1);
BENCHMARK(TestSet)->Iterations(20000)->Threads(1);
BENCHMARK(TestHashSet)->Iterations(20000)->Threads(1);
BENCHMARK(TestSet)->Iterations(2000)->Threads(1);
BENCHMARK(TestHashSet)->Iterations(2000)->Threads(1);
BENCHMARK(TestSet)->Iterations(200)->Threads(1);
BENCHMARK(TestHashSet)->Iterations(200)->Threads(1);
BENCHMARK(TestSet)->Iterations(20)->Threads(1);
BENCHMARK(TestHashSet)->Iterations(20)->Threads(1);
BENCHMARK(TestSet)->Iterations(10)->Threads(1);
BENCHMARK(TestHashSet)->Iterations(10)->Threads(1);
int main(int argc, char* argv[]) {

    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}