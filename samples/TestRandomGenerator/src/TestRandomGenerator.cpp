
#include "fundamental/basic/log.h"
#include "fundamental/basic/random_generator.hpp"
#include <chrono>
#include <iostream>
#include <memory>

#include <gtest/gtest.h>

template <typename T>
class RandomTypeTest : public ::testing::Test {
public:
    using TestType          = T;
    constexpr static bool Signed = std::is_signed_v<T>;
    constexpr static T kMin = static_cast<T>(10 * (Signed ? (-1) : 1));
    constexpr static T kMax = static_cast<T>(20);
    RandomTypeTest() : gen(Fundamental::RandomGenerator<T>(Fundamental::DefaultGenerator(), kMin, kMax)) {
    }

protected:
    Fundamental::RandomGenerator<T> gen;
};

using AllTestTypes = ::testing::Types<std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t, std::int8_t,
                                      std::int16_t, std::int32_t, std::int64_t>;
// declare
TYPED_TEST_SUITE(RandomTypeTest, AllTestTypes);

TYPED_TEST(RandomTypeTest, TestAllNumberType) {
    for (std::size_t i = 0; i < 20; ++i) {
        auto value =this->gen();
        EXPECT_GE(value, this->kMin);
        EXPECT_LE(value, this->kMax);
    }
}

TEST(TestRandomGenerator, TestBasic) {
    using ValueType               = std::int32_t;
    ValueType kMin                = 0;
    ValueType kMax                = 10;
    auto defaultGen               = Fundamental::DefaultGenerator();
    const auto& cDefaultGen       = defaultGen;
    auto defaultNumberGen         = Fundamental::DefaultNumberGenerator(kMin, kMax);
    const auto& cdefaultNumberGen = defaultNumberGen;
    {
        Fundamental::RandomGenerator<ValueType> t(std::move(defaultGen), kMin, kMax);
        auto value = t.gen();
        EXPECT_GE(value, kMin);
        EXPECT_LE(value, kMax);
    }
    {
        Fundamental::RandomGenerator<ValueType> t(cDefaultGen, kMin, kMax);
        auto value = t.gen();
        EXPECT_GE(value, kMin);
        EXPECT_LE(value, kMax);
    }
    {
        // copy
        Fundamental::RandomGenerator<ValueType> t(cdefaultNumberGen);
        auto value = t.gen();
        EXPECT_GE(value, kMin);
        EXPECT_LE(value, kMax);
        Fundamental::RandomGenerator<ValueType> t2 = t;
        value                                      = t2.gen();
        EXPECT_GE(value, kMin);
        EXPECT_LE(value, kMax);
    }
    {
        // move
        Fundamental::RandomGenerator<ValueType> t(std::move(defaultNumberGen));
        auto value = t.gen();
        EXPECT_GE(value, kMin);
        EXPECT_LE(value, kMax);
        Fundamental::RandomGenerator<ValueType> t2(std::move(t));
        value = t2.gen();
        EXPECT_GE(value, kMin);
        EXPECT_LE(value, kMax);
    }
}

TEST(TestRandomGenerator, TestGen) {
    using ValueType = std::int32_t;
    ValueType kMin  = 0;
    ValueType kMax  = 10;
    auto generator  = Fundamental::DefaultNumberGenerator(kMin, kMax);
    { // operator ()
        auto value = generator();
        EXPECT_GE(value, kMin);
        EXPECT_LE(value, kMax);
    }
    { // gen()
        auto value = generator.gen();
        EXPECT_GE(value, kMin);
        EXPECT_LE(value, kMax);
    }
    { // gen array
        ValueType dst[2];
        generator.gen(dst, 2);
        EXPECT_GE(dst[0], kMin);
        EXPECT_LE(dst[0], kMax);
        EXPECT_GE(dst[1], kMin);
        EXPECT_LE(dst[1], kMax);
    }
    { // multipe_gen
        using TestMultiType       = float;
        TestMultiType numerator   = 1.f;
        TestMultiType denominator = 3.f;
        auto value                = generator.multipe_gen(numerator, denominator);
        EXPECT_GE(value, kMin * numerator / denominator);
        EXPECT_LE(value, kMax * numerator / denominator);
    }
    { // multipe_gen arrry
        using TestMultiType       = float;
        TestMultiType numerator   = 1.f;
        TestMultiType denominator = 3.f;
        TestMultiType dst[2];
        generator.multipe_gen(dst, 2, numerator, denominator);
        EXPECT_GE(dst[0], kMin * numerator / denominator);
        EXPECT_LE(dst[0], kMax * numerator / denominator);
        EXPECT_GE(dst[1], kMin * numerator / denominator);
        EXPECT_LE(dst[1], kMax * numerator / denominator);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}