
#include "fundamental/basic/filesystem_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/md5_utils.hpp"
#include "test_server.h"
#include <iostream>
#include <optional>

int main(int argc, char* argv[]) {
    std::string root_path;
    if (argc > 1) root_path = argv[1];
    std::uint16_t port = 9000;
    if (argc > 2) {
        port = static_cast<std::uint16_t>(std::stoul(argv[2]));
    }
    FINFO("run with {} <path>:{} <port>:{}",argv[0],root_path,port);
    Fundamental::fs::SwitchToProgramDir(argv[0]);
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::debug;
    options.logFormat    = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    Fundamental::Logger::Initialize(std::move(options));
    run_server(root_path, port);
    exit_server();
    return 0;
}