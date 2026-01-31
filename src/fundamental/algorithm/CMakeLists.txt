if(F_BUILD_STATIC)
  if(BUILD_TESTING)
    # add test
    add_unit_test_executable(blake3_hasher_utils_test blake3_hasher_utils_test.cpp "" "")
    if(TARGET blake3_hasher_utils_test)
      target_link_libraries(blake3_hasher_utils_test
        PRIVATE
        ${current_test_lib_name}
      )
    endif()
    add_unit_test_executable(algorithm_common_test algorithm_common_test.cpp "" "")
  endif()
endif()
