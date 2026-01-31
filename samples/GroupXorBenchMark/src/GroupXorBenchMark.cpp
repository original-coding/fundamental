

#include "fundamental/basic/allocator.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
#include <chrono>
#include <iostream>
#include <list>
#include <memory>
#include <string>

#include "benchmark/benchmark.h"

extern "C" {
inline void TestNormalXor(uint8_t* out, const uint8_t* a, const uint8_t* b, std::size_t len) {
    uint8_t* pOut = (uint8_t*)out;
    auto pa       = (const uint8_t*)a;
    auto pb       = (const uint8_t*)b;
    size_t i;
    for (i = 0; i < len; i++) {
        pOut[i] = pa[i] ^ pb[i];
    }
}

inline void TestGroupXor(uint8_t* out, const uint8_t* a, const uint8_t* b, std::size_t len) {
    constexpr std::size_t stepLen = sizeof(size_t);
    std::size_t* pOut             = (std::size_t*)out;
    auto pa                       = (const std::size_t*)a;
    auto pb                       = (const std::size_t*)b;
    std::size_t count             = len / stepLen;
    std::size_t mod               = len % stepLen;
    for (std::size_t i=0; i < count; ++i) {
        pOut[i] = pa[i] ^ pb[i];
    }
    std::size_t offset = len - mod;
    TestNormalXor(out + offset, a + offset, b + offset, len - offset);
}
}

union TestData {
    uint8_t data[16];
    std::uint64_t value[2];
};

static void DoTestNormalXor(benchmark::State& state) {
    TestData out;
    TestData in;
    in.value[0] = 23123124;
    in.value[1] = 3123124124;
    TestData key;
    key.value[0] = 5435543534534535;
    key.value[1] = 3154353423123253424;

    TestNormalXor(out.data, in.data, key.data, state.range(0));
    TestData test;
    TestGroupXor(test.data, in.data, key.data, state.range(0));
    assert(std::memcmp(out.data, test.data, state.range(0)) == 0);
    for (auto _ : state) {
        TestNormalXor(out.data, in.data, key.data, state.range(0));
    }
}

static void DoTestGroupXor(benchmark::State& state) {
    TestData out;
    TestData in;
    in.value[0] = 23123124;
    in.value[1] = 3123124124;
    TestData key;
    key.value[0] = 5435543534534535;
    key.value[1] = 3154353423123253424;
    for (auto _ : state) {
        TestGroupXor(out.data, in.data, key.data, state.range(0));
    }
}

BENCHMARK(DoTestNormalXor)->DenseRange(1, 16);
BENCHMARK(DoTestGroupXor)->DenseRange(1, 16);

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