#pragma once

#include "fundamental/basic/buffer.hpp"
#include "fundamental/delay_queue/delay_queue.h"

#include <functional>
#include <unordered_map>

namespace network
{
namespace proxy
{
class ProxyManager;
using proxy_update_func = std::function<void(std::string& /*service*/, std::string& /*service*/)>;


enum proxy_host_type : std::uint32_t
{
    raw_tcp_proxy,
    ws_forward_proxy,
};

struct ProxyHost {
    friend class ProxyManager;
    // 10min
    static constexpr std::int64_t kDefaultMaxIdleCacheMsec = 10 * 60 * 1000;
    std::string host;
    std::string service;
    proxy_update_func update_func;
    bool enable_cache_auto_remove           = false;
    proxy_host_type host_type               = raw_tcp_proxy;
    std::int64_t last_active_msec_timestamp = 0;

protected:
    void access_host() {
        last_active_msec_timestamp = Fundamental::Timer::GetTimeNow<std::chrono::milliseconds>();
    }
    bool valid_cache(std::int64_t current_timestamp_msec) const {
        return !enable_cache_auto_remove ||
               (last_active_msec_timestamp + kDefaultMaxIdleCacheMsec > current_timestamp_msec);
    }
    void update() {
        if (!update_func) return;
        update_func(host, service);
    }
};

} // namespace proxy
} // namespace network