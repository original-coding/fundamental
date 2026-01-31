
if(__FUNDAMENTAL_CONFIG_TARGET__)
    return()
endif()
set(__FUNDAMENTAL_CONFIG_TARGET__ TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/import_gtest_config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/import_benchmark_config.cmake)
include(clang-tidy-helper)
option(F_BUILD_STATIC "build static fundamental lib" ON)
option(F_BUILD_SHARED "build dynamic fundamental lib" OFF)
option(ENABLE_DEBUG_MEMORY_TRACE "enable memory track" ON)
option(CLANG_BUILD_WITH_STD_CXX "clang build with libstdc++" ON)
option(F_ENABLE_COMPILE_OPTIMIZE "enable compile optimize" ON)

set(RTTR_LIB RTTR::Core_Lib CACHE STRING "use rttr static lib")
set(GLOB_NAMESPACE "fh::" CACHE STRING "generated lib namespace")
if(F_BUILD_SHARED)
    set(STATIC_LIB_SUFFIX _s CACHE STRING "static lib suffix")
endif()

#make these values invisible
mark_as_advanced(F_BUILD_STATIC)

if(TARGET BuildSettings)
    return()
endif()
add_library(BuildSettings INTERFACE)

message(STATUS "Target processor arch is: ${CMAKE_SYSTEM_PROCESSOR}")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" CMAKE_SYSTEM_PROCESSOR_LOWER)
if(CMAKE_SYSTEM_PROCESSOR_LOWER MATCHES "amd64|x86_64|x64")
    message(STATUS "Target is x86_64 (64-bit) architecture.")
    target_compile_definitions(BuildSettings INTERFACE TARGET_X64_PROCESSOR)
    set(TARGET_X64_PROCESSOR TRUE CACHE BOOL "Target is x86_64 (64-bit) architecture.")
    set(TARGET_X86_FAMILY TRUE CACHE BOOL "Target is x86 family (32 or 64-bit).")
    
elseif(CMAKE_SYSTEM_PROCESSOR_LOWER MATCHES "i[0-9]86|i386|i486|i586|i686|x86")
    message(STATUS "Target is x86 (32-bit) architecture.")
    target_compile_definitions(BuildSettings INTERFACE TARGET_X86_PROCESSOR)
    set(TARGET_X86_PROCESSOR TRUE CACHE BOOL "Target is x86 (32-bit) architecture.")
    set(TARGET_X86_FAMILY TRUE CACHE BOOL "Target is x86 family (32 or 64-bit).")
    
elseif(CMAKE_SYSTEM_PROCESSOR_LOWER MATCHES "(aarch64|arm64)")
    message(STATUS "Target is ARM 64 architecture.")
    target_compile_definitions(BuildSettings INTERFACE TARGET_ARM64_PROCESSOR)
    set(TARGET_ARM64_PROCESSOR TRUE CACHE BOOL "Target is ARM 64 architecture.")
    
elseif(CMAKE_SYSTEM_PROCESSOR_LOWER MATCHES "arm")
    message(STATUS "Target is ARM (32-bit) architecture.")
    target_compile_definitions(BuildSettings INTERFACE TARGET_ARM32_PROCESSOR)
    set(TARGET_ARM32_PROCESSOR TRUE CACHE BOOL "Target is ARM (32-bit) architecture.")
    
