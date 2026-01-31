if(F_BUILD_STATIC)
  if(BUILD_TESTING)
    # add test
    add_unit_test_executable(thread_pool_test thread_pool_test.cpp "" "")
    if(TARGET thread_pool_test)
      target_link_libraries(thread_pool_test
        PRIVATE
        ${current_test_lib_name}
      )
    endif()
  endif()
endif()
