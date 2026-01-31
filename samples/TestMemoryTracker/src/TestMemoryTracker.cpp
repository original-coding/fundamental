
#include "fundamental/basic/log.h"
#include "fundamental/tracker/memory_tracker.hpp"
#include <chrono>
#include <iostream>

// declare class
struct Test1 :Fundamental::MemoryTracker<Test1>
{
    char m[1024];
};


struct Test2
{

};

struct Test3:Test1
{

};
template<typename T>
void TestReport()
{
    std::string outstr;
    Fundamental::ReportMemoryTracker<T>(outstr);
    //we should not use typeid in production dev code
#if DEBUG
    FINFO("class {}:{}",typeid(T).name(),outstr.c_str());
#endif
}
void TestAllocateMemory()
{
    FINFO("test start");
    std::vector<Test1 *> cache;
    auto freeFunc=[](std::vector<Test1 *> &vec){
        for(auto &i:vec)
        {
            delete i;
        }
        vec.clear();
    };
    //new delete equal
    for(int i=0;i<100;++i)
    {
        delete(new Test1);
    }
    {
        TestReport<Test1>();
        cache.push_back(new Test1);
        cache.push_back(new Test3);
        cache.push_back(new Test1[100]);
        cache.push_back(new Test1[100]);
        cache.push_back(new Test1[100]);
        cache.push_back(new Test1[100]);
        cache.push_back(new Test1[100]);
        FDEBUG("after alloc");
        //514048
        TestReport<Test1>();
        TestReport<Test3>();
        freeFunc(cache);
        FDEBUG("after delete");
        TestReport<Test1>();
        
    }
    // test compile error
    //TestReport<Test2>();
    FINFO("test finished");
}


int main(int argc, char** argv)
{
    TestAllocateMemory();
    return 0;
}
