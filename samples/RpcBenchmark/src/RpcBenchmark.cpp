

#include "fundamental/basic/allocator.hpp"
#include "fundamental/basic/filesystem_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"

#include "rpc/rpc_client.hpp"

#include <chrono>
#include <iostream>
#include <list>
#include <memory>
#include <string>

#include "benchmark/benchmark.h"
#include "rpc/proxy/protocal_pipe/pipe_connection_upgrade_session.hpp"
#include "test_server.h"
using namespace network;
using network::rpc_service::rpc_client;

decltype(auto) gen_pipe_proxy() {
    forward::forward_request_context forward_request;
    forward_request.dst_host      = "127.0.0.1";
    auto ws_dst_port              = ::getenv("ws_dst_port");
    forward_request.dst_service   = ws_dst_port ? ws_dst_port : "9000";
    forward_request.route_path    = "/ws_proxy";
    forward_request.ssl_option    = forward::forward_optional_option;
    forward_request.socks5_option = forward::forward_disable_option;
    return proxy::pipe_connection_upgrade::make_shared(forward_request);
}
template <std::size_t blockSize>
static void TestNormal(benchmark::State& state) {
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    client->connect();
    std::string msg(blockSize, 'a');
    if constexpr (blockSize < 1024 * 1024 * 32) {
        auto msg_recv = client->call<20000, std::string>("echo", msg);
        FASSERT(msg_recv == msg);
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(client->call<20000, std::string>("echo", msg));
    }
}

template <std::size_t blockSize>
static void TestProxy(benchmark::State& state) {
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    client->set_proxy(gen_pipe_proxy());
    client->connect();
    std::string msg(blockSize, 'a');
    if constexpr (blockSize < 1024 * 1024 * 32) {
        auto msg_recv = client->call<20000, std::string>("echo", msg);
        FASSERT(msg_recv == msg);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(client->call<20000, std::string>("echo", msg));
    }
}
#ifndef NETWORK_DISABLE_SSL
template <std::size_t blockSize>
static void TestSslProxy(benchmark::State& state) {
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    client->enable_ssl(network_client_ssl_config { "local.crt", "local.key", "ca_root.crt" });
    client->set_proxy(gen_pipe_proxy());
    client->connect();
    std::string msg(blockSize, 'a');
    if constexpr (blockSize < 1024 * 1024 * 32) {
        auto msg_recv = client->call<20000, std::string>("echo", msg);
        FASSERT(msg_recv == msg);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(client->call<30000, std::string>("echo", msg));
    }
}
template <std::size_t blockSize>
static void TestSslProxyStream(benchmark::State& state) {
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    client->enable_ssl(network_client_ssl_config { "local.crt", "local.key", "ca_root.crt" });
    client->set_proxy(gen_pipe_proxy());
    client->connect();
    std::string msg(blockSize, 'a');
    std::string recv;
    auto stream = client->upgrade_to_stream("echos");
    if constexpr (blockSize < 1024 * 1024 * 32) {
        stream->Write(msg);
        stream->Read(recv, 0);
        FASSERT(msg == recv);
    }

    for (auto _ : state) {
        stream->Write(msg);
        stream->Read(recv, 0);
    }
    stream->WriteDone();
    stream->Finish();
}
#endif
BENCHMARK_TEMPLATE(TestNormal, 0);
BENCHMARK_TEMPLATE(TestNormal, 1024);
BENCHMARK_TEMPLATE(TestNormal, 4096);
BENCHMARK_TEMPLATE(TestNormal, 8192);
BENCHMARK_TEMPLATE(TestNormal, 1024 * 1024);
BENCHMARK_TEMPLATE(TestNormal, 1024 * 1024 * 32);
BENCHMARK_TEMPLATE(TestNormal, 1024 * 1024 * 128);
BENCHMARK_TEMPLATE(TestNormal, 1024 * 1024 * 1024);

BENCHMARK_TEMPLATE(TestProxy, 0);
BENCHMARK_TEMPLATE(TestProxy, 1024);
BENCHMARK_TEMPLATE(TestProxy, 4096);
BENCHMARK_TEMPLATE(TestProxy, 8192);
BENCHMARK_TEMPLATE(TestProxy, 1024 * 1024);
BENCHMARK_TEMPLATE(TestProxy, 1024 * 1024 * 32);
BENCHMARK_TEMPLATE(TestProxy, 1024 * 1024 * 128);
BENCHMARK_TEMPLATE(TestProxy, 1024 * 1024 * 1024);
#ifndef NETWORK_DISABLE_SSL
BENCHMARK_TEMPLATE(TestSslProxy, 1);
BENCHMARK_TEMPLATE(TestSslProxy, 1024);
BENCHMARK_TEMPLATE(TestSslProxy, 4096);
BENCHMARK_TEMPLATE(TestSslProxy, 8192);
BENCHMARK_TEMPLATE(TestSslProxy, 1024 * 1024);
BENCHMARK_TEMPLATE(TestSslProxy, 1024 * 1024 * 32);
BENCHMARK_TEMPLATE(TestSslProxy, 1024 * 1024 * 128);
BENCHMARK_TEMPLATE(TestSslProxy, 1024 * 1024 * 1024);

BENCHMARK_TEMPLATE(TestSslProxyStream, 1);
BENCHMARK_TEMPLATE(TestSslProxyStream, 1024);
BENCHMARK_TEMPLATE(TestSslProxyStream, 4096);
BENCHMARK_TEMPLATE(TestSslProxyStream, 8192);
BENCHMARK_TEMPLATE(TestSslProxyStream, 1024 * 1024);
BENCHMARK_TEMPLATE(TestSslProxyStream, 1024 * 1024 * 32);
BENCHMARK_TEMPLATE(TestSslProxyStream, 1024 * 1024 * 128);
BENCHMARK_TEMPLATE(TestSslProxyStream, 1024 * 1024 * 1024);
#endif
int main(int argc, char* argv[]) {
    int mode = 0;
    if (argc > 1) mode = std::stoi(argv[1]);
    Fundamental::fs::SwitchToProgramDir(argv[0]);
    argc = 0;
    argv = nullptr;
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::info;

    Fundamental::Logger::Initialize(std::move(options));
    if (mode == 0) {
        char arg0_default[] = "benchmark";
        char* args_default  = arg0_default;
        if (!argv) {
            argc = 1;
            argv = &args_default;
        }
        ::benchmark::Initialize(&argc, argv);
        if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
        run_server();
        ::benchmark::RunSpecifiedBenchmarks();
        ::benchmark::Shutdown();
        exit_server();
    } else if (mode == 1) {
        std::promise<void> sync_p;
        server_task(sync_p);
        sync_p.get_future().get();
        exit_server();
    } else {
        char arg0_default[] = "benchmark";
        char* args_default  = arg0_default;
        if (!argv) {
            argc = 1;
            argv = &args_default;
        }
        network::io_context_pool::s_excutorNums = 10;
        network::io_context_pool::Instance().start();
        ::benchmark::Initialize(&argc, argv);
        if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
        ::benchmark::RunSpecifiedBenchmarks();
        ::benchmark::Shutdown();
    }
    return 0;
}