cmake_minimum_required(VERSION 3.16)
set(VERSION_MAJOR 1)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)
set(FUNDAMENTAL_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
set(project_name project-fundamental)
project(${project_name} VERSION ${FUNDAMENTAL_VERSION} LANGUAGES CXX C)

set(FUNDAMENTAL_MAIN_PROJECT OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(FUNDAMENTAL_MAIN_PROJECT ON)
endif()
include(CTest)
if(FUNDAMENTAL_MAIN_PROJECT)
    enable_testing()
else()
    set(BUILD_TESTING OFF)
endif()
include(CMakeDependentOption)
cmake_dependent_option(FUNDAMENTAL_BUILD_TESTING
    "Build fundamental tests" ON
    "BUILD_TESTING" OFF
)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/ca_root.crt")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf "${CMAKE_CURRENT_SOURCE_DIR}/assets/pem.tar"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        RESULT_VARIABLE tar_result
    )
    if(NOT tar_result EQUAL 0)
        message(WARNING "Failed to extract pem.tar")
    endif()
endif()
option(FUNDAMENTAL_IMPORT_SRC "import fundamental lib source" ON)
option(ONLY_GENERATE_FUNDAMENTAL_DEPS "generate fundamental lib deps source" OFF)
option(USE_PRECOMPILE_FUNDAMENTAL_DEPS "using precompiled fundamental lib deps source" OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Enbale code IDE to use folders, this will enable source_group(TREE,...)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
#add cmake search directory
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(FUNDAMENTAL_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake CACHE INTERNAL "")
set(FUNDAMENTAL_ASSETS_PATH ${CMAKE_CURRENT_SOURCE_DIR}/assets CACHE INTERNAL "")
set(FUNDAMENTAL_SCRIPTS_PATH ${CMAKE_CURRENT_SOURCE_DIR}/scripts CACHE INTERNAL "")
#build with -fpic
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Make sure third party is working
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
# find_package() uses <PackageName>_ROOT variables
cmake_policy(SET CMP0074 NEW)

# other source import config definitions
# if you import fundametal as a subsource project,you should change the option value to minimize the dependencies
include(cmake/source_import_config.cmake)
include(cmake/config-target.cmake)
include(cmake/reflect-helper.cmake)

if(FUNDAMENTAL_IMPORT_SRC)
    include(GNUInstallDirs)
    if(ONLY_GENERATE_FUNDAMENTAL_DEPS)
        add_subdirectory(third-parties)
    else()

        if(USE_PRECOMPILE_FUNDAMENTAL_DEPS)
            find_package(eventpp CONFIG REQUIRED)
            find_package(json CONFIG REQUIRED)
            find_package(asio CONFIG REQUIRED)
            find_package(blake3 CONFIG REQUIRED)
            find_package(rttr CONFIG REQUIRED)
            find_package(spdlog CONFIG REQUIRED)
            find_package(quickjs CONFIG REQUIRED)
            find_package(quickjspp CONFIG REQUIRED)
        else()
            set(FUNDAMENTAL_ENABLE_INSTALL OFF CACHE BOOL "Disable installation" FORCE)
            message(STATUS "FUNDAMENTAL_ENABLE_INSTALL=${FUNDAMENTAL_ENABLE_INSTALL}")
            add_subdirectory(third-parties)
        endif()
        add_subdirectory(src)

        if(FUNDAMENTAL_BUILD_TESTING)
            add_subdirectory(samples)
        endif()
        if(FUNDAMENTAL_BUILD_APPLICATIONS)
            add_subdirectory(applications)
        endif()
    endif()
endif()




