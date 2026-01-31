include(CheckCXXCompilerFlag)


file(WRITE ${CMAKE_BINARY_DIR}/dummy.cpp "
    #include <ranges>
    #include <vector>
    int main() {
        std::vector<int> v{1, 2, 3};
        auto even = v | std::views::filter([](int x){ return x % 2 == 0; });
        return even.empty() ? 0 : 1;
    }
")

try_compile(
    CXX20_SUPPORTED                
    ${CMAKE_BINARY_DIR}            
    SOURCES ${CMAKE_BINARY_DIR}/dummy.cpp
    CXX_STANDARD 20               
    CXX_STANDARD_REQUIRED ON    
    OUTPUT_VARIABLE COMPILE_OUTPUT  
)

set(ENV_CXX20_SUPPORTED "${CXX20_SUPPORTED}" CACHE INTERNAL "")
message(STATUS "ENV_CXX20_SUPPORTED=${ENV_CXX20_SUPPORTED} CXX20_SUPPORTED=${CXX20_SUPPORTED}")


include(CheckCXXSourceCompiles)

check_cxx_source_compiles("
  #include <filesystem>
  int main() { std::filesystem::path p(\".\"); return 0; }
"    HAS_STD_FILESYSTEM)
message(STATUS "HAS_STD_FILESYSTEM=${HAS_STD_FILESYSTEM}")
set(HAS_STD_FILESYSTEM "${HAS_STD_FILESYSTEM}" CACHE INTERNAL "")


check_cxx_source_compiles("
  #include <memory_resource>
  int main() { std::pmr::pool_options option; return 0; }
"    HAS_STD_MEMORY_SOURCE)
message(STATUS "HAS_STD_MEMORY_SOURCE=${HAS_STD_MEMORY_SOURCE}")
set(HAS_STD_MEMORY_SOURCE "${HAS_STD_MEMORY_SOURCE}" CACHE INTERNAL "")