#include "proxy_manager.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
namespace network
{
namespace proxy
{
void ProxyManager::AddWsProxyRoute(const std::string& api_route, ProxyHost host) {
    std::scoped_lock<std::mutex> locker(dataMutex);
    auto& dst_host = ws_proxy_routes[api_route];
    if (!dst_host) dst_host = std::make_shared<ProxyHost>();
    *dst_host = host;
    dst_host->access_host();
}

void ProxyManager::RemoveWsProxyRoute(const std::string& api_route) {
    std::scoped_lock<std::mutex> locker(dataMutex);
    ws_proxy_routes.erase(api_route);
}

bool ProxyManager::GetWsProxyRoute(const std::string& api_route, ProxyHost& host) {
    auto ret = GetWsProxyRoute(api_route);
    if (ret) {
        host = *ret;
        return true;
    }
    return false;
}

std::shared_ptr<const ProxyHost> ProxyManager::GetWsProxyRoute(const std::string& api_route) {

    std::scoped_lock<std::mutex> locker(dataMutex);
    Fundamental::ScopeGuard clear_cache([this]() { remove_invalid_cache_route(); });
    auto iter = ws_proxy_routes.find(api_route);
    if (iter != ws_proxy_routes.end()) {
        FASSERT(iter->second, "route entry memory init error,check your code");
        iter->second->update();
        iter->second->access_host();
        return iter->second;
    }
    return {};
}

void ProxyManager::remove_invalid_cache_route() {
    auto time_now = Fundamental::Timer::GetTimeNow<std::chrono::milliseconds>();
    for (auto iter = ws_proxy_routes.begin(); iter != ws_proxy_routes.end();) {
        FASSERT(iter->second, "route entry memory init error,check your code");
        auto& item = *(iter->second);
        if (!item.enable_cache_auto_remove || item.valid_cache(time_now) || iter->second.use_count() > 1) {
            ++iter;
            continue;
        }
        FWARN("{} proxy to {} {} will be removed", iter->first, item.host, item.service);
        ws_proxy_routes.erase(iter++);
    }
}
} // namespace proxy
} // namespace network