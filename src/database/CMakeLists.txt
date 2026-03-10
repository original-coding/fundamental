# Set lib_name automatically to its folder name
get_filename_component(lib_name ${CMAKE_CURRENT_LIST_DIR} NAME)

file(GLOB lib_source
    *.h
    *.cpp
    *.hpp
)

if(FUNDAMENTAL_ENABLE_SQLITE3_SUPPORT)
    file(GLOB sqlite3_src
        sqlite3/*.h
        sqlite3/*.cpp
        sqlite3/*.hpp
    )
    list(APPEND lib_source ${sqlite3_src})
endif()

if(FUNDAMENTAL_ENABLE_ROCKSDB_SUPPORT)
    file(GLOB rocksdb_src
        rocksdb/*.h
        rocksdb/*.cpp
        rocksdb/*.hpp
    )
    list(APPEND lib_source ${rocksdb_src})
endif()

# Add to source group, TREE will enable cmake to config the folder layout automatically
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${lib_source})

filter_test_source_files(lib_source)

add_library(${lib_name} STATIC ${lib_source})
if(FUNDAMENTAL_ENABLE_SQLITE3_SUPPORT)
    if(TERGET_PLATFORM_LINUX)

        if(SQLITE3_FOUND)
            target_include_directories(${lib_name} INTERFACE
                ${SQLITE3_INCLUDE_DIRS}
            )
            target_link_libraries(${lib_name} INTERFACE
                ${SQLITE3_LIBRARIES}
            )

        else()
            message(FATAL_ERROR "you should install libsqlite3-dev package")
        endif()
    else()
        find_package(SQLite3 REQUIRED)
        target_link_libraries(${lib_name} INTERFACE SQLite::SQLite3)
    endif()
    if(FUNDAMENTAL_ENABLE_SQLITE3_LOADABLE_EXT_SUPPORT)
        target_compile_definitions(${lib_name} INTERFACE IMPORT_SQLITE_LOADABLE_EXTENSION)
    endif()
endif()

if(FUNDAMENTAL_ENABLE_ROCKSDB_SUPPORT)
    # librocksdb-dev should be installed firstly 
    ## find_package(rocksdb REQUIRED)
    ##if(rocksdb_FOUND)
    target_link_libraries(${lib_name} INTERFACE
        rocksdb
    )

    ##else()
    ##    message(FATAL_ERROR "you should install librocksdb-dev package")
    ##endif()
endif()
set_target_properties(${lib_name} PROPERTIES
    FOLDER ${src_root_dir_name}
)

target_include_directories(${lib_name} PUBLIC
    $<BUILD_INTERFACE:${src_root_dir}>
    $<INSTALL_INTERFACE:${fundamental_install_prefix}>
)

# Config link dependencies
target_link_libraries(${lib_name}
    PRIVATE
    BuildSettings
)
if(FUNDAMENTAL_ENABLE_INSTALL)
    native_install_lib_package(${lib_name} "${FUNDAMENTAL_VERSION}" BuildSettings)
endif()
add_library(${GLOB_NAMESPACE}${lib_name} ALIAS ${lib_name})

if(FUNDAMENTAL_ENABLE_INSTALL)
    # Install the headers
    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
        COMPONENT "${lib_name}_install"
        DESTINATION ${fundamental_install_prefix}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.ipp" PATTERN "*.hh"
    )
endif()
