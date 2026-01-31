cmake_minimum_required(VERSION 3.6)
# CMP007 is needed as option variables will be performing differently after 3.16
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set(thirdy_party_dir ${CMAKE_CURRENT_SOURCE_DIR})
get_filename_component(thirdy_party_dir_name ${thirdy_party_dir} NAME)
if(FUNDAMENTAL_BUILD_EVENTS)
    add_subdirectory(eventpp-source)
endif()
set(SPDLOG_BUILD_TESTING OFF)
set(SPDLOG_BUILD_EXAMPLES OFF)
option(SPDLOG_ENABLE_INSTALL "Enable installation of spdlog. (Projects embedding spdlog may want to turn this OFF.)" OFF)
add_subdirectory(spdlog-source)

if(FUNDAMENTAL_BUILD_RTTR)
    set(BUILD_RTTR_DYNAMIC ON)
    set(BUILD_UNIT_TESTS OFF)
    set(BUILD_STATIC ON)
    set(BUILD_WITH_STATIC_RUNTIME_LIBS OFF)
    if(MSVC)
        set(BUILD_WITH_RTTI OFF)
    endif()
    set(BUILD_BENCHMARKS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOCUMENTATION OFF)
    set(BUILD_INSTALLER OFF)
    set(BUILD_PACKAGE OFF)
    set(USE_PCH ON)
    set(CUSTOM_DOXYGEN_STYLE OFF)
    set(BUILD_WEBSITE_DOCU OFF)
    add_subdirectory(rttr-source)
endif()

# copy from https://github.com/nlohmann/json 
add_library(json_lib INTERFACE)
target_include_directories(json_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/nlohmann-source)
add_library(nlohmann::json ALIAS json_lib)

if(FUNDAMENTAL_BUILD_NETWORK)
    add_library(asio_lib INTERFACE)
    target_include_directories(asio_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/asio-source)
    add_library(asio ALIAS asio_lib)
endif()


if(FUNDAMENTAL_ENABLE_SCRIPT_SUPPORT)
    if(TARGET_PLATFORM_LINUX)
        add_subdirectory(quickjspp-source)
    endif()

endif()


if(TARGET_PLATFORM_WINDOWS)
    add_subdirectory(wingetopt)
    target_include_directories(wingetopt INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/wingetopt/src
    )
endif()
