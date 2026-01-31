#pragma once
#include <memory>
#include "cxx_config_include.hpp"
#ifdef AllocatorTracker
    #include <iostream>
#endif
namespace Fundamental
{
template <typename _Tp>
using AllocatorType = std_pmr::polymorphic_allocator<_Tp>;

template <typename MemorySourceType, typename... Args>
decltype(auto) MakeSharedMemorySource(Args&&... args) {
    return std::make_shared<MemorySourceType>(std::forward<Args>(args)...);
}

template <typename... Args>
decltype(auto) MakePoolMemorySource(Args&&... args) {
    return MakeSharedMemorySource<std_pmr::unsynchronized_pool_resource>(std::forward<Args>(args)...);
}

template <typename... Args>
decltype(auto) MakeThreadSafePoolMemorySource(Args&&... args) {
    return MakeSharedMemorySource<std_pmr::synchronized_pool_resource>(std::forward<Args>(args)...);
}

template <typename... Args>
decltype(auto) MakeMonoBufferMemorySource(Args&&... args) {
    return MakeSharedMemorySource<std_pmr::monotonic_buffer_resource>(std::forward<Args>(args)...);
}
namespace internal
{
template <
    size_t ObjectSize,
    bool ThreadSafe,
    typename PoolSourceType =
        std::conditional_t<ThreadSafe, std_pmr::synchronized_pool_resource, std_pmr::unsynchronized_pool_resource>>
inline std_pmr::memory_resource* GetObjectPoolSource() {
    static std_pmr::memory_resource* s_object_pool_source =
        new PoolSourceType(std_pmr::pool_options { 16, ObjectSize * 16 });
    return s_object_pool_source;
}
} // namespace internal

// fixed-size object allocator
template <typename ObjectType, bool ThreadSafe, size_t blockSize = sizeof(ObjectType)>
struct ObjectPoolAllocator : std_pmr::polymorphic_allocator<ObjectType> {
    ObjectPoolAllocator() :
    std_pmr::polymorphic_allocator<ObjectType>(internal::GetObjectPoolSource<blockSize, ThreadSafe>()) {
    }
    ~ObjectPoolAllocator() {
    }
#ifdef AllocatorTracker
#if TARGET_PLATFORM_WINDOWS
    _NODISCARD_RAW_PTR_ALLOC __declspec(allocator) ObjectType* allocate(_CRT_GUARDOVERFLOW size_t __n) {
        std::cout << "allocate align_size:" << alignof(ObjectType) << " total:" << __n * sizeof(ObjectType)
                  << " count:" << __n << std::endl;
        return std_pmr::polymorphic_allocator<ObjectType>::allocate(__n);
    }

    void deallocate(ObjectType* __p, size_t __n) noexcept  {
        std::cout << "deallocate align_size:" << alignof(ObjectType) << " total:" << __n * sizeof(ObjectType)
                  << " count:" << __n << std::endl;
        std_pmr::polymorphic_allocator<ObjectType>::deallocate(__p, __n);
    }
    template <typename... _CtorArgs>
    [[nodiscard]] ObjectType* NewObject(_CtorArgs&&... __ctor_args) {
        auto* __p = std_pmr::polymorphic_allocator<ObjectType>::allocate(1);
        try {
            std_pmr::polymorphic_allocator<ObjectType>::construct(__p, std::forward<_CtorArgs>(__ctor_args)...);
        }
        catch(...) {
            std_pmr::polymorphic_allocator<ObjectType>::deallocate(__p, 1);
            throw;
        }
        return __p;
    }
    #else
    [[nodiscard]]
    ObjectType* allocate(size_t __n) __attribute__((__returns_nonnull__)) {
        if ((__gnu_cxx::__int_traits<size_t>::__max / sizeof(ObjectType)) < __n) std::__throw_bad_array_new_length();
        std::cout << "allocate align_size:" << alignof(ObjectType) << " total:" << __n * sizeof(ObjectType)
                  << " count:" << __n << std::endl;
        return static_cast<ObjectType*>(std_pmr::polymorphic_allocator<ObjectType>::allocate(__n));
    }

    void deallocate(ObjectType* __p, size_t __n) noexcept __attribute__((__nonnull__)) {
        std::cout << "deallocate align_size:" << alignof(ObjectType) << " total:" << __n * sizeof(ObjectType)
                  << " count:" << __n << std::endl;
        std_pmr::polymorphic_allocator<ObjectType>::deallocate(__p, __n);
    }
    template <typename... _CtorArgs>
    [[nodiscard]] ObjectType* NewObject(_CtorArgs&&... __ctor_args) {
        auto* __p = std_pmr::polymorphic_allocator<ObjectType>::allocate(1);
        __try {
            std_pmr::polymorphic_allocator<ObjectType>::construct(__p, std::forward<_CtorArgs>(__ctor_args)...);
        }
        __catch(...) {
            std_pmr::polymorphic_allocator<ObjectType>::deallocate(__p, 1);
            __throw_exception_again;
        }
        return __p;
    }
    #endif
#endif


    void DeleteObject(ObjectType* __p) {
        std_pmr::polymorphic_allocator<ObjectType>::destroy(__p);
        std_pmr::polymorphic_allocator<ObjectType>::deallocate(__p, 1);
    }
};

template <typename ObjectType>
struct ThreadSafeObjectPoolAllocator : ObjectPoolAllocator<ObjectType, true> {
    ThreadSafeObjectPoolAllocator() = default;
    template <typename U>
    ThreadSafeObjectPoolAllocator(const ThreadSafeObjectPoolAllocator<U>&) noexcept {
    }

    template <typename U>
    struct rebind {
        using other = ThreadSafeObjectPoolAllocator<U>;
    };
};

template <typename ObjectType>
struct ThreadUnSafeObjectPoolAllocator : ObjectPoolAllocator<ObjectType, false> {
    ThreadUnSafeObjectPoolAllocator() = default;
    template <typename U>
    ThreadUnSafeObjectPoolAllocator(const ThreadUnSafeObjectPoolAllocator<U>&) noexcept {
    }

    template <typename U>
    struct rebind {
        using other = ThreadUnSafeObjectPoolAllocator<U>;
    };
};
} // namespace Fundamental