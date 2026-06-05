# Set App target name
get_filename_component(app_target_name ${CMAKE_CURRENT_LIST_DIR} NAME)

file(GLOB_RECURSE file_source
    "src/*.h" "src/*.cpp" "src/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${file_source})

set(file_resources
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${file_resources})

add_executable(${app_target_name} "${file_source}" "${file_resources}")

if(MSVC)
    set_property(TARGET ${app_target_name} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR})
endif()

target_include_directories(${app_target_name}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(${app_target_name} PROPERTIES
    FOLDER ${apps_dir_name}
)
target_link_libraries(${app_target_name}
    PRIVATE
    BuildSettings
    ${GLOB_NAMESPACE}network
    ${GLOB_NAMESPACE}fundamental
)

target_compile_definitions(${app_target_name}
    PRIVATE
    APP_NAME="${app_target_name}"
)

config_strip_debug_info(${app_target_name})
