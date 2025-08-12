#pragma once
#include <algorithm>
#include <future>
#include <vector>

namespace Fundamental
{

struct DefaultParallelExecutor {
    DefaultParallelExecutor() = default;
    template <typename Callable, typename... Args>
    auto execute(Callable&& f, Args&&... args) const -> std::future<std::invoke_result_t<Callable, Args...>> {
        return std::async(std::forward<Callable>(f), std::forward<Args>(args)...);
    }
};

/// @brief parallel process task,it will throw exception
/// @tparam Iterator iterator type for input datasheet
/// @tparam ProcessF  with singature void(Iterator input,std::size_t dataSize,std::size_t groupIndex)
/// @param inputIt start iterator
/// @param endIt end iterrator which is reachable for inputIt with + operation
/// @param f date process function
/// @param groupSize partition datasheet size
template <typename Iterator, typename ProcessF, typename Executor = DefaultParallelExecutor>
inline void ParallelRun(Iterator inputIt,
                        Iterator endIt,
                        ProcessF f,
                        std::size_t groupSize    = 1,
                        const Executor& executor = Executor {}) {
    std::size_t total_size = 0;
    if constexpr (std::is_integral_v<std::decay_t<Iterator>>) {
        total_size = endIt - inputIt;
    } else {
        total_size = std::abs(std::distance(inputIt, endIt));
    }
    auto groupNums = (total_size + groupSize - 1) / groupSize;
    std::vector<std::future<void>> tasks;
    auto taskFunc = [&](Iterator begin, std::size_t nums, std::size_t groupIndex) -> void {
        f(begin, nums, groupIndex);
    };
    // enqueue into thread pool
    if (groupNums > 1) {
        tasks.resize(groupNums - 1);
        auto moveIt = [](Iterator begin, std::size_t offset) -> decltype(auto) {
            if constexpr (std::is_integral_v<std::decay_t<Iterator>>) {
                return begin + offset;
            } else if constexpr (std::is_same_v<typename std::iterator_traits<Iterator>::iterator_category,
                                                std::random_access_iterator_tag>) {
                return begin + offset;
            } else {
                while (offset > 0) {
                    ++begin;
                    --offset;
                }

                return begin;
            }
        };
        std::size_t offset = groupSize;
        auto beginIt       = moveIt(inputIt, offset);
        for (std::size_t i = 0; offset < total_size; ++i) {
            auto currentGroupSize = (total_size - offset) > groupSize ? groupSize : (total_size - offset);
            tasks[i]              = std::move(executor.execute(std::bind(taskFunc, beginIt, currentGroupSize, i + 1)));
            offset += currentGroupSize;
            beginIt = moveIt(beginIt, currentGroupSize);
        }
    }

    std::exception_ptr eptr;
    try {
        // Run the first task on the current thread directly.
        if (!tasks.empty())
            taskFunc(inputIt, groupSize, 0);
        else {
            taskFunc(inputIt, total_size, 0);
        }
    } catch (...) {
        eptr = std::current_exception();
    }

    // Wait for all tasks to finish.
    for (auto& item : tasks) {
        try {
            item.get();
        } catch (...) {
            eptr = std::current_exception();
        }
    }

    if (eptr) {
        std::rethrow_exception(eptr);
    }
}
} // namespace Fundamental