#include "frp_config_types.hpp"

#include <rttr/registration>

namespace network::proxy
{

frp_public_server_config make_example_public_server_config() {
    frp_public_server_config config;
    config.listen_tcp_port       = 32000;
    config.listen_udp_port       = 32001;
    config.traffic_secret        = "traffic-secret-demo";
    config.allowed_register_keys = { "demo-register-key" };
    config.ssl.disable_ssl       = true;
    return config;
}

frp_proxy_client_config make_example_proxy_client_config() {
    frp_proxy_client_config config;
    config.public_server_host     = "127.0.0.1";
    config.public_server_tcp_port = 32000;
    config.public_server_udp_port = 32001;
    config.traffic_secret         = "traffic-secret-demo";
    config.nat_type               = frp_nat_type_cone;
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

void __register_frp_config_reflect_type__() {
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
