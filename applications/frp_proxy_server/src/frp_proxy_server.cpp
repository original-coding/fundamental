

#include "rpc/proxy/frp/frp_server.hpp"

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
    std::size_t threads          = 8;
    std::size_t port             = 32000;
    std::size_t proxy_port_start = 32001;
    std::size_t proxy_port_end   = 33000;
    arg_parser.AddOption("threads", Fundamental::StringFormat("handler's thread nums default:{}", threads), 't',
                         Fundamental::arg_parser::param_type::required_param, "number");
    arg_parser.AddOption("port", Fundamental::StringFormat("listening port default:{}", port), 'p',
                         Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("proxy_port_start",
                         Fundamental::StringFormat("proxy work port start default:{}", proxy_port_start), -1,
                         Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("proxy_port_end", Fundamental::StringFormat("proxy work port end default:{}", proxy_port_end),
                         -1, Fundamental::arg_parser::param_type::required_param, "port");
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
    network_server_ssl_config ssl_config;
    threads                        = arg_parser.GetValue("threads", threads);
    port                           = arg_parser.GetValue("port", port);
    proxy_port_start               = arg_parser.GetValue("proxy_port_start", proxy_port_start);
    proxy_port_end                 = arg_parser.GetValue("proxy_port_end", proxy_port_end);
    ssl_config.ca_certificate_path = arg_parser.GetValue("ca", ssl_config.ca_certificate_path);
    ssl_config.certificate_path    = arg_parser.GetValue("pem", ssl_config.certificate_path);
    ssl_config.private_key_path    = arg_parser.GetValue("key", ssl_config.private_key_path);
    if (!ssl_config.private_key_path.empty() && !ssl_config.certificate_path.empty()) {
        ssl_config.disable_ssl = false;
    }
    if (proxy_port_start > proxy_port_end) std::swap(proxy_port_start, proxy_port_end);
    std::set<std::uint16_t> work_ports;
    for (std::uint16_t p = proxy_port_start; p <= proxy_port_end; ++p) {
        work_ports.insert(p);
    }
    __register_frp_reflect_type__();
    auto s_server = network::make_guard<frp_signaling_server>(static_cast<std::uint16_t>(port), std::move(work_ports));
    auto p        = s_server.get();
    auto& server  = *s_server.get();
    s_server->enable_ssl(ssl_config);
    network::init_io_context_pool(threads);
    server.start();
    Fundamental::Application::Instance().Loop();
    Fundamental::Application::Instance().Exit();
    return 0;
}