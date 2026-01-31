#pragma once
#include <string>
#include <string_view>
#include <tuple>

using string_view = std::string_view;

#include "rpc/basic/codec.h"

namespace network {
namespace rpc_service {
inline bool has_error(string_view result) {
    if (result.empty()) {
        return true;
    }

    rpc_service::msgpack_codec codec;
    auto tp = codec.unpack_tuple<std::tuple<std::int32_t>>(result.data(), result.size());

    return std::get<0>(tp) != 0;
}

template <typename T>
inline T get_result(string_view result) {
    rpc_service::msgpack_codec codec;
    auto tp = codec.unpack_tuple<std::tuple<T>>(result.data(), result.size(), 1);
    return std::get<0>(tp);
}

inline std::string get_error_msg(string_view result) {
    rpc_service::msgpack_codec codec;
    auto tp = codec.unpack_tuple<std::tuple<std::string>>(result.data(), result.size(), 1);
    return std::get<0>(tp);
}

template <typename T>
inline T as(string_view result) {
    if (has_error(result)) {
        throw std::logic_error(get_error_msg(result));
    }

    return get_result<T>(result);
}
} // namespace rpc_service
} // namespace network