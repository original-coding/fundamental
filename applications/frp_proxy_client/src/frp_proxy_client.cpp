

#include "rpc/proxy/frp/frp_client.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "fundamental/application/application.hpp"
#include "fundamental/basic/arg_parser.hpp"
#include "fundamental/basic/log.h"

using namespace network;
using namespace network::proxy;

int main(int argc, char* argv[]) {

    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::debug;
    options.logFormat    = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    Fundamental::Logger::Initialize(std::move(options));
    Fundamental::arg_parser arg_parser { argc, argv, "1.0.1" };
    std::size_t threads     = 8;
    std::string server_ip   = "127.0.0.1";
    std::size_t server_port = 32000;
    std::size_t access_port = 32001;

    std::string proxy_host;
    std::size_t proxy_port;
    arg_parser.AddOption("threads", Fundamental::StringFormat("handler's thread nums default:{}", threads), 't',
                         Fundamental::arg_parser::param_type::required_param, "number");
    arg_parser.AddOption("server_port", Fundamental::StringFormat("signaling server's port default:{}", server_port),
                         'p', Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("server_ip", Fundamental::StringFormat("signaling server's ip default:{}", server_ip), 'i',
                         Fundamental::arg_parser::param_type::required_param, "domain info/ip");
    arg_parser.AddOption("access_port", Fundamental::StringFormat("access port after proxy default:{}", access_port),
                         -1, Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("proxy_port", Fundamental::StringFormat("local proxied server's port"), -1,
                         Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("proxy_host", Fundamental::StringFormat("local proxied server's ip"), -1,
                         Fundamental::arg_parser::param_type::required_param, "domain info/ip");
    arg_parser.AddOption("pem", Fundamental::StringFormat("certificate_path"), -1,
                         Fundamental::arg_parser::param_type::required_param, "path");
    arg_parser.AddOption("key", Fundamental::StringFormat("private_key_path"), -1,
                         Fundamental::arg_parser::param_type::required_param, "path");
    arg_parser.AddOption("ca", Fundamental::StringFormat("trust pems path"), -1,
                         Fundamental::arg_parser::param_type::required_param, "path");

    if (argc == 1) {
        arg_parser.ShowHelp();
        return 1;
    }
    if (!arg_parser.ParseCommandLine()) {
        arg_parser.ShowHelp();
        return 1;
    }
    if (arg_parser.HasParam()) {
        arg_parser.ShowHelp();
        return 1;
    }
    if (arg_parser.HasParam(arg_parser.kVersionOptionName)) {
        arg_parser.ShowVersion();
        return 1;
    }
    network_client_ssl_config ssl_config;
    threads                        = arg_parser.GetValue("threads", threads);
    server_port                    = arg_parser.GetValue("server_port", server_port);
    server_ip                      = arg_parser.GetValue("server_ip", server_ip);
    access_port                    = arg_parser.GetValue("access_port", access_port);
    proxy_host                     = arg_parser.GetValue("proxy_host", proxy_host);
    proxy_port                     = arg_parser.GetValue("proxy_port", proxy_port);
    ssl_config.ca_certificate_path = arg_parser.GetValue("ca", ssl_config.ca_certificate_path);
    ssl_config.certificate_path    = arg_parser.GetValue("pem", ssl_config.certificate_path);
    ssl_config.private_key_path    = arg_parser.GetValue("key", ssl_config.private_key_path);
    if ((!ssl_config.private_key_path.empty() && !ssl_config.certificate_path.empty()) ||
        !ssl_config.ca_certificate_path.empty()) {
        ssl_config.disable_ssl = false;
    }
    __register_frp_reflect_type__();
    auto s_client =
        network::make_guard<frp_client>(server_ip, std::to_string(server_port), static_cast<std::uint16_t>(access_port),
                                        proxy_host, std::to_string(proxy_port));
    auto p              = s_client.get();
    auto& actual_client = *s_client.get();
    s_client->enable_ssl(ssl_config);
    network::init_io_context_pool(threads);
    actual_client.start();
    Fundamental::Application::Instance().Loop();
    Fundamental::Application::Instance().Exit();
    return 0;
}