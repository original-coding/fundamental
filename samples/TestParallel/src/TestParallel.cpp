
#include "fundamental/basic/log.h"
#include "fundamental/basic/parallel.hpp"
#include "fundamental/delay_queue/delay_queue.h"
#include "fundamental/thread_pool/thread_pool.h"
#include <cmath>
#include <gtest/gtest.h>
#include <list>
#include <vector>
#if TARGET_PLATFORM_WINDOWS
#include <windows.h>
#endif
namespace
{
struct TestParallel : public ::testing::Test
{
protected:
    void SetUp() override
    {
    }
};
} // namespace

Fundamental::ThreadPoolParallelExecutor s_excutor{8};
TEST_F(TestParallel, TestVecAccess)
{
    std::vector<std::int32_t> nums;
    nums.resize(1000);
    std::size_t sum = (0 + 999) * 1000 / 2;
    for (std::size_t i = 0; i < 1000; ++i)
    {
        nums[i] = static_cast<std::int32_t>(i);
    }
    std::atomic<std::size_t> pSum { 0 };
    Fundamental::ParallelRun(
        nums.begin(), nums.end(), [&](decltype(nums.begin()) begin, std::size_t groupSize, std::size_t groupIndex) {
            EXPECT_LE(groupSize, 10);
            std::size_t sum_value = 0;
            for (std::size_t i = 0; i < groupSize; ++i)
            {
                sum_value += *begin;
                ++begin;
            }
            pSum += sum_value;
        },
        10,s_excutor);
    EXPECT_EQ(pSum.load(), sum);
}

TEST_F(TestParallel, TestListAccess)
{
    std::list<std::int32_t> nums;
    nums.resize(1000);
    auto iter       = nums.begin();
    std::size_t sum = (0 + 999) * 1000 / 2;
    for (std::size_t i = 0; i < 1000; ++i)
    {
        *iter = static_cast<std::int32_t>(i);
        ++iter;
    }
    std::atomic<std::size_t> pSum { 0 };
    Fundamental::ParallelRun(
        nums.begin(), nums.end(), [&](decltype(nums.begin()) begin, std::size_t groupSize, std::size_t groupIndex) {
            EXPECT_LE(groupSize, 10);
            std::size_t sum_value = 0;
            for (std::size_t i = 0; i < groupSize; ++i)
            {
                sum_value += *begin;
                ++begin;
            }
            pSum += sum_value;
        },
        10,s_excutor);
    EXPECT_EQ(pSum.load(), sum);
}

TEST_F(TestParallel, TestRawPtr)
{
    std::size_t size  = 11;
    std::int32_t* ptr = new std::int32_t[size];
    for (std::size_t i = 0; i < size; ++i)
    {
        ptr[i] = static_cast<std::int32_t>(i);
    }
    std::atomic<std::int32_t> pSum = 0;
    Fundamental::ParallelRun(
        ptr, ptr + size, [&](std::int32_t* p, std::size_t groupSize, std::size_t groupIndex) {
            std::int32_t sum_value = 0;
            for (std::size_t i = 0; i < groupSize; ++i)
            {
                sum_value += *p;
                ++p;
            }
            FDEBUG("sum_value:{} groupSize:{} groupIndex:{}", sum_value, groupSize, groupIndex);
            pSum += sum_value;
        },
        2,s_excutor);
    EXPECT_EQ(pSum.load(), 55);
    delete[] ptr;
}

TEST_F(TestParallel, TestException)
{
    std::vector<std::int32_t> nums;
    nums.resize(1000);
    for (std::size_t i = 0; i < 1000; ++i)
    {
        nums[i] = static_cast<std::int32_t>(i);
    }
    EXPECT_ANY_THROW(Fundamental::ParallelRun(
        nums.begin(), nums.end(), [&](decltype(nums.begin()) begin, std::size_t groupSize, std::size_t groupIndex) {
            throw std::invalid_argument("test");
        },
        10,s_excutor));
}

TEST_F(TestParallel, TestEnv)
{
    std::vector<std::int32_t> nums;
    nums.resize(1000);
    for (std::size_t i = 0; i < 1000; ++i)
    {
        nums[i] = static_cast<std::int32_t>(i);
    }
    Fundamental::ParallelRun(
        nums.begin(), nums.end(), [&](decltype(nums.begin()) begin, std::size_t groupSize, std::size_t groupIndex) {

        },
        10,s_excutor);
}

TEST_F(TestParallel, BenchMark)
{
    std::size_t calcNums = 100000000; // 1 billion
    std::atomic<double> result { 0.0 };
    auto sum = [&](std::size_t begin, std::size_t groupSize, std::size_t groupIndex) {
        double tmp = 0;
        while (groupSize > 0)
        {
            tmp += (std::sqrt(begin) + std::sqrt(begin * 2));
            --groupSize;
            ++begin;
        }
        result = result + tmp;
    };
    Fundamental::Timer t;
    t.Reset();
    Fundamental::ParallelRun(
        (std::size_t)0, calcNums, sum,
        calcNums,s_excutor);
    result.exchange(0.0);
    auto costTime = t.GetDuration<Fundamental::Timer::TimeScale::Millisecond>();
    FDEBUG("run for total:{} groupsize:{} cost {}[ms]", calcNums, calcNums, costTime);
    t.Reset();
    Fundamental::ParallelRun(
        (std::size_t)0, calcNums, sum,
        calcNums / 10,s_excutor);
    result.exchange(0.0);
    auto costTime2 = t.GetDuration<Fundamental::Timer::TimeScale::Millisecond>();
    FDEBUG("run for total:{} groupsize:{} cost {}[ms]", calcNums, calcNums / 10, costTime2);
    EXPECT_GT(costTime, costTime2);
}

int main(int argc, char** argv)
{

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}