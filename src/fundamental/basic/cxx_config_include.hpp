#pragma once

#ifdef USE_EXPERIMENTAL_FILESYSTEM
    #include <experimental/filesystem>
namespace std_fs = std::experimental::filesystem;
#else
    #include <filesystem>
namespace std_fs = std::filesystem;
#endif

#ifdef USE_EXPERIMENTAL_MEMORY_SOURCE
    #include <experimental/memory_resource>
namespace std_pmr = std::experimental::pmr;
#else
    #include <memory_resource>

namespace std_pmr = std::pmr;
#endif