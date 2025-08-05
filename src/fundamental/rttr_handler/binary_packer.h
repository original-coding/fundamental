#pragma once

#include <cstdlib>
#include <cstring>

#include "fundamental/basic/buffer.hpp"
#include "fundamental/basic/integer_codec.hpp"
#include "fundamental/basic/log.h"
#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 26819 26437 26439 26495 26800 26498)
#endif
#include <rttr/type>
#ifdef _MSC_VER
    #pragma warning(pop)
#endif
namespace Fundamental::io
{
using BinaryPackSizeType=std::uint64_t;
// throw when pack failed
template <typename T>
inline void binary_pack(std::vector<std::uint8_t>& out, const T& in);
template <typename T>
[[nodiscard]] inline std::vector<std::uint8_t> binary_pack(const T& in);
template <typename Tuple, typename = decltype(std::tuple_size<Tuple>::value)>
inline void binary_pack_tuple(std::vector<std::uint8_t>& out, const Tuple& in);
template <typename Tuple, typename = decltype(std::tuple_size<Tuple>::value)>
[[nodiscard]] inline std::vector<std::uint8_t> binary_pack_tuple(const Tuple& in);
template <typename... Args>
inline void binary_batch_pack(std::vector<std::uint8_t>& out, const Args&... args);
template <typename... Args>
[[nodiscard]] inline std::vector<std::uint8_t> binary_batch_pack(const Args&... args);
// return false when unpack failed
template <typename T>
inline bool binary_unpack(const void* data,
                          std::size_t len,
                          T& out,
                          bool ignore_invalid_properties = true,
                          std::size_t index              = 0);
template <typename Tuple, typename = decltype(std::tuple_size<Tuple>::value)>
inline bool binary_unpack_tuple(const void* data,
                                std::size_t len,
                                Tuple& t,
                                bool ignore_invalid_properties = true,
                                std::size_t index              = 0);
template <typename... Args>
inline bool binary_bacth_unpack(const void* data,
                                std::size_t len,
                                bool ignore_invalid_properties,
                                std::size_t index,
                                Args&&... args);

enum PackerDataType : std::uint8_t
{
    unknown_pack_data,
    bool_pack_data,
    char_pack_data,
    int8_pack_data,
    int16_pack_data,
    int32_pack_data,
    int64_pack_data,
    uint8_pack_data,
    uint16_pack_data,
    uint32_pack_data,
    uint64_pack_data,
    float_pack_data,
    double_pack_data,
    string_pack_data,
    enum_pack_data,
    array_pack_data,
    set_pack_data,
    map_pack_data,
    object_pack_data,
    custom_object_pack_data,
};

namespace internal
{
void do_binary_pack(const rttr::variant& var, std::vector<std::uint8_t>& out, bool& type_flag);
bool do_binary_unpack(const std::uint8_t*& data,
                      std::size_t& len,
                      rttr::variant& var,
                      rttr::instance dst_obj,
                      PackerDataType &type,
                      bool ignore_invalid_properties);
bool binary_unpack_skip_item(const std::uint8_t*& data, std::size_t& len);

template <typename T>
inline bool unpack_basic_fixed_value(const std::uint8_t*& data, std::size_t& len, T& value) {
    constexpr std::size_t value_size = sizeof(value);
    if (len < value_size)
    {
        FERR("read basic value failed");
        return false;
    }

    if constexpr (value_size > 1 && Fundamental::NeedConvertEndian<Fundamental::Endian::LittleEndian>())
    {
        std::uint8_t* p_dst = (std::uint8_t*)(&value);
        for (std::size_t i = 0; i < value_size; ++i)
        {
            p_dst[value_size - 1 - i] = data[i];
        }
    }
    else
    {
        std::memcpy(&value, data, value_size);
    }
    data = data + value_size;
    len -= value_size;
    return true;
}

template <typename T>
inline bool unpack_basic_varint_value(const std::uint8_t*& data, std::size_t& len, T& value) {
    if (!Fundamental::VarintDecodeCheckSize<T>(data, len))
    {
        FERR("read basic value failed");
        return false;
    }

    auto peek_size = Fundamental::VarintDecode(value, data);
    data           = data + peek_size;
    len -= peek_size;
    return true;
}

template <typename T>
inline bool binary_unpack_helper(const std::uint8_t*& data,
                                 std::size_t& len,
                                 T& out,
                                 bool ignore_invalid_properties = true,
                                 std::size_t index              = 0) {
    auto type = rttr::type::get<T>();
    if (!type.is_valid()) return false;
    while (index != 0)
    {
        if (!internal::binary_unpack_skip_item(data, len)) return false;
        --index;
    }
    rttr::variant var(out);
    PackerDataType data_type = unknown_pack_data;
    if (!internal::unpack_basic_varint_value(data, len, data_type)) return false;
    if (!internal::do_binary_unpack(data, len, var, out, data_type, ignore_invalid_properties)) return false;
    if (data_type != object_pack_data) out = var.get_value<T>();
    return true;
}

template <std::size_t Index = 0, typename Tuple, typename = decltype(std::tuple_size<Tuple>::value)>
inline void binary_pack_tuple_helper(std::vector<std::uint8_t>& out, const Tuple& in) {
    if constexpr (Index < std::tuple_size<Tuple>::value)
    {
        binary_pack(out, std::get<Index>(in));
        binary_pack_tuple_helper<Index + 1>(out, in);
    }
}

template <std::size_t Index = 0, typename Tuple, typename = decltype(std::tuple_size<Tuple>::value)>
inline bool binary_unpack_tuple_helper(const std::uint8_t*& data,
                                       std::size_t& len,
                                       Tuple& t,
                                       bool ignore_invalid_properties) {
    if constexpr (Index < std::tuple_size<Tuple>::value)
    {
        auto& item = std::get<Index>(t);
        if (!binary_unpack_helper(data, len, item, ignore_invalid_properties)) return false;
        return binary_unpack_tuple_helper<Index + 1>(data, len, t, ignore_invalid_properties);
    } else {
        return true;
    }
    
}

} // namespace internal

template <typename T>
inline void binary_pack(std::vector<std::uint8_t>& out, const T& in) {
    bool flag = false;
    internal::do_binary_pack(in, out, flag);
}

template <typename T>
[[nodiscard]] inline std::vector<std::uint8_t> binary_pack(const T& in) {
    std::vector<std::uint8_t> out;
    binary_pack(out, in);
    return out;
}

template <typename Tuple, typename V>
inline void binary_pack_tuple(std::vector<std::uint8_t>& out, const Tuple& in) {
    internal::binary_pack_tuple_helper<0>(out, in);
}

template <typename Tuple, typename V>
[[nodiscard]] inline std::vector<std::uint8_t> binary_pack_tuple(const Tuple& in) {
    std::vector<std::uint8_t> out;
    internal::binary_pack_tuple_helper<0>(out, in);
    return out;
}

template <typename... Args>
inline void binary_batch_pack(std::vector<std::uint8_t>& out, const Args&... args) {
    binary_pack_tuple(out, std::forward_as_tuple(std::forward<const Args>(args)...));
}

template <typename... Args>
[[nodiscard]] inline std::vector<std::uint8_t> binary_batch_pack(const Args&... args) {
    std::vector<std::uint8_t> out;
    binary_pack_tuple(out, std::forward_as_tuple(std::forward<const Args>(args)...));
    return out;
}

// return false when unpack failed
template <typename T>
inline bool binary_unpack(const void* data,
                          std::size_t len,
                          T& out,
                          bool ignore_invalid_properties,
                          std::size_t index) {
    auto* p_data = static_cast<const std::uint8_t*>(data);
    return internal::binary_unpack_helper(p_data, len, out, ignore_invalid_properties, index);
}

template <typename Tuple, typename V>
inline bool binary_unpack_tuple(const void* data,
                                std::size_t len,
                                Tuple& t,
                                bool ignore_invalid_properties,
                                std::size_t index) {
    auto* p_data = static_cast<const std::uint8_t*>(data);
    while (index != 0)
    {
        if (!internal::binary_unpack_skip_item(p_data, len)) return false;
        --index;
    }
    return internal::binary_unpack_tuple_helper(p_data, len, t, ignore_invalid_properties);
}

template <typename... Args>
inline bool binary_bacth_unpack(const void* data,
                                std::size_t len,
                                bool ignore_invalid_properties,
                                std::size_t index,
                                Args&&... args) {
    auto tuple = std::tie(args...);
    return binary_unpack_tuple(data, len, tuple, ignore_invalid_properties, index);
}
} // namespace Fundamental::io