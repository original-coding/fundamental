set(src_root_dir ${CMAKE_CURRENT_SOURCE_DIR})
get_filename_component(src_root_dir_name ${src_root_dir} NAME)

set(fundamental_install_prefix ${CMAKE_INSTALL_INCLUDEDIR}/fundamental)
if(FUNDAMENTAL_ENABLE_INSTALL)
    include(lib-deploy)
endif()
message(STATUS "CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")

add_subdirectory(fundamental)
if(FUNDAMENTAL_BUILD_NETWORK)

    add_subdirectory(network)
    add_subdirectory(rpc)
    add_subdirectory(http)
endif()
if(FUNDAMENTAL_ENABLE_DATABASE_SUPPORT)
    add_subdirectory(database)
endif()

