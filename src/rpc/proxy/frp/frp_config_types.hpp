#pragma once

#include "frp_command.hpp"
#include "fundamental/basic/filesystem_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/rttr_handler/deserializer.h"
#include "fundamental/rttr_handler/serializer.h"
#include "network/network.hpp"

#include <rttr/registration>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace network::proxy
{

void __register_frp_config_reflect_type__();

struct frp_tls_client_config_file {
    std::string certificate_path;
    std::string private_key_path;
    std::string ca_certificate_path;
    bool disable_ssl                      = true;
    virtual ~frp_tls_client_config_file() = default;
    RTTR_ENABLE()
};

struct frp_tls_server_config_file {
    std::string certificate_path;
    std::string private_key_path;
    std::string tmp_dh_path;
    std::string ca_certificate_path;
    bool verify_client                    = false;
    bool disable_ssl                      = true;
    virtual ~frp_tls_server_config_file() = default;
    RTTR_ENABLE()
};

struct frp_provider_service_config {
    std::string service_name;
    std::string target_host;
    std::uint16_t target_port              = 0;
    std::uint8_t service_type              = frp_service_tcp;
    bool enable_p2p                        = true;
    virtual ~frp_provider_service_config() = default;
    RTTR_ENABLE()
};

struct frp_accessor_listener_config {
    std::string service_name;
    std::string listen_host;
    std::uint16_t listen_port               = 0;
    std::uint8_t service_type               = frp_service_tcp;
    bool enable_p2p                         = true;
    virtual ~frp_accessor_listener_config() = default;
    RTTR_ENABLE()
};

struct frp_public_server_config {
    std::size_t threads           = 8;
    std::uint16_t listen_tcp_port = 32000;
    std::uint16_t listen_udp_port = 0;
    std::string traffic_secret;
    std::vector<std::string> allowed_register_keys;
    std::uint32_t data_channel_idle_timeout_seconds = 600; // 空闲检测默认 10min（链路检测由 KCP keepalive 承担）
    frp_tls_server_config_file ssl;
    virtual ~frp_public_server_config() = default;
    RTTR_ENABLE()
};

struct frp_proxy_client_group_config {
    std::string register_key;
    std::vector<frp_provider_service_config> services;
    std::vector<frp_accessor_listener_config> listeners;
    virtual ~frp_proxy_client_group_config() = default;
    RTTR_ENABLE()
};

struct frp_proxy_client_config {
    std::size_t threads                  = 8;
    std::string public_server_host       = "127.0.0.1";
    std::uint16_t public_server_tcp_port = 32000;
    std::uint16_t public_server_udp_port = 0;
    std::string traffic_secret;
    std::uint8_t nat_type = frp_nat_type_disabled;
    std::string local_ip;
    std::uint32_t data_channel_idle_timeout_seconds = 600; // 空闲检测默认 10min（链路检测由 KCP keepalive 承担）
    frp_tls_client_config_file ssl;
    std::vector<frp_proxy_client_group_config> groups;
    virtual ~frp_proxy_client_config() = default;
    RTTR_ENABLE()
};

inline network_client_ssl_config to_network_config(const frp_tls_client_config_file& config) {
    network_client_ssl_config ret;
    ret.certificate_path    = config.certificate_path;
    ret.private_key_path    = config.private_key_path;
    ret.ca_certificate_path = config.ca_certificate_path;
    ret.disable_ssl         = config.disable_ssl;
    return ret;
}

inline network_server_ssl_config to_network_config(const frp_tls_server_config_file& config) {
    network_server_ssl_config ret;
    ret.certificate_path    = config.certificate_path;
    ret.private_key_path    = config.private_key_path;
    ret.tmp_dh_path         = config.tmp_dh_path;
    ret.ca_certificate_path = config.ca_certificate_path;
    ret.verify_client       = config.verify_client;
    ret.disable_ssl         = config.disable_ssl;
    return ret;
}

template <typename ConfigType>
inline bool load_frp_config_file(const std::string& path, ConfigType& output, std::string& error_message) {
    __register_frp_config_reflect_type__();
    std::string raw;
    if (path.empty()) {
        error_message = "config path is empty";
        return false;
    }
    if (!Fundamental::fs::ReadFile(path, raw)) {
        error_message = Fundamental::StringFormat("failed to read config file:{}", path);
        return false;
    }
    if (!Fundamental::io::from_json(raw, output)) {
        error_message = Fundamental::StringFormat("failed to parse config file as json:{}", path);
        return false;
    }
    return true;
}

template <typename ConfigType>
inline std::string dump_frp_config_example_json(const ConfigType& config) {
    __register_frp_config_reflect_type__();
    return Fundamental::io::to_json(config);
}

frp_public_server_config make_example_public_server_config();

frp_proxy_client_config make_example_proxy_client_config();

bool validate_config(const frp_public_server_config& config, std::string& error_message);

bool validate_config(const frp_proxy_client_config& config, std::string& error_message);

} // namespace network::proxy
