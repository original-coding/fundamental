cmake_minimum_required(VERSION 3.16)

set(project_name project-fundamental)
project(${project_name} VERSION 0.1 LANGUAGES CXX C)

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
    add_subdirectory(third-parties)
    add_subdirectory(src)

    if(BUILD_TESTING)
        add_subdirectory(samples)
    endif()
    add_subdirectory(applications)
endif()




