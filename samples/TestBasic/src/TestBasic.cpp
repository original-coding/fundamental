
#include "fundamental/basic/error_code.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
#include "fundamental/tracker/memory_tracker.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
// set USE_RAW_PTR to  0 will cause memory segmentation
#define USE_RAW_PTR 1
namespace
{
struct X {
    X() {
        str  = std::make_shared<std::string>();
        *str = std::string(1024, 'c');
        std::cout << "x construct" << std::endl;
    }
    ~X() {
        str.reset();
        std::cout << "x destruct" << std::endl;
    }
    std::shared_ptr<std::string> str;
};

struct TestInstance {
    ~TestInstance() {
        std::cout << "TestInstance destruct" << std::endl;
        delete x;
        x = nullptr;
    }
    std::string GetX() {
        return *(x->str);
    }
    void SetX() {
        *(x->str) = std::string(1024, 'b');
    }
    X* x = new X;
};
#if USE_RAW_PTR
static auto t1 = new TestInstance();
#else
static std::shared_ptr<TestInstance> t1 = std::make_shared<TestInstance>();
#endif
struct TestS : public Fundamental::Singleton<TestS> {
    // we should not declare any other signature for contruct Singleton instance
    // except the default signature version if you want to init some member values
    [[maybe_unused]] ~TestS();
    int i = 0;
};
struct TestNormal {
    ~TestNormal();
};
static TestNormal s_normal;
#if USE_RAW_PTR
static auto t2 = new TestInstance();
#else
static std::shared_ptr<TestInstance> t2 = std::make_shared<TestInstance>();
#endif
TestS::~TestS() {
    // we should not access another static instance
    //  in the destruct function to avoid access invalid memory segmentation
    std::cout << "TestS destruct" << std::endl;
    std::cout << "access t1 " << t1->GetX() << std::endl;
    std::cout << "access t2" << t2->GetX() << std::endl;
}
TestNormal::~TestNormal() {
    FINFOS << "TestNormal destruct";
    std::cout << "access t1 " << t1->GetX() << std::endl;
    std::cout << "access t2 " << t2->GetX() << std::endl;
}
} // namespace
void test_errorcode() {
    auto ec = Fundamental::make_error_code(1, std::system_category(), "test_msg");
    {
        auto exception = ec.make_excepiton();
        FINFO("exception:{} {} {} {}", exception.code().category().name(), exception.code().value(),
              exception.code().message(), exception.what());
    }
    {
        try {
            auto exception_ptr = ec.make_exception_ptr();
            std::rethrow_exception(exception_ptr);
        } catch (const std::system_error& e) {
            FINFO("rethrow_exception:{} {} {} {}", e.code().category().name(), e.code().value(), e.code().message(),
                  e.what());
        }
    }
    auto ec_copy = ec;
    FINFO("copy ec:{} {} {} {} {}", ec_copy.value(), ec_copy.message(), ec_copy.details(), ec_copy.details_c_str(),
          ec_copy.details_view());
    FINFO("StringFormat:{}", Fundamental::StringFormat(ec_copy));
    FINFO("to_string:{}", Fundamental::to_string(ec_copy));
    FINFO("[out directly]:{}", ec_copy);
    Fundamental::error_code default_ec;
    FINFO("default ec:{} {} {} {} {}", default_ec.value(), default_ec.message(), default_ec.details(),
          default_ec.details_c_str(), default_ec.details_view());
    FINFO("{}",Fundamental::kTargetPlatform==Fundamental::TargetPlatformType::Linux);
}
int main(int argc, char** argv) {
    Fundamental::EnableMemoryProfiling();
    test_errorcode();
    auto& a = t1;
    a->SetX();
    auto& c = t2;
    c->SetX();
    auto& b = TestS::Instance();
    F_UNUSED(b);
    auto& n = s_normal;
    F_UNUSED(n);
    Fundamental::DumpMemorySnapShot("1.out");
    [[maybe_unused]] auto ptr = new int[100];
    std::promise<void> p;
    std::thread t([&p]() {
        [[maybe_unused]] auto ptr = new int[1000];
        p.set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    });
    p.get_future().get();
    Fundamental::DumpMemorySnapShot("2.out");
    t.join();
    return 0;
}
