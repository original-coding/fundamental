

#include <chrono>
#include <iostream>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>

#include "benchmark/benchmark.h"
#include "fundamental/algorithm/hash.hpp"

template <std::size_t blockSize>
static void TestStlHash(benchmark::State& state) {
    std::string msg(blockSize, 'a');
    std::hash<std::string> hasher;
    for (auto _ : state) {
        benchmark::DoNotOptimize(hasher(msg));
    }
}

template <std::size_t blockSize>
static void TestWYHash(benchmark::State& state) {
    std::string msg(blockSize, 'a');
    for (auto _ : state) {
        benchmark::DoNotOptimize(Fundamental::Hash(msg.data(), msg.size()));
    }
}

BENCHMARK_TEMPLATE(TestStlHash, 1);
BENCHMARK_TEMPLATE(TestStlHash, 64);
BENCHMARK_TEMPLATE(TestStlHash, 128);
BENCHMARK_TEMPLATE(TestStlHash, 512);
BENCHMARK_TEMPLATE(TestStlHash, 1024);
BENCHMARK_TEMPLATE(TestStlHash, 8192);
BENCHMARK_TEMPLATE(TestStlHash, 100000);
BENCHMARK_TEMPLATE(TestStlHash, 1000000);
BENCHMARK_TEMPLATE(TestStlHash, 10000000);

BENCHMARK_TEMPLATE(TestWYHash, 1);
BENCHMARK_TEMPLATE(TestWYHash, 64);
BENCHMARK_TEMPLATE(TestWYHash, 128);
BENCHMARK_TEMPLATE(TestWYHash, 512);
BENCHMARK_TEMPLATE(TestWYHash, 1024);
BENCHMARK_TEMPLATE(TestWYHash, 8192);
BENCHMARK_TEMPLATE(TestWYHash, 100000);
BENCHMARK_TEMPLATE(TestWYHash, 1000000);
BENCHMARK_TEMPLATE(TestWYHash, 10000000);

int main(int argc, char* argv[]) {

    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}