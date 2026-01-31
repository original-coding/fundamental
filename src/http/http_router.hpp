#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "http_definitions.hpp"

namespace network
{
namespace http
{
class http_connection;
class http_response;
class http_request;
using http_handler = std::function<void(std::shared_ptr<http_connection>, http_response&, http_request&)>;
struct http_route_table {
    http_handler handler;
    std::uint32_t method_mask = 0;
};
struct http_router {
public:
    http_router(std::string doc_root_path = "") : doc_root_path(doc_root_path) {
    }
    static std::unordered_map<std::string, MethodFilter> s_method_str_2_filter;
    static MethodFilter from_method_string(const std::string& httpMethod);
    const http_route_table& get_table(const std::string& pattern);
    void update_route_table(const std::string& pattern, const http_handler& handler, std::uint32_t method_mask);
    void remove_route_table(const std::string& pattern);
    void set_default_route_table(http_route_table table);
    decltype(auto) root_path() const {
        return doc_root_path;
    }

private:
    const std::string doc_root_path;
    std::mutex mutex_;
    http_route_table default_route_table;
    std::unordered_map<std::string, http_route_table> route_tables;
};
} // namespace http
} // namespace network