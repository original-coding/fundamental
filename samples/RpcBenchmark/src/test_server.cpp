#include "test_server.h"

#include "rpc/rpc_server.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <rttr/registration>
#include <thread>

#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"

#include "fundamental/application/application.hpp"

using namespace network;
using namespace rpc_service;
static auto& rpc_stream_pool = Fundamental::ThreadPool::Instance<100>();

std::string echo(rpc_conn conn, std::string src) {
    if (src.size() > 4096) {
        auto sp = conn.lock();
        sp->set_delay(true);
        auto req_id = sp->request_id(); // note: you need keep the request id at that
        rpc_stream_pool.Enqueue([src = std::move(src), conn, req_id]() -> void {
            auto sp = conn.lock();
            if (sp) sp->response(req_id, network::rpc_service::request_type::rpc_res, std::move(src));
        });
        return "";
    }
    return src;
}

void echos(rpc_conn conn) {
    auto c = conn.lock();
    auto w = c->InitRpcStream();

    rpc_stream_pool.Enqueue(
        [](decltype(w) stream) {
            std::string msg;
            while (stream->Read(msg, 0)) {
                if (!stream->Write(msg)) break;
            };
            stream->WriteDone();
            stream->Finish(0);
        },
        w);
}

std::unique_ptr<std::thread> s_thread;
static network::proxy::ProxyManager s_manager;

void server_task(std::promise<void>& sync_p) {
    auto s_server                                                   = rpc_server::make_shared(9000);
    s_server->enable_ssl({ nullptr, "local.crt", "local.key", "dh2048.pem", "ca_root.crt" });
    s_server->register_handler("echo", echo);
    s_server->register_handler("echos", echos);
    network::init_io_context_pool(10);
    network::rpc_server_external_config external_config;
    external_config.forward_config.ssl_config.ca_certificate_path = "ca_root.crt";
    external_config.forward_config.ssl_config.private_key_path    = "local.key";
    external_config.forward_config.ssl_config.certificate_path    = "local.crt";
    external_config.forward_config.ssl_config.disable_ssl         = false;
    external_config.forward_config.socks5_proxy_host              = "127.0.0.1";
    external_config.forward_config.socks5_proxy_port              = "9000";
    external_config.forward_config.socks5_username                = "fongwell";
    external_config.forward_config.socks5_passwd                  = "fongwell123456";
    external_config.enable_transparent_proxy                      = true;
    external_config.transparent_proxy_host                        = "127.0.0.1";
    external_config.transparent_proxy_port                        = "9000";
    s_server->set_external_config(external_config);
    {
        using namespace network::proxy;
        auto& manager = s_manager;
        manager.AddWsProxyRoute("/ws_proxy", proxy::ProxyHost { "127.0.0.1", "9000" });
    }

    s_server->enable_data_proxy(&s_manager);
    s_server->start();
    rpc_stream_pool.Spawn(5);
    sync_p.set_value();
    Fundamental::Application::Instance().Loop();
}

void run_server() {
    std::promise<void> sync_p;
    s_thread = std::make_unique<std::thread>([&]() { server_task(sync_p); });
    sync_p.get_future().wait();
}

void exit_server() {
    Fundamental::Application::Instance().Exit();
    if (s_thread) s_thread->join();
}