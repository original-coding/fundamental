#define AllocatorTracker 1

#include "fundamental/basic/allocator.hpp"
#include "fundamental/basic/log.h"

#include <array>
#include <deque>
#include <iostream>
#include "fundamental/basic/cxx_config_include.hpp"
#include <vector>

class tracer_memory_resource : public std_pmr::memory_resource {
public:
    explicit tracer_memory_resource(std_pmr::memory_resource* refsource) : ref(refsource) {
    }

protected:
    void* do_allocate(size_t __bytes, size_t __alignment) override {
        std::cout << "allocate " << __bytes << std::endl;
        return ref->allocate(__bytes, __alignment);
    }

    void do_deallocate(void* __p, size_t __bytes, size_t __alignment) override {
        std::cout << "deallocate " << __bytes << std::endl;
        ref->deallocate(__p, __bytes, __alignment);
    }

    bool do_is_equal(const memory_resource& __other) const noexcept override {
        return this == &__other;
    }

private:
    std_pmr::memory_resource* const ref;
};

void TestObjectPool();

int main() {
    {
        std_pmr::memory_resource* upstream = std_pmr::new_delete_resource();
        tracer_memory_resource source(upstream);
        std_pmr::pool_options options;
        constexpr std::size_t kLargeBlockSize = 4098;
        options.largest_required_pool_block   = kLargeBlockSize;
        options.max_blocks_per_chunk          = 4;

        auto poolResource = Fundamental::MakePoolMemorySource(options, &source);

        std_pmr::memory_resource* activeSource = poolResource.get();
        {
            std::cout << "test int vec " << std::endl;
            std_pmr::vector<int> myVector { activeSource };

            for (int i = 0; i < 32; ++i) {
                std::cout << "emplace" << std::endl;
                myVector.push_back(i);
            }
            std::cout << "clear" << std::endl;
            myVector.clear();
            std::cout << "shrink_to_fit" << std::endl;
            myVector.shrink_to_fit();
        }

        struct LargeBlock {
            char data[kLargeBlockSize * 2 + 1];
        };
        {
            std::cout << "test large block vec " << sizeof(LargeBlock) << std::endl;
            std_pmr::vector<LargeBlock> testVec { activeSource };
            for (int i = 0; i < 10; ++i) {
                std::cout << "emplace" << std::endl;
                testVec.emplace_back();
            }

            std::cout << "clear" << std::endl;
            testVec.clear();
            std::cout << "shrink_to_fit" << std::endl;
            testVec.shrink_to_fit();
        }
        { std::deque<int, std_pmr::polymorphic_allocator<int>> dq; }
        std::cout << "finished" << std::endl;
    }

    TestObjectPool();
    return 0;
}
struct TestMem {
    inline static uint8_t s_loop_value;
    uint8_t bytes[32];
    std::uint64_t k;
    TestMem() {
        bytes[0] = s_loop_value++;
        memset(bytes + 1, '1', 31);
        FINFO("TestMem constructor {}", static_cast<int>(bytes[0]));
    };
    ~TestMem() {
        FINFO("TestMem destructor {}", static_cast<int>(bytes[0]));
    }
};
void TestObjectPool() {
    using SafePool = Fundamental::ThreadSafeObjectPoolAllocator<TestMem>;

    SafePool pool;
    for (int i = 0; i < 10; ++i) {
        try {
            FINFO("allocate {}", i);
            auto* p = pool.allocate(1);
            FINFO("construct {}", i);
            pool.construct(p);
            FINFO("destroy {}", i);
            pool.destroy(p);
            FINFO("deallocate {}", i);
            pool.deallocate(p, 1);
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
        }
    }
    {
        std::vector<TestMem, SafePool> testVec;
        FINFO("resize 10");
        testVec.resize(10);
        FINFO("clear");
        testVec.clear();
        FINFO("resize 0");
        testVec.resize(0);
        FINFO("shrink_to_fit");
        testVec.shrink_to_fit();
    }
    {
        FINFO("new object");
        auto testobject = pool.NewObject();
        FINFO("delete object");
        pool.DeleteObject(testobject);
    }
}