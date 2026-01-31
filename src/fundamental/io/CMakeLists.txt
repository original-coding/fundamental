

if(F_BUILD_STATIC)
  if(BUILD_TESTING)
    # add test
    add_unit_test_executable(csv_utils_test csv/csv_utils_test.cpp "" "")
  endif()
endif()
