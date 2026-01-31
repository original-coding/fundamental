if(__lib_deploy__)
    return()
endif()
set(__lib_deploy__ TRUE)


function(native_copy_dlls app_target_name)
    add_custom_command(TARGET ${app_target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:${app_target_name}> $<TARGET_RUNTIME_DLLS:${app_target_name}>
        COMMAND_EXPAND_LISTS
    )
endfunction(native_copy_dlls)

function(make_deploy_real_path INPUT_PATH)
    get_filename_component(ABSOLUTE_PATH "${${INPUT_PATH}}" ABSOLUTE)
    set(${INPUT_PATH}
        "${ABSOLUTE_PATH}"
        PARENT_SCOPE)
endfunction()




function(native_install_lib_package lib_name version)
    # make cache variables for install destinations
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(ExportTargets "${lib_name}Targets")
    set(ExportConfigVersion "${lib_name}ConfigVersion")
    set(ExportConfig "${lib_name}Config")
    #declare install location
    install(TARGETS ${lib_name} EXPORT ${ExportTargets}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/${lib_name}/$<CONFIG>"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/${lib_name}/$<CONFIG>"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/${lib_name}/$<CONFIG>"
    )
    write_basic_package_version_file(
        ${ExportConfigVersion}.cmake
        VERSION ${version}
        COMPATIBILITY AnyNewerVersion
    )
    #if you need install a lib with multi configurations
    # you should config a lib with muliti configurations
    export(EXPORT ${ExportTargets}
        FILE "${CMAKE_CURRENT_BINARY_DIR}/${ExportTargets}.cmake"
        NAMESPACE fh::
    )
    configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/TemplateLib.cmake.in ${ExportConfig}.cmake @ONLY)
    install(EXPORT ${ExportTargets}
        FILE ${ExportTargets}.cmake
        NAMESPACE fh::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${lib_name}
        COMPONENT "${lib_name}"
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/${ExportConfig}.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${ExportConfigVersion}.cmake"
        COMPONENT "${lib_name}"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${lib_name}
    )
    install(TARGETS ${lib_name}
        COMPONENT "${lib_name}"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/${lib_name}/$<CONFIG>
    )
endfunction()

function(native_copy_extra_dependencies target_name lib_name)
    add_custom_command(TARGET ${target_name}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/${lib_name}/$<CONFIG>" "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

endfunction()


function(read_lib_path lib_name out_path)
    get_target_property(QUERY_LIB_LOCATION ${lib_name} IMPORTED_LOCATION)
    if(QUERY_LIB_LOCATION STREQUAL "QUERY_LIB_LOCATION-NOTFOUND")

        string(TOUPPER "${CMAKE_BUILD_TYPE}" TEMP_CONFIG_TYPE)
        get_target_property(QUERY_LIB_LOCATION ${lib_name} IMPORTED_LOCATION_${TEMP_CONFIG_TYPE})
    endif()

    set(${out_path}
        "${QUERY_LIB_LOCATION}"
        PARENT_SCOPE)
endfunction(read_lib_path)



