set(apps_dir ${CMAKE_CURRENT_SOURCE_DIR})
get_filename_component(apps_dir_name ${apps_dir} NAME)

if(F_BUILD_STATIC)
    if(FUNDAMENTAL_BUILD_NETWORK)
        add_subdirectory(tcp_custom_proxy_server)
        add_subdirectory(rudp_delay_test_server)
        add_subdirectory(frp_proxy_server)
        add_subdirectory(frp_proxy_client)
    endif()
endif()

