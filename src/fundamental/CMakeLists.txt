# Set lib_name automatically to its folder name
get_filename_component(lib_name ${CMAKE_CURRENT_LIST_DIR} NAME)

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
        list(APPEND extra_libs chaiscript)
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

add_library(${lib_name}_property INTERFACE)


target_include_directories(${lib_name}_property INTERFACE
    ${src_root_dir}
)

# Config link dependencies
target_link_libraries(${lib_name}_property INTERFACE
    ${extra_libs}
    spdlog
    nlohmann::json
    BuildSettings
)


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



