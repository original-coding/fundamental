#include "frp_config_types.hpp"

#include <string>
#include <tuple>
#include <unordered_set>

namespace network::proxy
{

bool validate_config(const frp_public_server_config& config, std::string& error_message) {
    if (config.log_level < static_cast<std::int32_t>(Fundamental::LogLevel::trace) ||
        config.log_level > static_cast<std::int32_t>(Fundamental::LogLevel::off)) {
        error_message = "log_level must be between 0(trace) and 6(off)";
        return false;
    }
    if (config.listen_tcp_port == 0) {
        error_message = "listen_tcp_port must be non-zero";
        return false;
    }
    if (config.traffic_secret.empty()) {
        error_message = "traffic_secret is required";
        return false;
    }
    std::unordered_set<std::string> register_keys;
    for (const auto& key : config.allowed_register_keys) {
        if (key.empty()) {
            error_message = "allowed_register_keys must not contain empty key";
            return false;
        }
        if (!register_keys.insert(key).second) {
            error_message = Fundamental::StringFormat("duplicated allowed_register_key:{}", key);
            return false;
        }
    }
    if (config.allowed_register_keys.empty()) {
        error_message = "allowed_register_keys must not be empty";
        return false;
    }
    return true;
}

bool validate_config(const frp_proxy_client_config& config, std::string& error_message) {
    if (config.log_level < static_cast<std::int32_t>(Fundamental::LogLevel::trace) ||
        config.log_level > static_cast<std::int32_t>(Fundamental::LogLevel::off)) {
        error_message = "log_level must be between 0(trace) and 6(off)";
        return false;
    }
    if (config.public_server_host.empty()) {
        error_message = "public_server_host is required";
        return false;
    }
    if (config.public_server_tcp_port == 0) {
        error_message = "public_server_tcp_port must be non-zero";
        return false;
    }
    if (config.traffic_secret.empty()) {
        error_message = "traffic_secret is required";
        return false;
    }
    if (config.groups.empty()) {
        error_message = "groups must not be empty";
        return false;
    }
    std::unordered_set<std::string> keys;
    for (const auto& g : config.groups) {
        if (g.register_key.empty()) {
            error_message = "register_key empty in group";
            return false;
        }
        if (!keys.insert(g.register_key).second) {
            error_message = Fundamental::StringFormat("duplicated register_key:{}", g.register_key);
            return false;
        }
        if (g.services.empty() && g.listeners.empty()) {
            error_message = Fundamental::StringFormat("group key:{} has no services or listeners", g.register_key);
            return false;
        }
        std::unordered_set<std::string> names;
        for (const auto& s : g.services) {
            if (s.service_name.empty()) {
                error_message = "service_name empty";
                return false;
            }
            if (!names.insert(s.service_name).second) {
                error_message =
                    Fundamental::StringFormat("duplicated service:{} key:{}", s.service_name, g.register_key);
                return false;
            }
            if (s.target_host.empty() || s.target_port == 0) {
                error_message = "invalid service target";
                return false;
            }
        }
        std::set<std::tuple<std::string, std::uint16_t, std::uint8_t>> eps;
        for (const auto& l : g.listeners) {
            if (l.service_name.empty() || l.listen_host.empty() || l.listen_port == 0) {
                error_message = "invalid listener";
                return false;
            }
            auto ep = std::make_tuple(l.listen_host, l.listen_port, l.service_type);
            if (!eps.insert(ep).second) {
                error_message = "duplicated listener endpoint";
                return false;
            }
        }
    }
    return true;
}

} // namespace network::proxy
