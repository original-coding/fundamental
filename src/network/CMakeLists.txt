# Set lib_name automatically to its folder name
get_filename_component(lib_name ${CMAKE_CURRENT_LIST_DIR} NAME)

file(GLOB lib_source
    *.h
    *.cpp
    *.hpp
    rudp/*.hpp
    rudp/*.cpp
    rudp/kcp_imp/*.h
    rudp/kcp_imp/*.c
)
option(NETWORK_DISABLE_SSL "disable network ssl support." OFF)


# Add to source group, TREE will enable cmake to config the folder layout automatically
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${lib_source})

filter_test_source_files(lib_source)

add_library(${lib_name} STATIC ${lib_source})

set_target_properties(${lib_name} PROPERTIES
    FOLDER ${src_root_dir_name}
)


target_include_directories(${lib_name} PUBLIC
    $<BUILD_INTERFACE:${src_root_dir}>
    $<INSTALL_INTERFACE:${fundamental_install_prefix}>
)

target_compile_definitions(${lib_name}
    PUBLIC
    ASIO_NO_TYPEID=1
)
if(FUNDAMENTAL_ENABLE_INSTALL)
    native_install_lib_package(${lib_name} "${FUNDAMENTAL_VERSION}" BuildSettings)
endif()
# Config link dependencies
target_link_libraries(${lib_name}
    PRIVATE
    BuildSettings
    PUBLIC
    ${GLOB_NAMESPACE}fundamental${STATIC_LIB_SUFFIX}
    asio
)
if(NETWORK_DISABLE_SSL)
    target_compile_definitions(${lib_name}
        PUBLIC
        NETWORK_DISABLE_SSL=1
    )
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(${lib_name}
        PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
    )
endif()
add_library(${GLOB_NAMESPACE}${lib_name} ALIAS ${lib_name})
config_disable_rtti(${lib_name})

if(FUNDAMENTAL_ENABLE_INSTALL)
    # Install the headers
    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
        COMPONENT "${lib_name}_install"
        DESTINATION ${fundamental_install_prefix}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.ipp" PATTERN "*.hh"
    )
endif()
