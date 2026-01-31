# Set lib_name automatically to its folder name
get_filename_component(lib_name ${CMAKE_CURRENT_LIST_DIR} NAME)

file(GLOB lib_source
    *.h
    *.cpp
    *.hpp
)



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
    ${GLOB_NAMESPACE}network
)
add_library(${GLOB_NAMESPACE}${lib_name} ALIAS ${lib_name})
config_disable_rtti(${lib_name})