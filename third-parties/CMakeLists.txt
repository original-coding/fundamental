cmake_minimum_required(VERSION 3.6)
# CMP007 is needed as option variables will be performing differently after 3.16
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
include(lib-deploy)
set(thirdy_party_dir ${CMAKE_CURRENT_SOURCE_DIR})
get_filename_component(thirdy_party_dir_name ${thirdy_party_dir} NAME)
if(FUNDAMENTAL_BUILD_EVENTS)
    if(FUNDAMENTAL_ENABLE_INSTALL)
        set(EVENTPP_INSTALL ON)
    endif()
    add_subdirectory(eventpp-source)
endif()
set(SPDLOG_BUILD_TESTING OFF)
set(SPDLOG_BUILD_EXAMPLES OFF)
option(SPDLOG_ENABLE_INSTALL "Enable installation of spdlog. (Projects embedding spdlog may want to turn this OFF.)" ${FUNDAMENTAL_ENABLE_INSTALL})
add_subdirectory(spdlog-source)
if(NOT SPDLOG_ENABLE_INSTALL)
    add_library(spdlog::spdlog ALIAS spdlog)
endif()
add_subdirectory(blake3)
if(FUNDAMENTAL_ENABLE_INSTALL)
    install(
        FILES blake3/blake3.h
        COMPONENT "blake3_install"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    native_install_external_lib_package(blake3 "1.8.2" BLAKE3)
endif()
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
    set(BUILD_INSTALLER ${FUNDAMENTAL_ENABLE_INSTALL})
    set(BUILD_PACKAGE OFF)
    set(USE_PCH ON)
    set(CUSTOM_DOXYGEN_STYLE OFF)
    set(BUILD_WEBSITE_DOCU OFF)
    add_subdirectory(rttr-source)
endif()


# copy from https://github.com/nlohmann/json 
add_library(json INTERFACE)
target_include_directories(json INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/nlohmann-source>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
add_library(nlohmann::json ALIAS json)
if(FUNDAMENTAL_ENABLE_INSTALL)
    native_install_external_lib_package(json "3.11.3" nlohmann)
    # Install the headers
    install(
        DIRECTORY nlohmann-source/
        COMPONENT "json_install"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.ipp" PATTERN "*.hh"
    )
endif()
if(FUNDAMENTAL_BUILD_NETWORK)
    add_library(asio INTERFACE)
    target_include_directories(asio INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/asio-source>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    # Install the headers
    install(
        DIRECTORY asio-source/
        COMPONENT "asio_install"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.ipp" PATTERN "*.hh"
    )
    native_install_external_lib_package(asio "1.32.0" "")
endif()


if(FUNDAMENTAL_ENABLE_SCRIPT_SUPPORT)
    if(TARGET_PLATFORM_LINUX)
        add_subdirectory(quickjspp-source)
        if(FUNDAMENTAL_ENABLE_INSTALL)
            native_install_external_lib_package(quickjspp "1.1.0" "")
            native_install_external_lib_package(quickjs "1.1.0" "")
            # Install the headers
            install(
                FILES quickjspp-source/quickjspp/quickjspp.hpp
                COMPONENT "quickjspp_install"
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            )
        endif()
    endif()

endif()


if(TARGET_PLATFORM_WINDOWS)
    add_subdirectory(wingetopt)
    target_include_directories(wingetopt INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/wingetopt/src
    )
endif()
