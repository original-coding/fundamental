
#include "fundamental/algorithm/blake3_hasher_utils.hpp"
#include "fundamental/algorithm/hash.hpp"
#include <gtest/gtest.h>
constexpr std::size_t kTestCnt = 10000;
const std::string test_str(kTestCnt, 'a');
TEST(Blake3HashTest, hasher) {
    using DataType = std::size_t;
    std::set<DataType> storage;
    for (std::size_t i = 1; i <= kTestCnt; ++i) {
        EXPECT_TRUE(storage.insert(Fundamental::blake3_hasher::hash<DataType>(test_str.data(), i)).second);
    }
}

TEST(WyHashTest, hasher) {
    using DataType = std::size_t;
    std::set<DataType> storage;
    for (std::size_t i = 1; i <= kTestCnt; ++i) {
        EXPECT_TRUE(storage.insert(Fundamental::Hash(test_str.data(), i)).second);
    }
}

TEST(StlHashTest, hasher) {
    using DataType = std::size_t;
    std::set<DataType> storage;
    std::hash<std::string_view> hasher;
    for (std::size_t i = 1; i <= kTestCnt; ++i) {
        EXPECT_TRUE(storage.insert(hasher(std::string_view(test_str.data(), i))).second);
    }
}

TEST(black3_hasher, empty_test) {
    std::vector<std::uint8_t> tmp1, tmp2;
    tmp1.resize(Fundamental::blake3_hasher::kMaxBlake3HashLength);
    tmp2.resize(Fundamental::blake3_hasher::kMaxBlake3HashLength);
    Fundamental::blake3_hasher::hash(nullptr, 0, tmp1.data(), tmp1.size());
    Fundamental::blake3_hasher::hash(nullptr, 0, tmp2.data(), tmp2.size());
    EXPECT_EQ(tmp1, tmp2);
}

TEST(WyHashTest, empty_test) {
    EXPECT_EQ(Fundamental::Hash(nullptr, 0), Fundamental::Hash(nullptr, 0));
}

int main(int argc, char** argv) {

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}