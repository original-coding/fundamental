#pragma once
#include "blake3.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Fundamental
{
// 32 bytes output
class blake3_hasher {
public:
    constexpr static std::size_t kMaxBlake3HashLength = BLAKE3_OUT_LEN;
public:
    void update(const void* ptr, std::size_t len) {
        blake3_hasher_update(&hasher_, ptr, len);
    }
    void finalize(void* ptr, std::size_t len) const {
        std::uint8_t temp[kMaxBlake3HashLength] = { 0 };
        blake3_hasher_finalize(&hasher_, temp, kMaxBlake3HashLength);
        if (ptr) {
            std::memcpy(ptr, temp, len > kMaxBlake3HashLength ? kMaxBlake3HashLength : len);
        }
    }
    void update_and_finalize(const void* data_ptr, std::size_t data_len, void* out_ptr, std::size_t out_len) {
        update(data_ptr, data_len);
        finalize(out_ptr, out_len);
    }

    void reset() {
        blake3_hasher_reset(&hasher_);
    }

    template <typename T, typename = std::enable_if_t<sizeof(T) <= kMaxBlake3HashLength>>
    T finalize() const {
        T ret {};
        blake3_hasher_finalize(&hasher_, reinterpret_cast<uint8_t*>(&ret), sizeof(ret));
        return ret;
    }
    template <typename T, typename = std::enable_if_t<sizeof(T) <= kMaxBlake3HashLength>>
    T update_and_finalize(const void* data_ptr, std::size_t data_len) {
        update(data_ptr, data_len);
        return finalize<T>();
    }

    static void hash(const void* data_ptr, std::size_t data_len, void* out_ptr, std::size_t out_len) {
        blake3_hasher hasher;
        hasher.update_and_finalize(data_ptr, data_len, out_ptr, out_len);
    }

    template <typename T, typename = std::enable_if_t<sizeof(T) <= kMaxBlake3HashLength>>
    static T hash(const void* data_ptr, std::size_t data_len) {
        blake3_hasher hasher;
        return hasher.update_and_finalize<T>(data_ptr, data_len);
    }

public:
    // constructors and assignment operators
    blake3_hasher() {
        blake3_hasher_init(&hasher_);
    }
    blake3_hasher(const blake3_hasher& ohter) {
        std::memcpy(&hasher_, &ohter.hasher_, sizeof(hasher_));
    }
    blake3_hasher& operator=(const blake3_hasher& ohter) {
        std::memcpy(&hasher_, &ohter.hasher_, sizeof(hasher_));
        return *this;
    }

    blake3_hasher(blake3_hasher&& ohter) noexcept {
        std::memcpy(&hasher_, &ohter.hasher_, sizeof(hasher_));
    }
    blake3_hasher& operator=(blake3_hasher&& ohter) noexcept {
        std::memcpy(&hasher_, &ohter.hasher_, sizeof(hasher_));
        return *this;
    }

private:
    ::blake3_hasher hasher_;
};
} // namespace Fundamental