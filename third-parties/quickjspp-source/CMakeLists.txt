cmake_minimum_required(VERSION 3.13)

# quickjs 的 LTO 只在 Release + 非 clang 下开启：clang 需要 lld/LLVMgold 插件
# 才能链接 LLVM bitcode，环境不保证可用；调试/RelWithDebInfo 构建也不需要。
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

add_subdirectory(quickjspp/quickjs)

add_library(quickjspp INTERFACE)
target_link_libraries(quickjspp INTERFACE quickjs ${CMAKE_REQUIRED_LIBRARIES})
target_compile_features(quickjspp INTERFACE cxx_std_17)
target_include_directories(quickjspp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/quickjspp>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