else()
    message(WARNING "Unknown target processor: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "build on linux")
    set(TARGET_PLATFORM_LINUX TRUE CACHE BOOL "Flag indicating LINUX platform")
    add_definitions(-DTARGET_PLATFORM_LINUX=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(STATUS "build on windows")
    set(TARGET_PLATFORM_WINDOWS TRUE CACHE BOOL "Flag indicating Windows platform")
    add_definitions(-DTARGET_PLATFORM_WINDOWS=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_definitions(-DTARGET_PLATFORM_MAC=1)
else()
    message(FATAL_ERROR "Unknown platform.")
endif()


if(TARGET_PLATFORM_WINDOWS)
    target_precompile_headers(BuildSettings INTERFACE "${CMAKE_CURRENT_LIST_DIR}/platform.h.in")
else()
    target_compile_options(BuildSettings INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-include "${CMAKE_CURRENT_LIST_DIR}/platform.h.in"
        >
    )
endif()


include(CheckCXXSourceCompiles)

## 
macro(CHECK_COMPILE_DEFINITIONS definition_name)
    check_cxx_source_compiles("
        #ifndef ${definition_name}
        #error ${definition_name} not defined
        #endif
        int main() { return 0; }
    "        ${definition_name}_DEFINED_)
endmacro(CHECK_COMPILE_DEFINITIONS)

macro(ADD_COMPILE_DEFINITION config_type definition_name)
    if(CMAKE_BUILD_TYPE STREQUAL "${config_type}")
        CHECK_COMPILE_DEFINITIONS(${definition_name})
        if(${definition_name}_DEFINED_)
            message(STATUS "${definition_name} is already defined in ${config_type} configuration.")
        else()
            message(STATUS "Adding ${definition_name}=1 for ${config_type} configuration")
            target_compile_definitions(BuildSettings INTERFACE
                $<$<CONFIG:${config_type}>:${definition_name}=1>
            )
        endif()
    endif()
endmacro(ADD_COMPILE_DEFINITION)

#add compile macro
ADD_COMPILE_DEFINITION(Debug DEBUG)
ADD_COMPILE_DEFINITION(Release NDEBUG)
target_compile_options(BuildSettings INTERFACE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
    -fPIC
    $<$<CONFIG:Debug>:-O0;-Wall;-g2;-ggdb;-fno-omit-frame-pointer>
    $<$<CONFIG:RelWithDebInfo>:-O2;-Wall;-g>
    $<$<CONFIG:Release>:-O3;-Wall>
    >
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>,$<CONFIG:Release>>:
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    >
    $<$<CXX_COMPILER_ID:MSVC>:/wd4101;/wd4996;/wd4100>
)

if(F_ENABLE_COMPILE_OPTIMIZE)
    # Release 编译器通用优化
    target_compile_options(BuildSettings INTERFACE
        $<$<CONFIG:Release>:
        # GCC/Clang
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -fno-strict-aliasing # 宽松别名规则(某些代码需要)
        -funroll-loops # 循环展开
        -ffast-math # 快速数学计算
        -fno-trapping-math # 禁用浮点陷阱
        >

        # MSVC
        $<$<CXX_COMPILER_ID:MSVC>:
        /Oy- # 禁用帧指针省略(调试更友好)
        /fp:fast # 快速浮点模型
        /Qpar # 自动并行化
        /GL # 全程序优化
        >
        >
    )
    target_compile_options(BuildSettings INTERFACE
        $<$<CONFIG:Release>:
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -finline-functions # 内联简单函数
        -finline-limit=200 # 提高内联阈值
        >
        $<$<CXX_COMPILER_ID:MSVC>:
        /Ob2 # 任意适合的内联
        >
        >
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|x86_64|AMD64")
        target_compile_options(BuildSettings INTERFACE
            $<$<CONFIG:Release>:
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -mavx2 -mbmi2 -mfma
            >
            $<$<CXX_COMPILER_ID:MSVC>:
            /arch:AVX2
            >
            >
        )
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
        target_compile_options(BuildSettings INTERFACE
            $<$<CONFIG:Release>:
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -mcpu=native -mtune=native
            >
            >
        )
    endif()
endif()
target_compile_features(BuildSettings INTERFACE
    "cxx_std_17"
)
target_compile_definitions(BuildSettings INTERFACE
    "$<$<CONFIG:Debug>:VERBOSE_LOGGING=1>"
    "$<$<CONFIG:Release>:OPTIMIZED=1>"
    $<$<CXX_COMPILER_ID:MSVC>:_WIN32_WINNT=0x0601
    $<$<CONFIG:Debug>:DEBUG=1>
    >
)

if(ENABLE_DEBUG_MEMORY_TRACE)
    target_compile_definitions(BuildSettings INTERFACE
        "$<$<CONFIG:Debug>:WITH_MEMORY_TRACK>"
    )
endif()

#add optimize config

target_compile_options(BuildSettings INTERFACE
    # LTO 编译选项（-flto 或 /GL）
    $<$<NOT:$<CONFIG:Debug>>:
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-flto=auto>
    $<$<CXX_COMPILER_ID:MSVC>:/GL>
    >
    # GCC/Clang 分节选项（为 --gc-sections 做准备）
    $<$<NOT:$<CONFIG:Debug>>:
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:
    -ffunction-sections
    -fdata-sections
    >
    >
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:
    -Wno-missing-field-initializers
    -Wno-unused-parameter
    -Wno-unused-but-set-parameter
    >
)


target_link_options(BuildSettings INTERFACE
    # LTO 链接选项（-flto 或 /LTCG）
    $<$<NOT:$<CONFIG:Debug>>:
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-flto=auto>
    $<$<CXX_COMPILER_ID:MSVC>:/LTCG>
    >
    # 无用代码剥离（--gc-sections 或 /OPT:REF）
    $<$<NOT:$<CONFIG:Debug>>:
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wl,--gc-sections>
    $<$<CXX_COMPILER_ID:MSVC>:/OPT:REF>
    >
    # 可选：MSVC 的代码段合并（类似 ICF）
    $<$<NOT:$<CONFIG:Debug>>:
    $<$<CXX_COMPILER_ID:MSVC>:/OPT:ICF>
    >

)
if(F_ENABLE_COMPILE_OPTIMIZE)
    if(TARGET_PLATFORM_LINUX)
        target_link_options(BuildSettings INTERFACE
            "$<$<CONFIG:RelWithDebInfo>:-Wl,-O2>"
            "$<$<CONFIG:Release>:-Wl,-O3>"
        )
    endif()
endif()



target_link_libraries(BuildSettings INTERFACE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:pthread
    >

)

if(NOT HAS_STD_FILESYSTEM)
    target_compile_definitions(BuildSettings INTERFACE
        USE_EXPERIMENTAL_FILESYSTEM
    )
    target_link_libraries(BuildSettings INTERFACE
        stdc++fs
    )
endif()

if(NOT HAS_STD_MEMORY_SOURCE)
    target_compile_definitions(BuildSettings INTERFACE
        USE_EXPERIMENTAL_MEMORY_SOURCE
    )
endif()

target_enable_clang_tidy(BuildSettings)

# enable POSITION_INDEPENDENT_CODE ON
function(config_enable_position_independent_code target_name)
    set_target_properties(${target_name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

# enable memory access check for debug mode
function(config_enable_sanitize_address_check target_name)
    if(TARGET_PLATFORM_WINDOWS)
        return()
    endif()
    if(DISABLE_DEBUG_SANITIZE_ADDRESS_CHECK)
        return()
    endif()
    target_compile_options(${target_name} PRIVATE
        "$<$<CONFIG:Debug>:-fstack-protector>"
    )
    target_compile_options(${target_name} PRIVATE
        "$<$<CONFIG:Debug>:-fsanitize=address>"
    )

    #you shoulde install libasan5 in ubuntu
    target_link_libraries(
        ${target_name} PRIVATE
        "$<$<CONFIG:Debug>:asan>"
    )
endfunction()

# enable memory profiling check for debug mode
# export MALLOC_CONF="prof:true,prof_active:true,lg_prof_sample:0,prof_leak:true,prof_accum:true"
function(config_enable_jemalloc_memory_profiling target_name)
    if(TARGET_PLATFORM_WINDOWS)
        return()
    endif()
    if(NOT ENABLE_JEMALLOC_MEMORY_PROFILING)
        return()
    endif()
    target_compile_definitions(${target_name} PRIVATE
        "$<$<CONFIG:Debug>:ENABLE_JEMALLOC_MEMORY_PROFILING=1>"
    )

    #you shoulde install libjemalloc-dev in ubuntu
    target_link_libraries(
        ${target_name} PRIVATE
        "$<$<CONFIG:Debug>:jemalloc>"
    )
endfunction()


# disable rtti for no debug mode
function(config_disable_rtti target_name)
    # TODO fix no rtti compile error
    # target_compile_options(${target_name} PRIVATE
    #     "$<$<NOT:$<CONFIG:Debug>>:$<$<CXX_COMPILER_ID:GNU,Clang>:-fno-rtti>>"
    #     "$<$<NOT:$<CONFIG:Debug>>:$<$<CXX_COMPILER_ID:MSVC>:/GR->>"
    # )
endfunction()

# enable profiling
# run you program to generate gmon.out
# then  'gprof program gmon.out > analysis.txt'
function(config_enable_pg_profiling target_name)
    if(TARGET_PLATFORM_WINDOWS)
        return()
    endif()
    target_compile_options(${target_name} PRIVATE "$<$<CONFIG:Debug>:-pg>"
    )
    target_link_options(${target_name} PRIVATE "$<$<CONFIG:Debug>:-pg>"
    )
endfunction()

#strip all debug info for no debug mode
function(config_strip_debug_info target_name)
    if(TARGET_PLATFORM_WINDOWS)
        return()
    endif()
    # 检查目标是否为可执行程序
    get_property(target_type TARGET ${target_name} PROPERTY TYPE)
    if(NOT (target_type STREQUAL "EXECUTABLE" OR target_type STREQUAL "SHARED_LIBRARY" OR target_type STREQUAL "MODULE_LIBRARY"))
        return()
    endif()
    # target_link_options(${target_name} PRIVATE
    #     $<$<NOT:$<CONFIG:Debug>>:
    #     $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wl,--strip-debug>
    #     $<$<CXX_COMPILER_ID:MSVC>:/link /RELEASE>
    #     >
    # )

    if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:${target_name}> $<TARGET_FILE:${target_name}>.sym
            COMMAND ${CMAKE_OBJCOPY} --strip-debug $<TARGET_FILE:${target_name}>
            COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink=${target_name}.sym $<TARGET_FILE:${target_name}>
            WORKING_DIRECTORY $<TARGET_FILE_DIR:${target_name}>
            COMMENT "Extracting ${target_name} debug symbols..."
        )
    endif()
endfunction()

#static linker 启用完全静态链接
function(config_static_link target_name)
    target_link_options(${target_name} PRIVATE -static)
endfunction()




function(add_plugin plugin_name)
    if(PLUGIN_USE_STATIC)
        add_library(${plugin_name} STATIC ${ARGN})
    else()
        add_library(${plugin_name} SHARED ${ARGN})
        target_compile_definitions(${plugin_name} PRIVATE __PLUGIN_DLL_EXPORTS_)
        target_compile_definitions(${plugin_name} PUBLIC PLUGIN_BUILD_SHARED)
        if(CMAKE_COMPILER_IS_GNUCXX)
            # we have to use this flag, otherwise dlclose does not call the destructor function
            # of the library
            target_compile_options(${plugin_name} PRIVATE "-fno-gnu-unique")
        endif()
    endif()
    # Add plugin name
    target_compile_definitions(${plugin_name}
        PRIVATE
        PLUGIN_NAME= "${plugin_name}"
    )
endfunction()

#add specific compile options 
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "use gnu compiler")
    if(MINGW)
        set(GNU_STATIC_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static")
    else()
        set(GNU_STATIC_LINKER_FLAGS "-static-libgcc -static-libstdc++")
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "use clang compiler")
    add_compile_options(-Wno-implicit-const-int-float-conversion)
    if(CLANG_BUILD_WITH_STD_CXX)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libstdc++")
        set(CLANG_STATIC_LINKER_FLAGS "-stdlib=libstdc++ -static-libstdc++")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
        set(CLANG_STATIC_LINKER_FLAGS "-stdlib=libc++ -static-libc++")
    endif()
endif()

function(enable_origin_rpath app_target_name)
    set_target_properties(${app_target_name} PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endfunction()

file(GLOB CMAKE_FILES
    "cmake/*.cmake"
    "cmake/*.txt"
    "cmake/*.in"
)

source_group("local cmake files" FILES ${CMAKE_FILES})

add_custom_target(fundamental_cmake_files SOURCES ${CMAKE_FILES})

set_target_properties(${fundamental_cmake_files} PROPERTIES
    FOLDER "cmake"
)


#[[
# filter_source_files - Filter source file list to exclude test and benchmark files
#
# Description:
#   This function filters a list of source files by excluding files that match
#   specific patterns. It is primarily used to separate main program code from
#   test and benchmark code. By default, it excludes any files containing
#   "_test." or "_benchmark." in their names, regardless of file extension.
#
# Parameters:
#   SOURCE_LIST_VAR - Name of the variable containing the source file list
#   ARGN            - Optional list of custom regex patterns for exclusion
#
# Default Behavior:
#   Excludes files matching:
#   - "[^.]*_test\\.[^.]*$"      (any files with "_test." before extension)
#   - "[^.]*_benchmark\\.[^.]*$" (any files with "_benchmark." before extension)
#
# Usage Examples:
#   # Use default patterns (exclude _test.* and _benchmark.* files)
#   filter_source_files(MY_SOURCES)
#
#   # Use custom patterns (exclude only _test.* files)
#   filter_source_files(MY_SOURCES "[^.]*_test\\.[^.]*$")
#
#   # Use multiple custom patterns
#   filter_source_files(MY_SOURCES 
#       "[^.]*_test\\.[^.]*$"
#       "[^.]*_example\\.[^.]*$"
#   )
#]]
function(filter_test_source_files SOURCE_LIST_VAR)
    set(EXCLUDE_PATTERNS
        "[^.]*_test\\.[^.]*$"
        "[^.]*_benchmark\\.[^.]*$"
    )
    if(ARGN)
        set(EXCLUDE_PATTERNS ${ARGN})
    endif()

    set(filtered_list "")
    foreach(source ${${SOURCE_LIST_VAR}})
        set(SHOULD_EXCLUDE OFF)
        foreach(pattern ${EXCLUDE_PATTERNS})
            if(source MATCHES "${pattern}")
                set(SHOULD_EXCLUDE ON)
                break()
            endif()
        endforeach()
        if(NOT SHOULD_EXCLUDE)
            list(APPEND filtered_list ${source})
        endif()
    endforeach()

    set(${SOURCE_LIST_VAR} ${filtered_list} PARENT_SCOPE)
endfunction()


#[[
.add_custom_app
Create a custom application executable with organized source groups and common configuration.

Description:
  This function creates an executable target with organized source files, assets,
  and dependencies. It sets up common properties including source grouping,
  include directories, debugger working directory (MSVC), and build settings.

Parameters:
  target_name (required): Name of the target to create
  SOURCES     (required): List of source files for the application
  ASSETS      (required): List of asset files for the application  
  DEPS        (required): List of dependency files for the application

Properties:
  - Organizes sources and assets in source groups maintaining directory structure
  - Sets VS_DEBUGGER_WORKING_DIRECTORY for MSVC builds
  - Adds current source directory to include paths
  - Sets FOLDER property for IDE organization
  - Links against BuildSettings library
  - Defines APP_NAME compile definition
  - Applies debug info stripping configuration

Usage Examples:
  # Basic usage with sources, assets, and dependencies
  add_custom_app(my_app 
    "main.cpp;utils.cpp" 
    "assets/config.json;assets/textures.png" 
    "generated/resources.h"
  )
]]#
function(add_custom_app target_name SOURCES ASSETS DEPS)
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ASSETS})

    # Create the executable
    add_executable(${target_name} "${SOURCES}" "${ASSETS}" "${DEPS}")

    if(MSVC)
        set_property(TARGET ${target_name} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR})
    endif()

    # Config includes
    target_include_directories(${target_name}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_target_properties(${target_name} PROPERTIES
        FOLDER ${CMAKE_CURRENT_SOURCE_DIR}
    )
    # Config links
    target_link_libraries(${target_name}
        PRIVATE
        BuildSettings
    )

    # Add target name
    target_compile_definitions(${target_name}
        PRIVATE
        APP_NAME="${target_name}"
    )

    config_strip_debug_info(${target_name})
endfunction()

#[[
add_unit_test_executable - Create a unit test executable using Google Test framework

Description:
  This function creates a unit test executable that uses Google Test framework.
  It only creates the target when BUILD_TESTING is ON and IMPORT_GTEST is available.

Parameters:
  test_target_name (required): Name of the test target to create
  SOURCES          (required): List of source files for the test
  ASSETS           (required): List of asset files for the test
  DEPS             (required): List of dependency files for the test
  ARGN             (optional): Additional command line arguments for the test

Usage Examples:
  add_unit_test_executable(math_tests "test_math.cpp" "test_data.json" "" "--gtest_filter=MathTest.*")
]]#
function(add_unit_test_executable test_target_name SOURCES ASSETS DEPS)
    if(NOT IMPORT_GTEST)
        return()
    endif()
    add_custom_app(${test_target_name} "${SOURCES}" "${ASSETS}" "${DEPS}")
    declare_gtest_exe(${test_target_name})
    add_test(NAME ${test_target_name} COMMAND ${test_target_name} ${ARGN})
endfunction()

#[[
add_benchmark_test_executable - Create a benchmark test executable using benchmark library

Description:
  This function creates a benchmark test executable that uses the benchmark library.
  It only creates the target when BUILD_TESTING is ON and IMPORT_BENCHMARK is available.

Parameters:
  benchmark_target_name (required): Name of the benchmark target to create
  SOURCES               (required): List of source files for the benchmark
  ASSETS                (required): List of asset files for the benchmark
  DEPS                  (required): List of dependency files for the benchmark
  ARGN                  (optional): Additional command line arguments for the benchmark

Usage Examples:
  add_benchmark_test_executable(performance_benchmark "benchmark_algorithm.cpp" "benchmark_data.json" "" "--benchmark_min_time=0.5")
]]#
function(add_benchmark_test_executable benchmark_target_name SOURCES ASSETS DEPS)
    if(NOT IMPORT_BENCHMARK)
        return()
    endif()
    add_custom_app(${benchmark_target_name} "${SOURCES}" "${ASSETS}" "${DEPS}")
    declare_benchmark_exe(${benchmark_target_name})
    add_test(NAME ${benchmark_target_name} COMMAND ${benchmark_target_name} ${ARGN})
endfunction()
