#pragma once
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>



namespace Fundamental {
// a base class for type check
struct MemoryTrackerBase {};
#ifndef WITH_MEMORY_TRACK
template <typename T>
struct MemoryTracker {};
#else
template <typename T>
struct MemoryTracker : MemoryTrackerBase {
    static void* operator new(std::size_t bytes) {
        auto ptr = ::operator new(bytes);
        if (ptr) {
            std::scoped_lock<std::mutex> locker(_mutex);
            _bytes += bytes;
            _records.emplace(ptr, bytes);
        }
        return ptr;
    }
    static void operator delete(void* ptr) {
        ::operator delete(ptr);
        if (ptr) {
            std::scoped_lock<std::mutex> locker(_mutex);
            auto iter = _records.find(ptr);
            if (iter != _records.end()) {
                _bytes -= iter->second;
                _records.erase(iter);
            }
        }
    }
    static void* operator new[](std::size_t bytes) {
        auto ptr = ::operator new[](bytes);
        if (ptr) {
            std::scoped_lock<std::mutex> locker(_mutex);
            _bytes += bytes;
            _records.emplace(ptr, bytes);
        }
        return ptr;
    }
    static void operator delete[](void* ptr) {
        ::operator delete[](ptr);
        if (ptr) {
            std::scoped_lock<std::mutex> locker(_mutex);
            auto iter = _records.find(ptr);
            if (iter != _records.end()) {
                _bytes -= iter->second;
                _records.erase(iter);
            }
        }
    }
    inline static std::mutex _mutex;
    inline static std::size_t _bytes;
    inline static std::unordered_map<void*, std::size_t> _records;
};
#endif

template <typename T, typename = void>
struct track_has_bytes : std::false_type {};

template <typename T>
struct track_has_bytes<T, std::void_t<decltype(T::_bytes)>> : std::true_type {};

template <typename T, typename = void>
struct track_has_records : std::false_type {};

template <typename T>
struct track_has_records<T, std::void_t<decltype(T::_records)>> : std::true_type {};

template <typename T, typename = void>
struct track_has_mutex : std::false_type {};

template <typename T>
struct track_has_mutex<T, std::void_t<decltype(T::_mutex)>> : std::true_type {};

template <typename T>
inline void ReportMemoryTracker(std::string& outStr) {
#ifdef WITH_MEMORY_TRACK
    static_assert(!(!std::is_base_of<MemoryTrackerBase, T>::value || !track_has_bytes<T>::value ||
                    !track_has_records<T>::value || !track_has_mutex<T>::value),
                  "ReportMemoryTracker should be inheritance from MemoryTrackerBase T");
    std::scoped_lock<std::mutex> locker(T::_mutex);
    outStr += "reserve size:" + std::to_string(T::_bytes);
    outStr += " reserve blocks:" + std::to_string(T::_records.size());
#endif
}

void EnableMemoryProfiling();
void DumpMemorySnapShot(const std::string &out_path);
} // namespace Fundamental