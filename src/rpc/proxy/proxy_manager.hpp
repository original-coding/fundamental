#pragma once
#include "fundamental/basic/utils.hpp"
#include "proxy_defines.h"
#include <memory>
#include <mutex>
#include <set>

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
    virtual void AddMatchPrefixPath(std::string prefix);

protected:
    void remove_invalid_cache_route();

private:
    mutable std::mutex dataMutex;
    std::unordered_map<std::string, std::shared_ptr<ProxyHost>> ws_proxy_routes;
    std::set<std::string> match_prefixs;
};

} // namespace proxy
} // namespace network