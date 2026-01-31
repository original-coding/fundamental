# Set App target name
get_filename_component(lib_name ${CMAKE_CURRENT_LIST_DIR} NAME)
set(app_ide_folder ${samples_dir_name}/${lib_name})

# Set the source files to compile
file(GLOB_RECURSE file_source
  "src/*.h" "src/*.cpp" "src/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${file_source})

# Create the executable
add_library(${lib_name} STATIC "${file_source}")

set_target_properties(${lib_name} PROPERTIES
    FOLDER ${samples_dir_name}
)

# Config includes
target_include_directories(${lib_name}
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<INSTALL_INTERFACE:include>
)

# Config links
target_link_libraries(${lib_name}
  PRIVATE

)
include(lib-deploy)
native_install_lib_package(${lib_name} "1.0.0")

install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/src
  COMPONENT "${lib_name}"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${lib_name}
  FILES_MATCHING
  PATTERN "*.h"
  PATTERN "*.hpp")

add_custom_command(TARGET ${lib_name}
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E echo " installing"
  COMMAND ${CMAKE_COMMAND} --install . --component ${lib_name}
  COMMENT "post-build  install"
  VERBATIM)

