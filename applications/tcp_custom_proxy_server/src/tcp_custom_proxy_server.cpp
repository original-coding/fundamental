

#include "rpc/rpc_server.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "fundamental/application/application.hpp"
#include "fundamental/basic/arg_parser.hpp"
#include "fundamental/basic/log.h"

using namespace network;
using namespace rpc_service;

int main(int argc, char* argv[]) {
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::debug;
    options.logFormat    = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    Fundamental::Logger::Initialize(std::move(options));
    Fundamental::arg_parser arg_parser { argc, argv, "1.0.1" };
    std::size_t threads = 8;
    std::size_t port    = 32000;
    std::string proxy_host;
    std::string proxy_port;
    bool support_pipe_proxy = false;
    arg_parser.AddOption("threads", Fundamental::StringFormat("handler's thread nums default:{}", threads), 't',
                         Fundamental::arg_parser::param_type::required_param, "number");
    arg_parser.AddOption("port", Fundamental::StringFormat("proxyserver's listening port default:{}", port), 'p',
                         Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("proxy_host", Fundamental::StringFormat("proxyserver's transparent proxy host"), -1,
                         Fundamental::arg_parser::param_type::required_param, "ip or domain name");
    arg_parser.AddOption("proxy_port", Fundamental::StringFormat("proxyserver's transparent proxy port", port), -1,
                         Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("pipe", Fundamental::StringFormat("pipe proxy flag default:{}", support_pipe_proxy), 'P',
                         Fundamental::arg_parser::param_type::with_none_param);
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
    threads            = arg_parser.GetValue("threads", threads);
    port               = arg_parser.GetValue("port", port);
    proxy_host         = arg_parser.GetValue("proxy_host", proxy_host);
    proxy_port         = arg_parser.GetValue("proxy_port", proxy_port);
    support_pipe_proxy = arg_parser.HasParam("pipe");
    auto s_server      = network::make_guard<rpc_server>(static_cast<std::uint16_t>(port));
    auto p             = s_server.get();
    auto& server       = *s_server.get();
    network::rpc_server_external_config config;
    if (!proxy_host.empty() && !proxy_port.empty()) {
        FINFO("transparent proxy to {} {}", proxy_host, proxy_port);
        config.enable_transparent_proxy = true;
        config.transparent_proxy_host   = proxy_host;
        config.transparent_proxy_port   = proxy_port;
        config.rpc_protocal_mask        = network::rpc_protocal_enable_mask::rpc_protocal_filter_none;
    }
    config.rpc_protocal_mask |= network::rpc_protocal_enable_mask::rpc_protocal_filter_socks5;
    network::proxy::ProxyManager proxy;
    if (support_pipe_proxy) {
        config.rpc_protocal_mask |= network::rpc_protocal_enable_mask::rpc_protocal_filter_pipe_connection;
        FINFO("enable pipe proxy");
        server.enable_data_proxy(&proxy);
    }
    server.set_external_config(config);
    server.enable_socks5_proxy(SocksV5::Sock5Handler::make_default_handler());
    network::init_io_context_pool(threads);
    server.start();
    Fundamental::Application::Instance().Loop();
    Fundamental::Application::Instance().Exit();
    return 0;
}