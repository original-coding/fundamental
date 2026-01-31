if(__patch_helper__)
    return()
endif()
set(__patch_helper__ TRUE )

# apply patches to a directory.
# 
# Parameters:
#   DST_DIR [required]: Path where patched will be applied.
#   PATCHS_DIR [required]: patches dir,patch's name pattern '.*/[0-9]+-.*\.patch$'.
#   LEVEL [optional]: patch applied level
# Note:
# The patches will be applied in ascending order based on the numbers preceding their filenames
function(f_patch_dir)

    cmake_parse_arguments(ARG "" "DST_DIR;PATCHS_DIR;LEVEL" "" ${ARGN})
    set(dst_dir "${ARG_DST_DIR}")

    if(NOT dst_dir)
        message(FATAL_ERROR "you should set DST_DIR")
    endif()

    set(patches_dir "${ARG_PATCHS_DIR}")

    if(NOT patches_dir)
        message(FATAL_ERROR "you should set PATCHS_DIR")
    endif()

    set(level "${ARG_LEVEL}")

    if(NOT level)
        message(STATUS "you can set patch level by LEVEL param,now we use default level 1")
        set(level "1")
    endif()

 
execute_process(
    COMMAND bash -c "${FUNDAMENTAL_SCRIPTS_PATH}/patch-helper.sh ${dst_dir} ${patches_dir} ${level}"
    RESULT_VARIABLE SCRIPT_RETURN_CODE
    OUTPUT_VARIABLE SCRIPT_OUTPUT
    ERROR_VARIABLE SCRIPT_ERROR
)
 
if(NOT "${SCRIPT_RETURN_CODE}" STREQUAL "0")
    message(FATAL_ERROR "patch  ${dst_dir} with ${patches_dir} ${level} failed")
else()
    message(STATUS "patch  ${dst_dir} with ${patches_dir} ${level} successfully.")
endif()
endfunction()