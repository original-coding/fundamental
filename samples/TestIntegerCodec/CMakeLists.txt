# Set App target name
get_filename_component(app_target_name ${CMAKE_CURRENT_LIST_DIR} NAME)
set(app_ide_folder ${samples_dir_name}/${app_target_name})

# Set the source files to compile
file(GLOB_RECURSE file_source 
    "src/*.h" "src/*.cpp" "src/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${file_source})


# Set resource files
set(file_resources
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${file_resources})

# Create the executable
add_executable(${app_target_name}  "${file_source}"  "${file_resources}")

set_target_properties(${app_target_name} PROPERTIES
    FOLDER ${samples_dir_name}
)

if(MSVC)
    set_property(TARGET ${app_target_name} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR})
endif()

# Config includes
target_include_directories(${app_target_name}
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Config links
target_link_libraries(${app_target_name} 
PRIVATE
    BuildSettings
    ${GLOB_NAMESPACE}fundamental${STATIC_LIB_SUFFIX}
)


# Add target name
target_compile_definitions(${app_target_name}
    PRIVATE
        APP_NAME="${app_target_name}"
)

declare_gtest_exe(${app_target_name})