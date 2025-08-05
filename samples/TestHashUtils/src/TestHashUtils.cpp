
#include "fundamental/algorithm/hash.hpp"
#include <gtest/gtest.h>
// Demonstrate some basic assertions.
TEST(HashTest, basic) {
    constexpr std::size_t kTestCnt = 10000;
    std::string s(kTestCnt, 'a');
    std::set<std::size_t> storage;
    for (std::size_t i = 0; i < kTestCnt; ++i) {
        EXPECT_TRUE(storage.insert(Fundamental::Hash(s.data(), i + 1)).second);
    }
    std::vector<char> s_v(kTestCnt, 'a');
    for (std::size_t i = 0; i < kTestCnt; ++i) {
        EXPECT_FALSE(storage.insert(Fundamental::Hash(s_v)).second);
    }
}

int main(int argc, char** argv) {

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}