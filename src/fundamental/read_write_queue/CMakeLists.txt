if(F_BUILD_STATIC)
  if(BUILD_TESTING)
    # add test
    add_unit_test_executable(step_task_queue_executor_test step_task_queue_executor_test.cpp "" "")
    if(TARGET step_task_queue_executor_test)
      target_link_libraries(step_task_queue_executor_test
        PRIVATE
        ${current_test_lib_name}
      )
    endif()
  endif()
endif()
