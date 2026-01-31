
#include "benchmark/benchmark.h"
#include "fundamental/basic/random_generator.hpp"
double test_func(double x, double y) {
    double ret = 0.0;
    for (std::size_t i = 0; i < 10000; ++i) {
        ret *= x;
        ret += y;
    }
    return ret;
};
std::size_t test_func2(std::size_t x, std::size_t y) {
    std::size_t ret = 0;
    for (std::size_t i = 0; i < 10000; ++i) {
        ret += x * 2;
        ret /= y;
    }
    return ret;
};
template <typename T, std::size_t level>
auto test_mul(const T a[level][level], const T b[level][level], T r[level][level]) {
    for (std::size_t i = 0; i < level; ++i) {
        for (std::size_t j = 0; j < level; ++j) {
            r[i][j] = a[i][j] * b[i][j];
        }
    }
}

void BM_Optimize(benchmark::State& state) {
    for (auto _ : state) {
        auto ret = test_func(1.1, 2.1);
        benchmark::DoNotOptimize(ret);
    }
}
void BM_Optimize2(benchmark::State& state) {
    for (auto _ : state) {
        auto ret = test_func2(7, 13);
        benchmark::DoNotOptimize(ret);
    }
}

template <typename T, std::size_t level>
static void BM_TestMul(benchmark::State& state) {
    T a[level][level];
    T b[level][level];
    T r[level][level];
    auto gen = Fundamental::DefaultNumberGenerator<T>();
    for (std::size_t i = 0; i < level; ++i) {
        for (std::size_t j = 0; j < level; ++j) {
            a[i][j] = gen();
            b[i][j] = gen();
        }
    }

    for (auto _ : state) {
        test_mul<T, level>(a, b, r);
        benchmark::DoNotOptimize(r); // 防止编译器优化掉计算
    }
}
BENCHMARK(BM_Optimize);
BENCHMARK(BM_Optimize2);
// 注册测试
BENCHMARK_TEMPLATE(BM_TestMul, std::int32_t, 1);
BENCHMARK_TEMPLATE(BM_TestMul, std::int32_t, 4);
BENCHMARK_TEMPLATE(BM_TestMul, std::int32_t, 8);
BENCHMARK_TEMPLATE(BM_TestMul, std::int32_t, 16);
BENCHMARK_TEMPLATE(BM_TestMul, std::int32_t, 23);

BENCHMARK_TEMPLATE(BM_TestMul, std::int64_t, 1);
BENCHMARK_TEMPLATE(BM_TestMul, std::int64_t, 4);
BENCHMARK_TEMPLATE(BM_TestMul, std::int64_t, 8);
BENCHMARK_TEMPLATE(BM_TestMul, std::int64_t, 16);
BENCHMARK_TEMPLATE(BM_TestMul, std::int64_t, 23);
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
