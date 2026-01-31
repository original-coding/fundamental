#include "http_router.hpp"
#include "fundamental/basic/log.h"

#include <memory>

namespace network
{
namespace http
{


std::unordered_map<std::string, MethodFilter> http_router::s_method_str_2_filter = {
    { "GET", MethodFilter::HttpGet },       { "POST", MethodFilter::HttpPost },
    { "PUT", MethodFilter::HttpPut },       { "DELETE", MethodFilter::HttpDelete },
    { "PATCH", MethodFilter::HttpPatch },   { "COPY", MethodFilter::HttpCopy },
    { "HEAD", MethodFilter::HttpHead },     { "OPTIONS", MethodFilter::HttpOptions },
    { "LINK", MethodFilter::HttpLink },     { "UNLINK", MethodFilter::HttpUnlink },
    { "PURGE", MethodFilter::HttpPurge },   { "LOCK", MethodFilter::HttpLock },
    { "UNLOCK", MethodFilter::HttpUnlock }, { "PROPFIND", MethodFilter::HttpPropfind },
    { "VIEW", MethodFilter::HttpView },

};

const http_route_table& http_router::get_table(const std::string& pattern) {
    std::scoped_lock<std::mutex> locker(mutex_);
    auto iter = route_tables.find(pattern);
    if (iter == route_tables.end()) return default_route_table;
    return iter->second;
}

void http_router::update_route_table(const std::string& pattern,
                                     const http_handler& handler,
                                     std::uint32_t method_mask) {
    std::scoped_lock<std::mutex> locker(mutex_);
    FASSERT_ACTION(route_tables.count(pattern) == 0, , "http pattern is alreay existed");
    auto &item        = route_tables[pattern];
    item.handler     = handler;
    item.method_mask = method_mask;
}

void http_router::remove_route_table(const std::string& pattern) {
    std::scoped_lock<std::mutex> locker(mutex_);
    route_tables.erase(pattern);
}

void http_router::set_default_route_table(http_route_table table) {
    std::scoped_lock<std::mutex> locker(mutex_);
    default_route_table = table;
}

MethodFilter http_router::from_method_string(const std::string& httpMethod) {
    auto iter = s_method_str_2_filter.find(httpMethod);
    if (iter != s_method_str_2_filter.end()) {
        return iter->second;
    }
    return MethodFilter::HttpNone;
}
} // namespace http
} // namespace network