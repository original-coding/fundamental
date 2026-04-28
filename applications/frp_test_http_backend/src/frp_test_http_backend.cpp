#include "http/http_server.hpp"

#include "fundamental/application/application.hpp"
#include "fundamental/basic/arg_parser.hpp"
#include "fundamental/basic/log.h"

using namespace network;
using namespace network::http;

int main(int argc, char* argv[]) {
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::debug;
    options.logFormat    = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    Fundamental::Logger::Initialize(std::move(options));

    Fundamental::arg_parser arg_parser { argc, argv, "1.0.0" };
    std::size_t threads = 2;
    std::size_t port    = 18080;
    std::string root_path = ".";
    arg_parser.AddOption("threads", Fundamental::StringFormat("handler's thread nums default:{}", threads), 't',
                         Fundamental::arg_parser::param_type::required_param, "number");
    arg_parser.AddOption("port", Fundamental::StringFormat("http backend port default:{}", port), 'p',
                         Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("root", Fundamental::StringFormat("http backend root path default:{}", root_path), 'r',
                         Fundamental::arg_parser::param_type::required_param, "path");

    if (argc == 1) {
        arg_parser.ShowHelp();
        return 1;
    }
    if (!arg_parser.ParseCommandLine() || arg_parser.HasParam()) {
        arg_parser.ShowHelp();
        return 1;
    }
    if (arg_parser.HasParam(arg_parser.kVersionOptionName)) {
        arg_parser.ShowVersion();
        return 0;
    }

    threads   = arg_parser.GetValue("threads", threads);
    port      = arg_parser.GetValue("port", port);
    root_path = arg_parser.GetValue("root", root_path);

    http_server_config config;
    config.port      = static_cast<std::uint16_t>(port);
    config.root_path = root_path;

    network::init_io_context_pool(threads);
    auto server = network::make_guard<http_server>(config);
    server->enable_default_handler();
    server->start();
    Fundamental::Application::Instance().Loop();
    Fundamental::Application::Instance().Exit();
    return 0;
}
