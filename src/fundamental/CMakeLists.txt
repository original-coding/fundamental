# Set lib_name automatically to its folder name
get_filename_component(lib_name ${CMAKE_CURRENT_LIST_DIR} NAME)
set(current_test_lib_name ${lib_name})

add_subdirectory(algorithm/blake3)
file(GLOB lib_source
    *.h
    *.cpp
    *.hpp
    basic/*.h
    basic/*.cpp
    basic/*.hpp
    delay_queue/*.h
    delay_queue/*.cpp
    delay_queue/*.hpp
    thread_pool/*.h
    thread_pool/*.cpp
    thread_pool/*.hpp
    tracker/*.h
    tracker/*.cpp
    tracker/*.hpp
    algorithm/*.h
    algorithm/*.cpp
    algorithm/*.hpp
    algorithm/*.cc
    file_io/csv/*.h
    file_io/csv/*.cpp
    file_io/csv/*.hpp
    file_io/csv/*.cc
)

if(TARGET_PLATFORM_LINUX)
    file(GLOB process_source
        process/*.h
        process/*.cc
        process/*.cpp
    )
    list(APPEND lib_source ${process_source})
endif()
set(extra_libs)
set(extra_compile_options)
#prepare rttr
if(FUNDAMENTAL_BUILD_RTTR)
    file(GLOB rttr_source
        rttr_handler/*.h
        rttr_handler/*.cpp
        rttr_handler/*.hpp
    )
    list(APPEND lib_source ${rttr_source})
    list(APPEND extra_libs ${RTTR_LIB})
    if(FUNDAMENTAL_ENABLE_SCRIPT_SUPPORT)
        if(TARGET_PLATFORM_LINUX)
            list(APPEND extra_libs quickjspp)
        endif()
    endif()
    if(TARGET_PLATFORM_WINDOWS)
        list(APPEND extra_compile_options /bigobj)
    endif()
endif()

if(FUNDAMENTAL_BUILD_EVENTS)
    #prepare events
    file(GLOB events_source
        events/*.h
        events/*.cpp
        events/*.hpp
        application/*.h
        application/*.cpp
        application/*.hpp
        data_storage/*.h
        data_storage/*.cpp
        data_storage/*.hpp
    )
    list(APPEND lib_source ${events_source})
    list(APPEND extra_libs eventpp)
endif()

if(TARGET_PLATFORM_WINDOWS)
    message(STATUS "use wingetopt to replace getopt")
    list(APPEND extra_libs wingetopt)
endif()


# Add to source group, TREE will enable cmake to config the folder layout automatically
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${lib_source})

filter_test_source_files(lib_source)

add_library(${lib_name}_property INTERFACE)


target_include_directories(${lib_name}_property INTERFACE
    ${src_root_dir}
)

find_package(ZLIB REQUIRED)

# Config link dependencies
target_link_libraries(${lib_name}_property INTERFACE
    ${extra_libs}
    spdlog
    nlohmann::json
    BuildSettings
    ZLIB::ZLIB
    BLAKE3::blake3
)

if(FUNDAMENTAL_LOG_PRINT_FILE_NAME)
    target_compile_definitions(${lib_name}_property
        INTERFACE
        __LOG_PRINT_FILE_NAME__=1
    )
endif()

target_compile_options(${lib_name}_property INTERFACE
    ${extra_compile_options}
)

if(F_BUILD_STATIC)
    # Create the static library
    add_library(${lib_name}${STATIC_LIB_SUFFIX} STATIC ${lib_source})
    set_target_properties(${lib_name}${STATIC_LIB_SUFFIX} PROPERTIES
        FOLDER ${src_root_dir_name}
    )

    config_enable_jemalloc_memory_profiling(${lib_name}${STATIC_LIB_SUFFIX})
    config_disable_rtti(${lib_name}${STATIC_LIB_SUFFIX})
    # Config link dependencies
    target_link_libraries(${lib_name}${STATIC_LIB_SUFFIX}
        PUBLIC
        ${lib_name}_property
    )
    add_library(${GLOB_NAMESPACE}${lib_name}${STATIC_LIB_SUFFIX} ALIAS ${lib_name}${STATIC_LIB_SUFFIX})
endif()

if(F_BUILD_SHARED)
    # Create the shared library
    add_library(${lib_name} SHARED ${lib_source})
    set_target_properties(${lib_name}$ PROPERTIES
        FOLDER ${src_root_dir_name}
    )
    config_enable_jemalloc_memory_profiling(${lib_name})
    config_disable_rtti(${lib_name})
    config_enable_sanitize_address_check(${lib_name})
    # Config link dependencies
    target_link_libraries(${lib_name}
        PUBLIC
        ${lib_name}_property
    )

    add_library(${GLOB_NAMESPACE}${lib_name} ALIAS ${lib_name})
endif()


if(F_BUILD_STATIC)
    if(BUILD_TESTING)
        # add test
        add_unit_test_executable(base64_utils_test basic/base64_utils_test.cpp "" "")
        if(TARGET base64_utils_test)
            target_link_libraries(base64_utils_test
                PRIVATE
                ${current_test_lib_name}
            )
        endif()
        # add benchmark
        add_benchmark_test_executable(hash_benchmark algorithm/hash_benchmark.cpp "" "")
        if(TARGET hash_benchmark)
            target_link_libraries(hash_benchmark
                PRIVATE
                ${current_test_lib_name}
            )
        endif()
    endif()
endif()
add_subdirectory(algorithm)
add_subdirectory(io)
add_subdirectory(read_write_queue)
add_subdirectory(thread_pool)
