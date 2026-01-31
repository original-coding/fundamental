#include "test_server.h"

#include "http/http_server.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <rttr/registration>
#include <thread>

#include "fundamental/application/application.hpp"
#include "fundamental/basic/filesystem_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/random_generator.hpp"
#include "fundamental/delay_queue/delay_queue.h"
#include "fundamental/thread_pool/thread_pool.h"

using namespace network;
using namespace network::http;
void OnHttpBinary(std::shared_ptr<http_connection> conn, http_response& response, http_request& request) {
    constexpr std::string_view kData = "{\"code\":0,\"message\":\"hello http standalone\"}";
    response.set_content_type(".data");
    std::size_t max_count = 10;
    response.set_body_size(max_count * kData.size());
    std::size_t index = 0;
    while (index < max_count) {
        ++index;
        response.append_body(kData.data(), kData.size(),
                             [index]() { FINFO("OnHttpBinary finish send index:{}", index); });
    }
}

void OnUploadFile(std::shared_ptr<http_connection> conn, http_response& response, http_request& request) {

    auto& pool = Fundamental::ThreadPool::LongTimePool();
    pool.Enqueue([conn]() mutable {
        auto& request    = conn->get_request();
        auto& response   = conn->get_response();
        auto output_path = request.get_query_param("path");
        if (output_path.empty()) output_path = "default.binary";
        FINFO("upload to path:{}", output_path);
        response.set_content_type(".data");

        auto& file_data = request.get_body();
        auto ret        = Fundamental::fs::WriteFile(output_path, file_data.data(), file_data.size());
        auto response_str =
            Fundamental::StringFormat("write {} bytes to {} result:{}", file_data.size(), output_path, ret);
        response.set_body_size(response_str.size());
        response.append_body(response_str.data(), response_str.size());
        response.perform_response();
    });
}

std::unique_ptr<std::thread> s_thread;
void server_task(std::promise<void>& sync_p, const std::string& root_path, std::uint16_t port) {
    http_server_config config;
    config.port                = port;
    config.head_case_sensitive = false;
    config.root_path           = root_path;
    auto s_server              = network::make_guard<http_server>(config);
    s_server->enable_default_handler();
    network_server_ssl_config ssl_config;
    ssl_config.passwd_cb = nullptr;
    ssl_config.private_key_path = "local.key";
    ssl_config.certificate_path = "local.crt";
    ssl_config.tmp_dh_path      = "dh2048.pem";
    ssl_config.verify_client    = true;
    ssl_config.enable_no_ssl    = true;
    s_server->enable_ssl(std::move(ssl_config));
    s_server->register_handler("/binary", OnHttpBinary, http::MethodFilter::HttpAll);
    s_server->register_handler("/upload", OnUploadFile, http::MethodFilter::HttpAll);
    network::init_io_context_pool(10);

    s_server->start();

    Fundamental::Application::Instance().Loop();
    FDEBUG("finish loop");
    sync_p.set_value();
}

void run_server(const std::string& root_path, std::uint16_t port) {
    std::promise<void> sync_p;
    s_thread = std::make_unique<std::thread>([&]() { server_task(sync_p, root_path, port); });
    sync_p.get_future().get();
}

void exit_server() {
    // delay 100ms to exit to test resource manager
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    FDEBUG("exit server 2");
    Fundamental::Application::Instance().Exit();
    FDEBUG("exit server 1");
    if (s_thread) s_thread->join();
    FDEBUG("exit server");
}
