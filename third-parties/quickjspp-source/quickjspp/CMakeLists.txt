cmake_minimum_required(VERSION 3.13)
project(quickjspp LANGUAGES CXX)

#set(CMAKE_CXX_STANDARD 17)
# quickjs 的 LTO 只在 Release + 非 clang 下开启：clang 需要 lld/LLVMgold 插件
# 才能链接 LLVM bitcode，环境不保证可用；调试/RelWithDebInfo 构建也不需要。
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

add_subdirectory(quickjs)

add_library(quickjspp INTERFACE)
target_link_libraries(quickjspp INTERFACE quickjs ${CMAKE_REQUIRED_LIBRARIES})
target_compile_features(quickjspp INTERFACE cxx_std_17)
target_include_directories(quickjspp INTERFACE .)
set_target_properties(quickjspp PROPERTIES PUBLIC_HEADER quickjspp.hpp)

add_executable(qjs qjs.cpp)
target_link_libraries(qjs quickjspp)

# cmake -DBUILD_TESTING=OFF to disable tests
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(test)
endif()

install(TARGETS quickjspp qjs)
