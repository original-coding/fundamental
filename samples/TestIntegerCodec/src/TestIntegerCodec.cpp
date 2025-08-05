
#include "fundamental/basic/integer_codec.hpp"
#include "fundamental/basic/log.h"
#include <chrono>
#include <iostream>
#include <memory>

#include <gtest/gtest.h>
using namespace Fundamental;
template <typename T>
class IntegerCodecTest : public ::testing::Test {
public:
    constexpr static bool Signed = std::is_signed_v<T>;
    constexpr static T kMin      = std::numeric_limits<T>::min();
    constexpr static T kMax      = std::numeric_limits<T>::max();
};

using AllTestTypes = ::testing::Types<std::uint8_t,
                                      std::uint16_t,
                                      std::uint32_t,
                                      std::uint64_t,
                                      std::int8_t,
                                      std::int16_t,
                                      std::int32_t,
                                      std::int64_t>;
// declare
TYPED_TEST_SUITE(IntegerCodecTest, AllTestTypes);
TEST(IntegerCodecTestBool, TestBasic) {
    std::uint8_t src[1] = {};
    {
        bool encode_v = false;
        auto len      = VarintEncode(encode_v, src);
        bool decode_v = 0;
        auto len2     = VarintDecode(decode_v, src);
        EXPECT_EQ(len, len2);
        EXPECT_EQ(decode_v, encode_v);
    }
    {
        bool encode_v = true;
        auto len      = VarintEncode(encode_v, src);
        bool decode_v = 0;
        auto len2     = VarintDecode(decode_v, src);
        EXPECT_EQ(len, len2);
        EXPECT_EQ(decode_v, encode_v);
    }
}

TYPED_TEST(IntegerCodecTest, TestAllNumberType) {
    constexpr std::size_t max_extra_bytes                  = sizeof(TestFixture::kMin);
    std::uint8_t src[max_extra_bytes + 1];
    std::memset(src,0,max_extra_bytes + 1);
    using ValueType                       = std::decay_t<decltype(TestFixture::kMin)>;
    if constexpr (TestFixture::Signed)
    { // test len
        ValueType encode_v = 0;
        for (std::size_t shift = 0; shift < sizeof(ValueType) * 8 - 1; ++shift)
        {
            encode_v += static_cast <ValueType>(1) << shift;
            auto len           = VarintEncode(encode_v, src);
            ValueType decode_v = 0;
            auto len2          = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, encode_v);
        }
        encode_v = 0;
        for (std::size_t shift = 0; shift < sizeof(ValueType) * 8 - 1; ++shift)
        {
            encode_v += static_cast<ValueType>(-1) * static_cast<ValueType>(std::pow(2, shift));
            auto len           = VarintEncode(encode_v, src);
            ValueType decode_v = 0;

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, encode_v);
        }
        enum Test_Enum : ValueType
        {
            Test_Enum_V0 = TestFixture::kMin,
            Test_Enum_V1 = 1,
            Test_Enum_V2 = -1,
            Test_Enum_V3 = TestFixture::kMax,
        };
        {
            auto enum_v               = Test_Enum::Test_Enum_V0;
            auto len                  = VarintEncode(enum_v, src);
            decltype(enum_v) decode_v = static_cast<Test_Enum>(0);

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, enum_v);
        }
        {
            auto enum_v               = Test_Enum::Test_Enum_V0;
            auto len                  = VarintEncode(enum_v, src);
            decltype(enum_v) decode_v = static_cast<Test_Enum>(0);

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, enum_v);
        }
        {
            auto enum_v               = Test_Enum::Test_Enum_V1;
            auto len                  = VarintEncode(enum_v, src);
            decltype(enum_v) decode_v = static_cast<Test_Enum>(0);

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, enum_v);
        }
        {
            auto enum_v               = Test_Enum::Test_Enum_V2;
            auto len                  = VarintEncode(enum_v, src);
            decltype(enum_v) decode_v = static_cast<Test_Enum>(0);

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, enum_v);
        }
        {
            auto enum_v               = Test_Enum::Test_Enum_V3;
            auto len                  = VarintEncode(enum_v, src);
            decltype(enum_v) decode_v = static_cast<Test_Enum>(0);

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, enum_v);
        }
    }
    else
    {
        ValueType encode_v = 0;
        for (std::size_t shift = 0; shift < sizeof(ValueType) * 8; ++shift)
        {
            encode_v += static_cast <ValueType>(1) << shift;
            auto len = VarintEncode(encode_v, src);

            ValueType decode_v = 0;
            auto len2          = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, encode_v);
        }
        enum Test_Enum : ValueType
        {
            Test_Enum_V0 = TestFixture::kMin,
            Test_Enum_V1 = 1,
            Test_Enum_V2 = (TestFixture::kMin / 2 + TestFixture::kMax / 2),
            Test_Enum_V3 = TestFixture::kMax,
        };
        {
            auto enum_v               = Test_Enum::Test_Enum_V0;
            auto len                  = VarintEncode(enum_v, src);
            decltype(enum_v) decode_v = static_cast<Test_Enum>(0);

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, enum_v);
        }
        {
            auto enum_v               = Test_Enum::Test_Enum_V0;
            auto len                  = VarintEncode(enum_v, src);
            decltype(enum_v) decode_v = static_cast<Test_Enum>(0);

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, enum_v);
        }
        {
            auto enum_v               = Test_Enum::Test_Enum_V1;
            auto len                  = VarintEncode(enum_v, src);
            decltype(enum_v) decode_v = static_cast<Test_Enum>(0);

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, enum_v);
        }
        {
            auto enum_v               = Test_Enum::Test_Enum_V2;
            auto len                  = VarintEncode(enum_v, src);
            decltype(enum_v) decode_v = static_cast<Test_Enum>(0);

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, enum_v);
        }
        {
            auto enum_v               = Test_Enum::Test_Enum_V3;
            auto len                  = VarintEncode(enum_v, src);
            decltype(enum_v) decode_v = static_cast<Test_Enum>(0);

            auto len2 = VarintDecode(decode_v, src);
            EXPECT_EQ(len, len2);
            EXPECT_EQ(decode_v, enum_v);
        }
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}