//
// @author lightning1993 <469953258@qq.com> 2025/08
//
#pragma once
#if defined(_WIN32)
#else
    #include <endian.h>
#endif

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Fundamental
{
enum class Endian : std::uint8_t
{
    None = 0,
    LittleEndian,
    BigEndian
};
inline constexpr auto kHostEndian =
#if defined(_WIN32)
    Endian::LittleEndian;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    Endian::LittleEndian;
#else
    Endian::BigEndian;
#endif

template <Endian targetEndian>
inline constexpr bool NeedConvertEndian() {
    return targetEndian != kHostEndian;
}

inline constexpr auto kNeedConvertForTransfer = kHostEndian == Endian::BigEndian;
template <typename T>
union type_operation_buffer {
    T v;
    std::uint8_t b[sizeof(T)];
};

template <typename T>
inline constexpr T bswap_internal(T value) noexcept {
    static_assert(std::is_integral_v<T>, "Only integer types are supported");
    type_operation_buffer<T> input { value };
    type_operation_buffer<T> output  = {};
    constexpr std::size_t kValueSize = sizeof(T);
    for (std::size_t i = 0; i < kValueSize; ++i)
        output.b[i] = input.b[kValueSize - 1 - i];
    return output.v;
}

template <typename T, Endian another_endian = Endian::LittleEndian, typename = std::enable_if_t<std::is_integral_v<T>>>
inline constexpr std::decay_t<T> host_value_convert(T value) {
    if constexpr (NeedConvertEndian<another_endian>()) {
        return bswap_internal<std::decay_t<T>>(value);
    } else {
        return value;
    }
}

template <Endian another_endian = Endian::LittleEndian>
inline void net_buffer_copy(const void* src_buf, void* dst_buf, std::size_t buf_len) noexcept {
    if constexpr (NeedConvertEndian<another_endian>()) {
        std::memcpy(dst_buf, src_buf, buf_len);
    } else {
        auto p_src_buf = static_cast<const std::uint8_t*>(src_buf);
        auto p_dst_buf = static_cast<std::uint8_t*>(dst_buf);
        for (std::size_t i = 0; i < buf_len; ++i)
            p_dst_buf[i] = p_src_buf[buf_len - 1 - i];
    }
}

} // namespace Fundamental