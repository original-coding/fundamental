#pragma once
#include "wyhash_utils.hpp"

#include <type_traits>

namespace Fundamental
{
inline std::size_t Hash(const void* data, std::size_t size, std::size_t seed = 0) {
    if constexpr (sizeof(std::size_t) == 8) {
        return wyhash::wyhash(data, size, seed, wyhash::_wyp);
    } else {
        return wyhash::wyhash32(data, size, static_cast<std::uint32_t>(seed));
    }
}

inline std::uint64_t Hash64(const void* data, std::size_t size, std::uint64_t seed = 0) {
    return wyhash::wyhash(data, size, seed, wyhash::_wyp);
}

template <typename T,
          typename = std::void_t<decltype(std::declval<std::decay_t<T>>().data()),
                                 decltype(std::declval<std::decay_t<T>>().size()),
                                 typename std::decay_t<T>::value_type>>
inline std::size_t Hash(const T& input, std::size_t seed = 0) {
    return Hash(input.data(), input.size() * sizeof(typename std::decay_t<T>::value_type), seed);
}

template <typename T,
          typename = std::void_t<decltype(std::declval<std::decay_t<T>>().data()),
                                 decltype(std::declval<std::decay_t<T>>().size()),
                                 typename std::decay_t<T>::value_type>>
inline std::uint64_t Hash64(const T& input, std::uint64_t seed = 0) {
    return Hash64(input.data(), input.size() * sizeof(typename std::decay_t<T>::value_type), seed);
}
} // namespace Fundamental