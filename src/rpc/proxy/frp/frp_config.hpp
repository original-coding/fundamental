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
    bool disable_ssl = true;
    virtual ~frp_tls_client_config_file() = default;
    RTTR_ENABLE()
};

struct frp_tls_server_config_file {
    std::string certificate_path;
    std::string private_key_path;
    std::string tmp_dh_path;
    std::string ca_certificate_path;
    bool verify_client = false;
    bool disable_ssl   = true;
    virtual ~frp_tls_server_config_file() = default;
    RTTR_ENABLE()
};

struct frp_provider_service_config {
    std::string service_name;
    std::string target_host;
    std::uint16_t target_port = 0;
    bool enable_p2p = true;
    virtual ~frp_provider_service_config() = default;
    RTTR_ENABLE()
};

struct frp_accessor_listener_config {
    std::string service_name;
    std::string listen_host;
    std::uint16_t listen_port = 0;
    bool enable_p2p           = true;
    virtual ~frp_accessor_listener_config() = default;
    RTTR_ENABLE()
};

struct frp_public_server_config {
    std::size_t threads               = 8;
    std::uint16_t listen_tcp_port     = 32000;
    std::uint16_t listen_udp_port     = 0; // base UDP port; server binds port and port+1; 0 = relay only
    std::string traffic_secret;
    std::vector<std::string> allowed_register_keys;
    frp_tls_server_config_file ssl;
    virtual ~frp_public_server_config() = default;
    RTTR_ENABLE()
};

struct frp_provider_config {
    std::size_t threads               = 8;
    std::string public_server_host    = "127.0.0.1";
    std::uint16_t public_server_tcp_port = 32000;
    std::uint16_t public_server_udp_port = 0; // base UDP port; client probes port and port+1; 0 = no p2p
    std::string traffic_secret;
    std::string register_key;
    std::uint8_t nat_type = frp_runtime_nat_type_full;
    std::string local_ip; // LAN IP for same-network p2p; empty = skip local candidate
    frp_tls_client_config_file ssl;
    std::vector<frp_provider_service_config> services;
    virtual ~frp_provider_config() = default;
    RTTR_ENABLE()
};

struct frp_accessor_config {
    std::size_t threads               = 8;
    std::string public_server_host    = "127.0.0.1";
    std::uint16_t public_server_tcp_port = 32000;
    std::uint16_t public_server_udp_port = 0; // base UDP port; client probes port and port+1; 0 = no p2p
    std::string traffic_secret;
    std::string register_key;
    std::uint8_t nat_type = frp_runtime_nat_type_disabled;
    std::string local_ip; // LAN IP for same-network p2p; empty = skip local candidate
    frp_tls_client_config_file ssl;
    std::vector<frp_accessor_listener_config> listeners;
    virtual ~frp_accessor_config() = default;
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
    config.listen_tcp_port      = 32000;
    config.listen_udp_port      = 32001;
    config.traffic_secret       = "traffic-secret-demo";
    config.allowed_register_keys = { "demo-register-key" };
    config.ssl.disable_ssl      = true;
    return config;
}

inline frp_provider_config make_example_provider_config() {
    frp_provider_config config;
    config.public_server_host     = "127.0.0.1";
    config.public_server_tcp_port = 32000;
    config.public_server_udp_port = 32001;
    config.traffic_secret         = "traffic-secret-demo";
    config.register_key           = "demo-register-key";
    config.nat_type               = frp_runtime_nat_type_full;
    config.local_ip             = "192.168.1.100";
    config.ssl.disable_ssl        = true;
    frp_provider_service_config service;
    service.service_name = "demo-web";
    service.target_host  = "127.0.0.1";
    service.target_port  = 18080;
    config.services.push_back(std::move(service));
    return config;
}

