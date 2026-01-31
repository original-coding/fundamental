#include "memory_tracker.hpp"
#if ENABLE_JEMALLOC_MEMORY_PROFILING
    #include <jemalloc/jemalloc.h>
#endif

namespace Fundamental
{
void EnableMemoryProfiling() {
#if ENABLE_JEMALLOC_MEMORY_PROFILING
    bool active = true;
    // Enable profiling
    ::mallctl("prof.active", NULL, NULL, &active, sizeof(bool));
#endif
}

void DumpMemorySnapShot([[maybe_unused]] const std::string& out_path) {
#if ENABLE_JEMALLOC_MEMORY_PROFILING
    auto p = out_path.c_str();
    mallctl("prof.dump", NULL, NULL, &p, sizeof(p));
#endif
}
} // namespace Fundamental