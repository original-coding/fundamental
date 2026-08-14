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
    using rttr::metadata;

    rttr::registration::class_<frp_tls_client_config_file>("network::proxy::frp_tls_client_config_file")
        .constructor()(rttr::policy::ctor::as_object)
        .property("certificate_path", &frp_tls_client_config_file::certificate_path)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//TLS 证书路径"))
        .property("private_key_path", &frp_tls_client_config_file::private_key_path)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//TLS 私钥路径"))
        .property("ca_certificate_path", &frp_tls_client_config_file::ca_certificate_path)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//TLS CA 证书路径"))
        .property("disable_ssl", &frp_tls_client_config_file::disable_ssl)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//是否禁用 TLS 默认值:true"));

    rttr::registration::class_<frp_tls_server_config_file>("network::proxy::frp_tls_server_config_file")
        .constructor()(rttr::policy::ctor::as_object)
        .property("certificate_path", &frp_tls_server_config_file::certificate_path)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//TLS 证书路径"))
        .property("private_key_path", &frp_tls_server_config_file::private_key_path)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//TLS 私钥路径"))
        .property("tmp_dh_path", &frp_tls_server_config_file::tmp_dh_path)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//TLS 临时 DH 参数路径"))
        .property("ca_certificate_path", &frp_tls_server_config_file::ca_certificate_path)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//TLS CA 证书路径"))
        .property("verify_client", &frp_tls_server_config_file::verify_client)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//是否验证客户端证书 默认值:false"))
        .property("disable_ssl", &frp_tls_server_config_file::disable_ssl)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//是否禁用 TLS 默认值:true"));

    rttr::registration::class_<frp_provider_service_config>("network::proxy::frp_provider_service_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("service_name", &frp_provider_service_config::service_name)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//服务名称"))
        .property("target_host", &frp_provider_service_config::target_host)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//后端服务地址"))
        .property("target_port", &frp_provider_service_config::target_port)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//后端服务端口"))
        .property("service_type", &frp_provider_service_config::service_type)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//服务类型 0=TCP 1=UDP"))
        .property("enable_p2p", &frp_provider_service_config::enable_p2p)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//是否允许 P2P 升级 默认值:true"));

    rttr::registration::class_<frp_accessor_listener_config>("network::proxy::frp_accessor_listener_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("service_name", &frp_accessor_listener_config::service_name)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//要访问的服务名称"))
        .property("listen_host", &frp_accessor_listener_config::listen_host)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//本地监听地址"))
        .property("listen_port", &frp_accessor_listener_config::listen_port)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//本地监听端口"))
        .property("service_type", &frp_accessor_listener_config::service_type)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//服务类型 0=TCP 1=UDP"))
        .property("enable_p2p", &frp_accessor_listener_config::enable_p2p)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//是否尝试 P2P 升级 默认值:true"));

    rttr::registration::class_<frp_public_server_config>("network::proxy::frp_public_server_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("threads", &frp_public_server_config::threads)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//工作线程数 默认值:8"))
        .property("listen_tcp_port", &frp_public_server_config::listen_tcp_port)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//TCP 监听端口"))
        .property("listen_udp_port", &frp_public_server_config::listen_udp_port)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//UDP 端口基址，0=禁用 UDP"))
        .property("traffic_secret", &frp_public_server_config::traffic_secret)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//流量加密密钥"))
        .property("allowed_register_keys", &frp_public_server_config::allowed_register_keys)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//允许注册的密钥列表"))
        .property("data_channel_idle_timeout_seconds", &frp_public_server_config::data_channel_idle_timeout_seconds)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//数据通道空闲超时秒数，0=禁用"))
        .property("log_output_path", &frp_public_server_config::log_output_path)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//日志输出目录 默认值:logs"))
        .property("log_program_name", &frp_public_server_config::log_program_name)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//日志文件名前缀 默认值:frp_proxy_server"))
        .property("log_level", &frp_public_server_config::log_level)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                     "//日志输出级别 0=trace 1=debug 2=info 3=warn 4=err 5=critical 6=off"))
        .property("enable_console_output", &frp_public_server_config::enable_console_output)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//是否输出到控制台 默认值:true"))
        .property("ssl", &frp_public_server_config::ssl)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//TLS 配置"));

    rttr::registration::class_<frp_proxy_client_group_config>("network::proxy::frp_proxy_client_group_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("register_key", &frp_proxy_client_group_config::register_key)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//注册密钥"))
        .property("services", &frp_proxy_client_group_config::services)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//提供的服务列表"))
        .property("listeners", &frp_proxy_client_group_config::listeners)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//监听的访问入口列表"));

    rttr::registration::class_<frp_proxy_client_config>("network::proxy::frp_proxy_client_config")
        .constructor()(rttr::policy::ctor::as_object)
        .property("threads", &frp_proxy_client_config::threads)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//工作线程数 默认值:8"))
        .property("public_server_host", &frp_proxy_client_config::public_server_host)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//公网服务器地址"))
        .property("public_server_tcp_port", &frp_proxy_client_config::public_server_tcp_port)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//公网服务器 TCP 端口"))
        .property("public_server_udp_port", &frp_proxy_client_config::public_server_udp_port)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//公网服务器 UDP 端口，0=禁用 UDP"))
        .property("traffic_secret", &frp_proxy_client_config::traffic_secret)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//流量加密密钥"))
        .property("nat_type", &frp_proxy_client_config::nat_type)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                     "//NAT 类型 0=禁用 P2P 1=Symmetric 2=Cone"))
        .property("local_ip", &frp_proxy_client_config::local_ip)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//本机对外上报 IP，可留空"))
        .property("data_channel_idle_timeout_seconds", &frp_proxy_client_config::data_channel_idle_timeout_seconds)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//数据通道空闲超时秒数，0=禁用"))
        .property("log_output_path", &frp_proxy_client_config::log_output_path)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//日志输出目录 默认值:logs"))
        .property("log_program_name", &frp_proxy_client_config::log_program_name)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//日志文件名前缀 默认值:frp_proxy_client"))
        .property("log_level", &frp_proxy_client_config::log_level)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(),
                     "//日志输出级别 0=trace 1=debug 2=info 3=warn 4=err 5=critical 6=off"))
        .property("enable_console_output", &frp_proxy_client_config::enable_console_output)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//是否输出到控制台 默认值:true"))
        .property("ssl", &frp_proxy_client_config::ssl)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//TLS 配置"))
        .property("groups", &frp_proxy_client_config::groups)(
            metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey(), "//服务组列表"));
}

} // namespace network::proxy
