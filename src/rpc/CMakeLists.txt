# Set lib_name automatically to its folder name
get_filename_component(lib_name ${CMAKE_CURRENT_LIST_DIR} NAME)

file(GLOB lib_source
    *.h
    *.cpp
    *.hpp
    basic/*.h
    basic/*.cpp
    basic/*.hpp
    proxy/*.h
    proxy/*.cpp
    proxy/*.hpp
    proxy/socks5/*.h
    proxy/socks5/*.cpp
    proxy/socks5/*.hpp
    proxy/websocket/*.h
    proxy/websocket/*.cpp
    proxy/websocket/*.hpp
    proxy/protocal_pipe/*.h
    proxy/protocal_pipe/*.cpp
    proxy/protocal_pipe/*.hpp
    proxy/frp/*.h
    proxy/frp/*.cpp
    proxy/frp/*.hpp
    netlink/*.h
    netlink/*.cpp
    netlink/*.hpp
)

# Add to source group, TREE will enable cmake to config the folder layout automatically
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${lib_source})

filter_test_source_files(lib_source)


add_library(${lib_name} STATIC ${lib_source})

set_target_properties(${lib_name} PROPERTIES
    FOLDER ${src_root_dir_name}
)

#target_compile_definitions(${lib_name} PUBLIC RPC_VERBOSE=1)
#target_compile_definitions(${lib_name} PUBLIC RPC_DATA_VERBOSE=1)
target_include_directories(${lib_name} PUBLIC
    ${src_root_dir}
)


# Config link dependencies
target_link_libraries(${lib_name} PUBLIC
    BuildSettings
    ${GLOB_NAMESPACE}network
)


add_library(${GLOB_NAMESPACE}${lib_name} ALIAS ${lib_name})


if(BUILD_TESTING)
    # add test
    add_unit_test_executable(netlink_test netlink/netlink_test.cpp "" "")
    if(TARGET netlink_test)
        target_link_libraries(netlink_test
            PRIVATE
            ${lib_name}
        )
    endif()
endif()