inline frp_accessor_config make_example_accessor_config() {
    frp_accessor_config config;
    config.public_server_host     = "127.0.0.1";
    config.public_server_tcp_port = 32000;
    config.public_server_udp_port = 32001;
    config.traffic_secret         = "traffic-secret-demo";
    config.register_key           = "demo-register-key";
    config.nat_type               = frp_runtime_nat_type_full;
    config.local_ip               = "192.168.1.100";
    config.ssl.disable_ssl        = true;
    frp_accessor_listener_config listener;
    listener.service_name = "demo-web";
    listener.listen_host  = "127.0.0.1";
    listener.listen_port  = 28080;
    config.listeners.push_back(std::move(listener));
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

inline bool validate_config(const frp_provider_config& config, std::string& error_message) {
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
    if (config.register_key.empty()) {
        error_message = "register_key is required";
        return false;
    }
    if (config.services.empty()) {
        error_message = "services must not be empty";
        return false;
    }
    std::unordered_set<std::string> service_names;
    for (const auto& service : config.services) {
        if (service.service_name.empty()) {
            error_message = "service_name must not be empty";
            return false;
        }
        if (!service_names.insert(service.service_name).second) {
            error_message = Fundamental::StringFormat("duplicated service_name:{}", service.service_name);
            return false;
        }
        if (service.target_host.empty()) {
            error_message = Fundamental::StringFormat("target_host is required for service:{}", service.service_name);
            return false;
        }
        if (service.target_port == 0) {
            error_message = Fundamental::StringFormat("target_port must be non-zero for service:{}",
                                                      service.service_name);
            return false;
        }
    }
    return true;
}

inline bool validate_config(const frp_accessor_config& config, std::string& error_message) {
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
    if (config.register_key.empty()) {
        error_message = "register_key is required";
        return false;
    }
    std::set<std::pair<std::string, std::uint16_t>> listen_endpoints;
    for (const auto& listener : config.listeners) {
        if (listener.service_name.empty()) {
            error_message = "listener service_name must not be empty";
            return false;
        }
        if (listener.listen_host.empty()) {
            error_message =
                Fundamental::StringFormat("listen_host is required for service:{}", listener.service_name);
            return false;
        }
        if (listener.listen_port == 0) {
            error_message =
                Fundamental::StringFormat("listen_port must be non-zero for service:{}", listener.service_name);
            return false;
        }
        auto endpoint = std::make_pair(listener.listen_host, listener.listen_port);
        if (!listen_endpoints.insert(endpoint).second) {
            error_message = Fundamental::StringFormat("duplicated listener endpoint {}:{}",
                                                      listener.listen_host,
                                                      listener.listen_port);
            return false;
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
        .property("enable_p2p", &frp_provider_service_config::enable_p2p);

    rttr::registration::class_<frp_accessor_listener_config>("network::proxy::frp_accessor_listener_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("service_name", &frp_accessor_listener_config::service_name)
        .property("listen_host", &frp_accessor_listener_config::listen_host)
        .property("listen_port", &frp_accessor_listener_config::listen_port)
        .property("enable_p2p", &frp_accessor_listener_config::enable_p2p);

    rttr::registration::class_<frp_public_server_config>("network::proxy::frp_public_server_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("threads", &frp_public_server_config::threads)
        .property("listen_tcp_port", &frp_public_server_config::listen_tcp_port)
        .property("listen_udp_port", &frp_public_server_config::listen_udp_port)
        .property("traffic_secret", &frp_public_server_config::traffic_secret)
        .property("allowed_register_keys", &frp_public_server_config::allowed_register_keys)
        .property("ssl", &frp_public_server_config::ssl);

    rttr::registration::class_<frp_provider_config>("network::proxy::frp_provider_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("threads", &frp_provider_config::threads)
        .property("public_server_host", &frp_provider_config::public_server_host)
        .property("public_server_tcp_port", &frp_provider_config::public_server_tcp_port)
        .property("public_server_udp_port", &frp_provider_config::public_server_udp_port)
        .property("traffic_secret", &frp_provider_config::traffic_secret)
        .property("register_key", &frp_provider_config::register_key)
        .property("nat_type", &frp_provider_config::nat_type)
        .property("local_ip", &frp_provider_config::local_ip)
        .property("ssl", &frp_provider_config::ssl)
        .property("services", &frp_provider_config::services);

    rttr::registration::class_<frp_accessor_config>("network::proxy::frp_accessor_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("threads", &frp_accessor_config::threads)
        .property("public_server_host", &frp_accessor_config::public_server_host)
        .property("public_server_tcp_port", &frp_accessor_config::public_server_tcp_port)
        .property("public_server_udp_port", &frp_accessor_config::public_server_udp_port)
        .property("traffic_secret", &frp_accessor_config::traffic_secret)
        .property("register_key", &frp_accessor_config::register_key)
        .property("nat_type", &frp_accessor_config::nat_type)
        .property("local_ip", &frp_accessor_config::local_ip)
        .property("ssl", &frp_accessor_config::ssl)
        .property("listeners", &frp_accessor_config::listeners);
}

} // namespace network::proxy
