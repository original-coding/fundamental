#pragma once
#include "proxy_defines.h"
#include <memory>
#include <mutex>

#include "fundamental/basic/utils.hpp"

namespace network
{
namespace proxy
{

class ProxyManager {
public:
    virtual ~ProxyManager() = default;
    virtual void AddWsProxyRoute(const std::string& api_route, ProxyHost host);
    virtual void RemoveWsProxyRoute(const std::string& api_route);
    virtual bool GetWsProxyRoute(const std::string& api_route, ProxyHost& host);
    virtual std::shared_ptr<const ProxyHost> GetWsProxyRoute(const std::string& api_route);
protected:
    void remove_invalid_cache_route();
private:
    mutable std::mutex dataMutex;
    std::unordered_map<std::string, std::shared_ptr<ProxyHost>> ws_proxy_routes;
};

} // namespace proxy
} // namespace network