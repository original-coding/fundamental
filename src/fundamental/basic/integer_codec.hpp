#pragma once
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace Fundamental
{

template <typename T, typename = std::enable_if_t<std::is_signed_v<T>>>
struct PairSignedWithUnsignedType {
    using SignedType                   = T;
    using UnsignedType                 = std::make_unsigned_t<T>;
    static constexpr std::size_t shift = sizeof(T) * 8 - 1;
};

template <typename T, typename = std::enable_if_t<std::is_signed_v<T>>>
[[nodiscard]] inline decltype(auto) ZigZagEncode(T value) noexcept {
    return static_cast<typename PairSignedWithUnsignedType<T>::UnsignedType>(
        (value << 1) ^ (value >> PairSignedWithUnsignedType<T>::shift));
}
template <typename T, typename = std::enable_if_t<std::is_signed_v<T>>>
[[nodiscard]] inline T ZigZagDecode(typename PairSignedWithUnsignedType<T>::UnsignedType value) noexcept {
    return static_cast<T>((value >> 1) ^ -static_cast<typename PairSignedWithUnsignedType<T>::SignedType>(value & 1));
}
template <typename T,
          typename = std::enable_if_t<std::disjunction_v<std::is_integral<T>, std::is_enum<T>, std::is_same<T, bool>>>>
[[nodiscard]] constexpr inline std::size_t VarintEncodeGuessMaxSize() noexcept {
    return sizeof(T) + 1;
}

template <typename T, typename = std::void_t<>>
struct SelectUnderlyingType {
    using type = T;
};

template <typename T>
struct SelectUnderlyingType<T, std::void_t<std::enable_if_t<std::is_enum<T>::value>>> {
    using type = std::underlying_type_t<T>;
};

template <typename T,
          typename = std::enable_if_t<std::disjunction_v<std::is_integral<T>, std::is_enum<T>, std::is_same<T, bool>>>>
[[nodiscard]] inline std::size_t VarintEncode(T inValue, std::uint8_t* dst) noexcept {
    using UnderlyingType = typename SelectUnderlyingType<T>::type;
    if constexpr (sizeof(UnderlyingType) == 1) {
        dst[0] = static_cast<std::uint8_t>(inValue);
        return 1;
    }
    else
    {
        if constexpr (std::is_signed_v<UnderlyingType>) {
            return VarintEncode(ZigZagEncode(static_cast<UnderlyingType>(inValue)), dst);
        }
        else
        {
            std::size_t extra_bytes = 0;
            if constexpr (std::is_same_v<UnderlyingType, T>) {
                dst[extra_bytes] = static_cast<std::uint8_t>(inValue & 0xf);
                inValue          = inValue >> 4;
                while (inValue > 0) {
                    ++extra_bytes;
                    dst[extra_bytes] = static_cast<std::uint8_t>(inValue & 0xff);
                    inValue          = inValue >> 8;
                }
            } else {
                UnderlyingType opValue = static_cast<UnderlyingType>(inValue);
                dst[extra_bytes]       = static_cast<std::uint8_t>(opValue & 0xf);
                opValue                = opValue >> 4;
                while (opValue > 0) {
                    ++extra_bytes;
                    dst[extra_bytes] = static_cast<std::uint8_t>(opValue & 0xff);
                    opValue          = opValue >> 8;
                }
            }
            dst[0] |= (static_cast<std::uint8_t>(extra_bytes)) << 4;
            return extra_bytes + 1;
        }
    }
}

[[nodiscard]] inline std::size_t VarintDecodePeekSize(const std::uint8_t* src) noexcept {
    std::size_t extra_bytes = src[0] >> 4;
    return extra_bytes + 1;
}

template <typename T>
[[nodiscard]] inline bool VarintDecodeCheckSize(const std::uint8_t* src, std::size_t len) noexcept {
    if constexpr (sizeof(T) == 1) return len >= 1;
    else {
        if (len == 0) return false;
        std::size_t extra_bytes = src[0] >> 4;
        return len >= (extra_bytes + 1);
    }
}

template <typename T,
          typename = std::enable_if_t<std::disjunction_v<std::is_integral<T>, std::is_enum<T>, std::is_same<T, bool>>>>
[[nodiscard]] inline std::size_t VarintDecode(T& outValue, const std::uint8_t* src) noexcept {
    using UnderlyingType = typename SelectUnderlyingType<T>::type;

    if constexpr (sizeof(UnderlyingType) == 1) {
        outValue = static_cast<T>(src[0]);
        return 1;
    }
    else
    {
        if constexpr (std::is_signed_v<UnderlyingType>) {
            typename PairSignedWithUnsignedType<UnderlyingType>::UnsignedType tmp;
            auto ret = VarintDecode(tmp, src);
            outValue = static_cast<T>(ZigZagDecode<UnderlyingType>(tmp));
            return ret;
        } else {
            std::size_t extra_bytes = src[0] >> 4;
            assert(extra_bytes <= sizeof(UnderlyingType) && "invalid decode stc data");
            if constexpr (std::is_same_v<UnderlyingType, T>) {
                outValue          = 0;
                outValue          = static_cast<T>(src[0] & 0xf);
                std::size_t shift = 4;
                src += 1;
                for (std::size_t i = 0; i < extra_bytes; ++i) {
                    outValue |= static_cast<T>(src[i]) << shift;
                    shift += 8;
                }
            } else {
                UnderlyingType tmpValue = 0;
                tmpValue                = static_cast<UnderlyingType>(src[0] & 0xf);
                std::size_t shift       = 4;
                src += 1;
                for (std::size_t i = 0; i < extra_bytes; ++i) {
                    tmpValue |= static_cast<UnderlyingType>(src[i]) << shift;
                    shift += 8;
                }
                outValue = static_cast<T>(tmpValue);
            }
            return extra_bytes + 1;
        }
    }
}

} // namespace Fundamental