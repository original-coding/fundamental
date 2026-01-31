#pragma once
#include <cstdint>
#include <memory>

namespace Fundamental
{
struct storage_config {
    bool overwrite                  = false;
    std::size_t expired_time_msec   = 0;
    std::function<void()> remove_cb = nullptr;
};
} // namespace Fundamental