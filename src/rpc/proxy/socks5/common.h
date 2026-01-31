#pragma once

#include "socks5_type.h"

#include <cstring>
#include <fstream>
#include <functional>
#include <list>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <asio.hpp>

#include "fundamental/basic/error_code.hpp"
#include "fundamental/events/event_system.h"

namespace SocksV5
{

class noncopyable {
protected:
    noncopyable() {
    }
    ~noncopyable() {
    }
    noncopyable(const noncopyable&)                  = delete;
    const noncopyable& operator=(const noncopyable&) = delete;
};

namespace convert
{

template <typename InternetProtocol>
std::string format_address(const asio::ip::basic_endpoint<InternetProtocol>& endpoint) {
    if (endpoint.address().is_v6()) {
        return "[" + endpoint.address().to_string() + "]" + ":" + std::to_string(endpoint.port());
    }
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

std::string dst_to_string(const std::vector<uint8_t>& dst_addr, Socks5HostType addr_type);

} // namespace convert

struct Sock5Handler {
    static auto make_handler() {
        return std::make_shared<Sock5Handler>();
    }
    static auto make_default_handler() {
        auto ret = make_handler();

        ret->user_verify_handler.Connect(default_user_verify_func);
        ret->method_verify_handler.Connect(default_method_verify_func);
        return ret;
    }
    static void default_user_verify_func(const std::string& user,
                                         const std::string& passwd,
                                         Fundamental::error_code& ec) {
        static const std::unordered_map<std::string, std::string> s_default_auth { { "fongwell", "fongwell123456" } };
        auto iter = s_default_auth.find(user);
        if (iter == s_default_auth.end() || iter->second != passwd) {
            ec =
                Fundamental::make_error_code(std::errc::permission_denied, std::system_category(), "invalid user info");
        }
    }
    static void default_method_verify_func(SocksV5::Method method, Fundamental::error_code& ec) {
        using method_type = std::underlying_type_t<decltype(method)>;
        static const std::set<method_type> s_supported_methods {
            static_cast<method_type>(SocksV5::Method::NoAuth), static_cast<method_type>(SocksV5::Method::UserPassWd)
        };
        auto iter = s_supported_methods.find(static_cast<method_type>(method));
        if (iter == s_supported_methods.end()) {
            ec = Fundamental::make_error_code(std::errc::operation_not_supported, std::system_category(),
                                              "unsupported method");
        }
    }
    Fundamental::Signal<void(const std::string& user, const std::string& passwd, Fundamental::error_code& ec)>
        user_verify_handler;
    Fundamental::Signal<void(SocksV5::Method method, Fundamental::error_code& ec)> method_verify_handler;
    std::size_t timeout_check_sec_interval = 5000;
};
} // namespace SocksV5