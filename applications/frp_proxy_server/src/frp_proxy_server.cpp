#include "rpc/proxy/frp/frp_config.hpp"
#include "rpc/proxy/frp/frp_runtime_command.hpp"
#include "rpc/proxy/frp/frp_runtime_server.hpp"

#include "fundamental/application/application.hpp"
#include "fundamental/basic/arg_parser.hpp"
#include "fundamental/basic/log.h"

#ifdef _MSC_VER
#include <windows.h>
#endif

using namespace network;
using namespace network::proxy;

int main(int argc, char* argv[]) {
#ifdef _MSC_VER
    SetThreadUILanguage(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
#endif
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::debug;
    options.logFormat    = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    Fundamental::Logger::Initialize(std::move(options));

    Fundamental::arg_parser arg_parser { argc, argv, "2.0.0" };
    arg_parser.AddOption("config", Fundamental::StringFormat("json config path"), 'c',
                         Fundamental::arg_parser::param_type::required_param, "path");
    arg_parser.AddOption("print-example-config", "print example json config and exit", 'p',
                         Fundamental::arg_parser::param_type::with_none_param);

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
    if (arg_parser.HasParam("print-example-config")) {
        std::cout << dump_frp_config_example_json(make_example_public_server_config()) << std::endl;
        return 0;
    }

    auto config_path = arg_parser.GetValue<std::string>("config", "");
    if (config_path.empty()) {
        FERR("public_server requires --config");
        return 1;
    }

    frp_public_server_config config;
    std::string error_message;
    __register_frp_runtime_reflect_type__();
    if (!load_frp_config_file(config_path, config, error_message) || !validate_config(config, error_message)) {
        FERR("invalid frp public server config:{} err:{}", config_path, error_message);
        return 1;
    }

    network::init_io_context_pool(config.threads);
    auto server = network::make_guard<frp_runtime_public_server>(std::move(config));
    server->start();
    Fundamental::Application::Instance().Loop();
    Fundamental::Application::Instance().Exit();
    return 0;
}
