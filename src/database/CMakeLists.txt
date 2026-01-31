# Set lib_name automatically to its folder name
get_filename_component(lib_name ${CMAKE_CURRENT_LIST_DIR} NAME)

file(GLOB lib_source
    *.h
    *.cpp
    *.hpp
)

add_library(${lib_name}_property INTERFACE)
if(FUNDAMENTAL_ENABLE_SQLITE3_SUPPORT)
    file(GLOB sqlite3_src
        sqlite3/*.h
        sqlite3/*.cpp
        sqlite3/*.hpp
    )
    list(APPEND lib_source ${sqlite3_src})
if(TERGET_PLATFORM_LINUX)
    # we should get sqlite3 with pkgconfig
    # libsqlite3-dev should be installed firstly 
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    if(SQLITE3_FOUND)
        target_include_directories(${lib_name}_property INTERFACE
            ${SQLITE3_INCLUDE_DIRS}
        )
        target_link_libraries(${lib_name}_property INTERFACE
            ${SQLITE3_LIBRARIES}
        )

    else()
        message(FATAL_ERROR "you should install libsqlite3-dev package")
    endif()
else()
    find_package(SQLite3 REQUIRED)
    target_link_libraries(${lib_name}_property INTERFACE SQLite::SQLite3)
endif()
    if(FUNDAMENTAL_ENABLE_SQLITE3_LOADABLE_EXT_SUPPORT)
        target_compile_definitions(${lib_name}_property INTERFACE IMPORT_SQLITE_LOADABLE_EXTENSION)
    endif()
endif()

if(FUNDAMENTAL_ENABLE_ROCKSDB_SUPPORT)
    file(GLOB rocksdb_src
        rocksdb/*.h
        rocksdb/*.cpp
        rocksdb/*.hpp
    )
    list(APPEND lib_source ${rocksdb_src})
    # librocksdb-dev should be installed firstly 
    ## find_package(rocksdb REQUIRED)
    ##if(rocksdb_FOUND)
        target_link_libraries(${lib_name}_property INTERFACE
            rocksdb
        )

    ##else()
    ##    message(FATAL_ERROR "you should install librocksdb-dev package")
    ##endif()
endif()

# Add to source group, TREE will enable cmake to config the folder layout automatically
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${lib_source})

filter_test_source_files(lib_source)

add_library(${lib_name} STATIC ${lib_source})
set_target_properties(${lib_name} PROPERTIES
    FOLDER ${src_root_dir_name}
)

target_include_directories(${lib_name} PUBLIC
    ${src_root_dir}
)




# Config link dependencies
target_link_libraries(${lib_name} PUBLIC
    BuildSettings
    ${lib_name}_property
)

add_library(${GLOB_NAMESPACE}${lib_name} ALIAS ${lib_name})
