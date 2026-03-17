if(F_BUILD_STATIC)
  if(FUNDAMENTAL_BUILD_TESTING)
    # add test
    add_unit_test_executable(events_tracker_test events_tracker_test.cpp "" "")
    if(TARGET events_tracker_test)
      target_link_libraries(events_tracker_test
        PRIVATE
        ${current_test_lib_name}
      )
    endif()
  endif()
endif()
