if(__import_benchmark_config__)
  return()
endif()
set(__import_benchmark_config__ TRUE)


option(IMPORT_BENCHMARK "import benchmark" ON)
option(BENCHMARK_ENABLE_TESTING "Enable testing of the benchmark library." OFF)
if(IMPORT_BENCHMARK)
  find_package(benchmark QUIET)
  if(benchmark_FOUND)
    message(STATUS "use system benchmark")
  else()
    message(STATUS "use custom benchmark")
    include(FetchContent)
    FetchContent_Declare(
      benchmark
      GIT_REPOSITORY http://192.168.50.101:8787/rdd/benchmark.git
      GIT_TAG f4f93b5553ced834b2120048f65690cddb4b7a2f
    )
    option(BENCHMARK_ENABLE_TESTING "Enable testing of the benchmark library." OFF)
    option(BENCHMARK_ENABLE_GTEST_TESTS "Enable building the unit tests which depend on gtest" OFF)
    option(BENCHMARK_USE_BUNDLED_GTEST "Use bundled GoogleTest. If disabled, the find_package(GTest) will be used." OFF)
    option(BENCHMARK_ENABLE_INSTALL "Enable installation of benchmark. (Projects embedding benchmark may want to turn this OFF.)" OFF)
    option(BENCHMARK_ENABLE_DOXYGEN "Build documentation with Doxygen." OFF)
    option(BENCHMARK_INSTALL_DOCS "Enable installation of documentation." OFF)
    FetchContent_MakeAvailable(benchmark)
  endif()
  add_library(import_benchmark_interface INTERFACE)
  if(TARGET benchmark::benchmark)
    target_link_libraries(import_benchmark_interface
      INTERFACE
      benchmark::benchmark)
  elseif(TARGET benchmark)
    target_link_libraries(import_benchmark_interface
      INTERFACE
      benchmark)
  else()
    message(FATAL_ERROR "Found benchmark but no valid target")
  endif()


  function(declare_benchmark_exe target_name)
    target_link_libraries(${target_name}
      PRIVATE
      import_benchmark_interface)


    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "NVHPC")
      target_compile_options(${target_name} PRIVATE --diag_suppress partial_override)
    endif()

  endfunction()
  macro(benchmark_add_test)
    add_test(${ARGV})
    if(WIN32 AND BUILD_SHARED_LIBS)
      cmake_parse_arguments(TEST "" "NAME" "" ${ARGN})
      set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:benchmark::benchmark>")
    endif()
  endmacro(benchmark_add_test)
endif()

