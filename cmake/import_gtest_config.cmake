if(__import_gtest_config__)
  return()
endif()
set(__import_gtest_config__ TRUE)


option(IMPORT_GTEST "import gtest" ON)
if(IMPORT_GTEST)

  find_package(GTest QUIET)
  if(GTest_FOUND)
    message(STATUS "use system gtest")
  else()
    message(STATUS "use custom gtest")
    include(FetchContent)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY http://192.168.50.101:8787/rdd/googletest.git
      GIT_TAG 58d77fa8070e8cec2dc1ed015d66b454c8d78850 # release-1.12.1
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    option(BUILD_GMOCK "Builds the googlemock subproject" OFF)
    option(INSTALL_GTEST "Enable installation of googletest. (Projects embedding googletest may want to turn this OFF.)" OFF)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    include(GoogleTest)
  endif()

  add_library(import_gtest_interface INTERFACE)

  if(TARGET GTest::gtest)
    target_link_libraries(import_gtest_interface
      INTERFACE
      GTest::gtest)
  else()
    target_link_libraries(import_gtest_interface
      INTERFACE
      gtest)
  endif()


  function(declare_gtest_exe target_name)

    target_link_libraries(${target_name}
      PRIVATE
      import_gtest_interface
    )
    gtest_discover_tests(${target_name})
  endfunction()

endif()
