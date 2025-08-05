#include "parallel.hpp"
#include <atomic>
#include <string>

namespace Fundamental::internal
{
static void InitParallelThreadPool() {
    std::size_t threadNums = std::thread::hardware_concurrency();
    try {
        auto ptr = ::getenv("F_PARALLEL_THREADS");
        if (ptr) {
            auto value = std::max(static_cast<std::size_t>(1), static_cast<std::size_t>(std::stoul(ptr)));
            threadNums = std::min(threadNums, value);
        }
    } catch (const std::exception&) {
    }
    auto& pool = GetParallelThreadPool();
    ThreadPoolConfig config;
    config.max_threads_limit    = threadNums;
    config.min_work_threads_num = 2;
    config.ilde_wait_time_ms    = 10000;
    pool.InitThreadPool(config);
}

void _InitThreadPool() {
    static std::once_flag flag;
    std::call_once(flag, InitParallelThreadPool);
}
} // namespace Fundamental::internal
