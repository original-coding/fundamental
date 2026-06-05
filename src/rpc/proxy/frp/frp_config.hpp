#pragma once

#include "frp_runtime_command.hpp"
#include "fundamental/basic/filesystem_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/rttr_handler/deserializer.h"
#include "fundamental/rttr_handler/serializer.h"
#include "network/network.hpp"

#include <rttr/registration>
#include <set>
#include <unordered_set>

namespace network::proxy
{

inline void __register_frp_config_reflect_type__();

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
    std::uint8_t service_type              = frp_runtime_service_tcp;
    bool enable_p2p                        = true;
    virtual ~frp_provider_service_config() = default;
    RTTR_ENABLE()
};

struct frp_accessor_listener_config {
    std::string service_name;
    std::string listen_host;
    std::uint16_t listen_port               = 0;
    std::uint8_t service_type               = frp_runtime_service_tcp;
    bool enable_p2p                         = true;
    virtual ~frp_accessor_listener_config() = default;
    RTTR_ENABLE()
};

struct frp_public_server_config {
    std::size_t threads           = 8;
    std::uint16_t listen_tcp_port = 32000;
    std::uint16_t listen_udp_port = 0; // base UDP port; server binds port and port+1; 0 = relay only
    std::string traffic_secret;
    std::vector<std::string> allowed_register_keys;
    std::uint32_t data_channel_idle_timeout_seconds = 120; // 0 = disabled
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
    std::uint8_t nat_type = frp_runtime_nat_type_disabled;
    std::string local_ip;
    std::uint32_t data_channel_idle_timeout_seconds = 120;
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

inline frp_public_server_config make_example_public_server_config() {
    frp_public_server_config config;
    config.listen_tcp_port       = 32000;
    config.listen_udp_port       = 32001;
    config.traffic_secret        = "traffic-secret-demo";
    config.allowed_register_keys = { "demo-register-key" };
    config.ssl.disable_ssl       = true;
    return config;
}

inline bool validate_config(const frp_public_server_config& config, std::string& error_message) {
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

inline frp_proxy_client_config make_example_proxy_client_config() {
    frp_proxy_client_config config;
    config.public_server_host     = "127.0.0.1";
    config.public_server_tcp_port = 32000;
    config.public_server_udp_port = 32001;
    config.traffic_secret         = "traffic-secret-demo";
    config.nat_type               = frp_runtime_nat_type_cone;
    config.local_ip               = "192.168.1.100";
    config.ssl.disable_ssl        = true;

    frp_proxy_client_group_config g1;
    g1.register_key = "demo-key-1";
    frp_provider_service_config svc;
    svc.service_name = "echo-tcp";
    svc.target_host  = "127.0.0.1";
    svc.target_port  = 18080;
    g1.services.push_back(std::move(svc));
    frp_accessor_listener_config lsn;
    lsn.service_name = "rdp";
    lsn.listen_host  = "0.0.0.0";
    lsn.listen_port  = 19001;
    g1.listeners.push_back(std::move(lsn));
    config.groups.push_back(std::move(g1));

    frp_proxy_client_group_config g2;
    g2.register_key = "demo-key-2";
    frp_accessor_listener_config lsn2;
    lsn2.service_name = "echo-tcp";
    lsn2.listen_host  = "0.0.0.0";
    lsn2.listen_port  = 19002;
    g2.listeners.push_back(std::move(lsn2));
    config.groups.push_back(std::move(g2));

    return config;
}

inline bool validate_config(const frp_proxy_client_config& config, std::string& error_message) {
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

inline void __register_frp_config_reflect_type__() {
    static bool has_registered = false;
    if (has_registered) return;
    has_registered = true;

    rttr::registration::class_<frp_tls_client_config_file>("network::proxy::frp_tls_client_config_file")
        .constructor()(rttr::policy::ctor::as_object)
        .property("certificate_path", &frp_tls_client_config_file::certificate_path)
        .property("private_key_path", &frp_tls_client_config_file::private_key_path)
        .property("ca_certificate_path", &frp_tls_client_config_file::ca_certificate_path)
        .property("disable_ssl", &frp_tls_client_config_file::disable_ssl);

    rttr::registration::class_<frp_tls_server_config_file>("network::proxy::frp_tls_server_config_file")
        .constructor()(rttr::policy::ctor::as_object)
        .property("certificate_path", &frp_tls_server_config_file::certificate_path)
        .property("private_key_path", &frp_tls_server_config_file::private_key_path)
        .property("tmp_dh_path", &frp_tls_server_config_file::tmp_dh_path)
        .property("ca_certificate_path", &frp_tls_server_config_file::ca_certificate_path)
        .property("verify_client", &frp_tls_server_config_file::verify_client)
        .property("disable_ssl", &frp_tls_server_config_file::disable_ssl);

    rttr::registration::class_<frp_provider_service_config>("network::proxy::frp_provider_service_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("service_name", &frp_provider_service_config::service_name)
        .property("target_host", &frp_provider_service_config::target_host)
        .property("target_port", &frp_provider_service_config::target_port)
        .property("service_type", &frp_provider_service_config::service_type)
        .property("enable_p2p", &frp_provider_service_config::enable_p2p);

    rttr::registration::class_<frp_accessor_listener_config>("network::proxy::frp_accessor_listener_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("service_name", &frp_accessor_listener_config::service_name)
        .property("listen_host", &frp_accessor_listener_config::listen_host)
        .property("listen_port", &frp_accessor_listener_config::listen_port)
        .property("service_type", &frp_accessor_listener_config::service_type)
        .property("enable_p2p", &frp_accessor_listener_config::enable_p2p);

    rttr::registration::class_<frp_public_server_config>("network::proxy::frp_public_server_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("threads", &frp_public_server_config::threads)
        .property("listen_tcp_port", &frp_public_server_config::listen_tcp_port)
        .property("listen_udp_port", &frp_public_server_config::listen_udp_port)
        .property("traffic_secret", &frp_public_server_config::traffic_secret)
        .property("allowed_register_keys", &frp_public_server_config::allowed_register_keys)
        .property("data_channel_idle_timeout_seconds", &frp_public_server_config::data_channel_idle_timeout_seconds)
        .property("ssl", &frp_public_server_config::ssl);

    rttr::registration::class_<frp_proxy_client_group_config>("network::proxy::frp_proxy_client_group_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("register_key", &frp_proxy_client_group_config::register_key)
        .property("services", &frp_proxy_client_group_config::services)
        .property("listeners", &frp_proxy_client_group_config::listeners);

    rttr::registration::class_<frp_proxy_client_config>("network::proxy::frp_proxy_client_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("threads", &frp_proxy_client_config::threads)
        .property("public_server_host", &frp_proxy_client_config::public_server_host)
        .property("public_server_tcp_port", &frp_proxy_client_config::public_server_tcp_port)
        .property("public_server_udp_port", &frp_proxy_client_config::public_server_udp_port)
        .property("traffic_secret", &frp_proxy_client_config::traffic_secret)
        .property("nat_type", &frp_proxy_client_config::nat_type)
        .property("local_ip", &frp_proxy_client_config::local_ip)
        .property("data_channel_idle_timeout_seconds", &frp_proxy_client_config::data_channel_idle_timeout_seconds)
        .property("ssl", &frp_proxy_client_config::ssl)
        .property("groups", &frp_proxy_client_config::groups);
}

} // namespace network::proxy
