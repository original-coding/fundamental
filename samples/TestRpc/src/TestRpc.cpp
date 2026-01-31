
#include "test_server.h"

#include "fundamental/basic/filesystem_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"
#include "rpc/proxy/protocal_pipe/forward_pipe_codec.hpp"
#include "rpc/proxy/protocal_pipe/pipe_connection_upgrade_session.hpp"
#include "rpc/proxy/protocal_pipe/ws_port_pipe_server.hpp"
#include "rpc/proxy/socks5/socks5_proxy_session.hpp"
#include "rpc/proxy/websocket/ws_upgrade_session.hpp"

#include "fundamental/application/application.hpp"
#include "fundamental/basic/random_generator.hpp"
#include "rpc/proxy/proxy_manager.hpp"
#include "rpc/proxy/rpc_forward_connection.hpp"
#include "rpc/rpc_client.hpp"
#include "rpc/rpc_server.hpp"

#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>

using namespace network;
using namespace network::rpc_service;
static Fundamental::ThreadPool& s_test_pool = Fundamental::ThreadPool::Instance<101>();

decltype(auto) gen_pipe_proxy(const std::string& path = "/ws_proxy") {
    forward::forward_request_context forward_request;
    forward_request.dst_host    = "127.0.0.1";
    auto ws_dst_port            = ::getenv("ws_dst_port");
    forward_request.dst_service = ws_dst_port ? ws_dst_port : "9000";
    forward_request.route_path  = path;
    forward_request.ssl_option  = forward::forward_required_option;
    return proxy::pipe_connection_upgrade::make_shared(forward_request);
}
decltype(auto) gen_pipe_add_server_proxy(const std::string& path) {
    forward::forward_request_context forward_request;
    forward_request.dst_host         = "127.0.0.1";
    forward_request.dst_service      = "9000";
    forward_request.route_path       = path;
    forward_request.ssl_option       = forward::forward_required_option;
    forward_request.forward_protocal = forward::forward_add_server;
    return proxy::pipe_connection_upgrade::make_shared(forward_request);
}

#if 1
TEST(rpc_test, test_forward_protocal_codec) {
    {
        using context_type = network::forward::forward_request_context;
        context_type context;
        context.dst_host              = "127.0.0.1";
        context.dst_service           = "9000";
        context.route_path            = "#/test_api#";
        context.forward_protocal      = network::forward::forward_raw;
        context.ssl_option            = network::forward::forward_required_option;
        context.socks5_option         = network::forward::forward_required_option;
        auto [encode_ret, encode_str] = context.encode();
        EXPECT_TRUE(encode_ret);
        context_type parse_context;
        std::size_t i = 0;
        encode_str.push_back('a');
        for (; i < encode_str.size() - 3; i += 2) {
            auto [status, len] = parse_context.decode(encode_str.data() + i, 2);
            EXPECT_EQ(status, network::forward::forward_parse_need_more_data);
        }
        auto left_size     = encode_str.size() - i;
        auto [status, len] = parse_context.decode(encode_str.data() + i, left_size);
        EXPECT_EQ(status, network::forward::forward_parse_success);
        EXPECT_EQ(len, left_size - 1);

        EXPECT_EQ(parse_context.dst_host, context.dst_host);
        EXPECT_EQ(parse_context.dst_service, context.dst_service);
        EXPECT_EQ(parse_context.route_path, context.route_path);
        EXPECT_EQ(parse_context.ssl_option, context.ssl_option);
        EXPECT_EQ(parse_context.socks5_option, context.socks5_option);
        EXPECT_EQ(parse_context.forward_protocal, context.forward_protocal);
    }
    {
        using context_type = network::forward::forward_response_context;
        context_type context;
        context.code                  = 100;
        context.msg                   = "okkk";
        auto [encode_ret, encode_str] = context.encode();
        EXPECT_TRUE(encode_ret);
        context_type parse_context;
        std::size_t i = 0;
        encode_str.push_back('a');
        for (; i < encode_str.size() - 3; i += 2) {
            auto [status, len] = parse_context.decode(encode_str.data() + i, 2);
            EXPECT_EQ(status, network::forward::forward_parse_need_more_data);
        }
        auto left_size     = encode_str.size() - i;
        auto [status, len] = parse_context.decode(encode_str.data() + i, left_size);
        EXPECT_EQ(status, network::forward::forward_parse_success);
        EXPECT_EQ(len, left_size - 1);

        EXPECT_EQ(parse_context.code, context.code);
        EXPECT_EQ(parse_context.msg, context.msg);
    }
}
TEST(rpc_test, test_ws_forward) {
    {
        std::string ws_context =
            "GET /api111 HTTP/1.1\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
            "dGhlIHNhbXBsZSBub25jZQ==\r\nConnection: upgrade\r\nUpgrade: websocket\r\nHost: example.com\r\n\r\n";

        network::websocket::http_handler_context context;
        context.head1 = context.kWebsocketMethod;
        context.head2 = "/api111";
        context.head3 = context.kHttpVersion;
        context.headers.emplace(context.kHttpHost, "example.com");
        context.headers.emplace(context.kHttpUpgradeStr, context.kHttpWebsocketStr);
        context.headers.emplace(context.kHttpConnection, context.kHttpUpgradeValueStr);
        context.headers.emplace(context.kWebsocketRequestKey, "dGhlIHNhbXBsZSBub25jZQ==");
        context.headers.emplace(context.kWebsocketRequestVersion, context.kWebsocketVersion);
        auto encode_str = context.encode();
        EXPECT_EQ(encode_str, ws_context);
        network::websocket::http_handler_context parse_context;
        std::size_t i = 0;
        encode_str.push_back('a');
        for (; i < encode_str.size() - 3; i += 2) {
            auto ret    = parse_context.parse(encode_str.data() + i, 2);
            auto status = std::get<0>(ret);
            auto len    = std::get<1>(ret);
            EXPECT_EQ(status, network::websocket::http_handler_context::parse_need_more_data);
            EXPECT_EQ(len, 2);
        }
        auto left_size = encode_str.size() - i;
        auto ret       = parse_context.parse(encode_str.data() + i, left_size);
        auto status    = std::get<0>(ret);
        auto len       = std::get<1>(ret);
        EXPECT_EQ(status, network::websocket::http_handler_context::parse_success);
        EXPECT_EQ(len, left_size - 1);

        EXPECT_EQ(parse_context.head1, context.head1);
        EXPECT_EQ(parse_context.head2, context.head2);
        EXPECT_EQ(parse_context.head3, context.head3);

        for (auto& item : context.headers) {
            auto iter = parse_context.headers.find(item.first);
            EXPECT_TRUE(iter != parse_context.headers.end() && item.second == iter->second);
        }
    }
    {
        std::string ws_context =
            "HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: dGhlIHNhbXBsZSBub25jZQ==\r\nConnection: "
            "upgrade\r\nUpgrade: websocket\r\n\r\n";

        network::websocket::http_handler_context context;
        context.head1 = context.kHttpVersion;
        context.head2 = context.kWebsocketSuccessCode;
        context.head3 = context.kWebsocketSuccessStr;
        context.headers.emplace(context.kHttpUpgradeStr, context.kHttpWebsocketStr);
        context.headers.emplace(context.kHttpConnection, context.kHttpUpgradeValueStr);
        context.headers.emplace(context.kWebsocketResponseAccept, "dGhlIHNhbXBsZSBub25jZQ==");
        auto encode_str = context.encode();
        EXPECT_EQ(encode_str, ws_context);
        network::websocket::http_handler_context parse_context;
        std::size_t i = 0;
        encode_str.push_back('a');
        for (; i < encode_str.size() - 3; i += 2) {
            auto ret    = parse_context.parse(encode_str.data() + i, 2);
            auto status = std::get<0>(ret);
            auto len    = std::get<1>(ret);
            EXPECT_EQ(status, network::websocket::http_handler_context::parse_need_more_data);
            EXPECT_EQ(len, 2);
        }
        auto left_size = encode_str.size() - i;
        auto ret       = parse_context.parse(encode_str.data() + i, left_size);
        auto status    = std::get<0>(ret);
        auto len       = std::get<1>(ret);
        EXPECT_EQ(status, network::websocket::http_handler_context::parse_success);
        EXPECT_EQ(len, left_size - 1);

        EXPECT_EQ(parse_context.head1, context.head1);
        EXPECT_EQ(parse_context.head2, context.head2);
        EXPECT_EQ(parse_context.head3, context.head3);

        for (auto& item : context.headers) {
            auto iter = parse_context.headers.find(item.first);
            EXPECT_TRUE(iter != parse_context.headers.end() && item.second == iter->second);
        }
    }
}
    #if 1
TEST(rpc_test, test_connect) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    auto client             = network::make_guard<rpc_client>();
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r && client->has_connected());
}

TEST(rpc_test, test_add) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    try {
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");

        bool r = client->connect();
        if (!r) {
            EXPECT_TRUE(false && "connect timeout");
            return;
        }
        std::int32_t op1 = 1;
        std::int32_t op2 = 2;
        {
            auto result = client->call<std::int32_t>("add", op1, op2);
            EXPECT_EQ(op1 + op2, result);
        }

        {
            auto result = client->call<2000, std::int32_t>("add", op2, op1);
            EXPECT_EQ(op1 + op2, result);
        }
        // test return value type not matched
        EXPECT_THROW((client->call<2000, std::string>("add", op2, op1)), std::invalid_argument);
    } catch (const std::exception& e) {
        std::cout << __func__ << ":" << e.what() << std::endl;
    }
}

TEST(rpc_test, test_translate) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    try {
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
        bool r      = client->connect();
        if (!r) {
            EXPECT_TRUE(false && "connect timeout");
            return;
        }

        auto result = client->call<std::string>("translate", "hello");
        EXPECT_TRUE(result == "HELLO");
    } catch (const std::exception& e) {
        std::cout << __func__ << ":" << e.what() << std::endl;
    }
}

TEST(rpc_test, test_hello) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    try {
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
        bool r      = client->connect();
        if (!r) {
            EXPECT_TRUE(false && "connect timeout");
            return;
        }
        client->call("hello", "purecpp");
    } catch (const std::exception& e) {
        std::cout << __func__ << ":" << e.what() << std::endl;
    }
}
TEST(rpc_test, test_aborted_stream) {
    auto g        = Fundamental::DefaultNumberGenerator<std::size_t>(1, 10);
    auto test_cnt = g();
    while (test_cnt > 0) {
        test_cnt--;
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_abort_stream");
        if (!stream) break;
        stream->EnableAutoHeartBeat(true, 1000);
        bool aborted = false;
        std::mutex lock;
        std::condition_variable cv;
        stream->notify_stream_abort.Connect([&]() {
            FWARN("remote stream aborted");
            std::scoped_lock<std::mutex> locker(lock);
            aborted = true;
            cv.notify_one();
        });
        EXPECT_TRUE(stream->WriteDone());
        std::size_t max_try_cnt = 5;
        { // block wait connection disconneted
            std::unique_lock<std::mutex> locker(lock);
            while (!aborted && max_try_cnt > 0) {
                --max_try_cnt;
                cv.wait_for(locker, std::chrono::milliseconds(10));
            }
        }
        // we won't call finish,disconenction
    }
}
TEST(rpc_test, test_get_person_name) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    try {
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
        bool r      = client->connect();
        if (!r) {
            EXPECT_TRUE(false && "connect timeout");
            return;
        }
        std::string name = "tom";
        auto result      = client->call<std::string>("get_person_name", person { 1, name, 20 });
        EXPECT_EQ(name, result);
    } catch (const std::exception& e) {
        std::cout << __func__ << ":" << e.what() << std::endl;
    }
}
TEST(rpc_test, test_get_person) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    try {
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
        bool r      = client->connect();
        if (!r) {
            EXPECT_TRUE(false && "connect timeout");
            return;
        }
        auto result = client->call<50, person>("get_person");
        EXPECT_EQ("tom", result.name);
    } catch (const std::exception& e) {
        std::cout << __func__ << ":" << e.what() << std::endl;
    }
}

TEST(rpc_test, test_async_client) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    bool r      = client->connect();
    if (!r) {
        EXPECT_TRUE(false && "connect timeout");
        return;
    }

    client->set_error_callback([](asio::error_code ec) { std::cout << ec.message() << std::endl; });

    auto f = client->async_call("get_person");
    f.guard_post_request();
    EXPECT_EQ("tom", f.get().as<person>().name);
    auto fu = client->async_call("hello", "purecpp");
    fu.guard_post_request();
    fu.get().as(); // no return
}

static std::vector<std::uint8_t> s_file_data(1024 * 1024, 'a');
TEST(rpc_test, test_upload) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    bool r      = client->connect(1);
    if (!r) {
        EXPECT_TRUE(false && "connect timeout");
        return;
    }
    std::string file_path = "test.file";

    EXPECT_TRUE(Fundamental::fs::WriteFile(file_path, s_file_data.data(), s_file_data.size()));
    std::ifstream file(file_path, std::ios::binary);
    file.seekg(0, std::ios::end);
    size_t file_len = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string conent;
    conent.resize(file_len);
    file.read(&conent[0], file_len);

    {
        auto f = client->async_call("upload", "test", conent);
        f.guard_post_request();
        EXPECT_NO_THROW((f.get().as()));
    }
    {
        auto f = client->async_call("upload", "test1", conent);
        f.guard_post_request();
        EXPECT_NO_THROW((f.get().as()));
    }
}

TEST(rpc_test, test_download) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    bool r      = client->connect(1);
    if (!r) {
        EXPECT_TRUE(false && "connect timeout");
        return;
    }

    auto f       = client->async_call("download", "test");
    f.guard_post_request();
    auto content = f.get().as<std::string>();
    EXPECT_TRUE(s_file_data.size() == content.size() &&
                ::memcmp(s_file_data.data(), content.data(), content.size()) == 0);
}

TEST(rpc_test, test_echo) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    bool r      = client->connect();
    if (!r) {
        EXPECT_TRUE(false && "connect timeout");
        return;
    }

    {
        dummy1 d1 { 42, "test" };
        auto result = client->call<dummy1>("get_dummy", d1);
        EXPECT_TRUE(d1.id == result.id);
        EXPECT_TRUE(d1.str == result.str);
    }

    {
        auto result = client->call<std::string>("echo", "test");
        EXPECT_EQ(result, "test");
    }

    {
        auto result = client->call<std::string>("delay_echo", "test", 50);
        EXPECT_EQ(result, "test");
    }
}

TEST(rpc_test, test_call_with_timeout) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    auto client = network::make_guard<rpc_client>();
    client->async_connect("127.0.0.1", "9000");

    try {
        auto result = client->call<50, person>("get_person");
        std::cout << result.name << std::endl;
        result = client->call<50, person>("get_person");
        std::cout << result.name << std::endl;
    } catch (const std::exception& ex) {
        std::cout << "test_call_with_timeout:throw " << ex.what() << std::endl;
        EXPECT_TRUE(false);
    }
}

TEST(rpc_test, test_callback) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 200); });
    std::atomic<std::size_t> count = 200;
    std::atomic_bool is_failed     = false;
    auto client                    = network::make_guard<rpc_client>();
    client->enable_auto_reconnect();
    client->enable_timeout_check();
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r);

    Fundamental::Application::Instance().exitStarted.Connect([&]() { is_failed.exchange(true); });
    for (size_t i = 0; i < 100; i++) {
        std::string test = "test_callback " + std::to_string(i + 1);
        // set timeout 100ms
        FDEBUGS << "post delay_echo:" << test;
        client->async_call("delay_echo", test, 50)
            .async_response<std::string>([&, i](Fundamental::error_code ec, std::string data) {
                Fundamental::ScopeGuard g([&]() { --count; });
                if (ec) {
                    FINFOS << i << " delay_echo timeout:" << ec;
                    is_failed.exchange(true);
                    return;
                }
                FWARNS << "delay_echo " << data;
            });

        std::string test1 = "test_callback " + std::to_string(i + 2);
        FDEBUGS << "post echo:" << test1;
        // zero means no timeout check, no param means using default timeout(5s)
        client->async_call("echo", test1)
            .async_response<std::string>([&](Fundamental::error_code ec, std::string data) {
                Fundamental::ScopeGuard g([&]() { --count; });
                if (ec) {
                    FINFOS << "echo timeout:" << ec;
                    is_failed.exchange(true);
                    return;
                }

                FWARNS << "echo " << data;
            });
    }
    while (count.load() != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(!is_failed);
}

TEST(rpc_test, test_proxy) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });

    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    client->set_proxy(gen_pipe_proxy());
    bool r = client->connect();
    if (!r) {
        EXPECT_TRUE(false && "connect timeout");
        return;
    }

    {
        dummy1 d1 { 42, "test" };
        auto result = client->call<dummy1>("get_dummy", d1);
        EXPECT_TRUE(d1.id == result.id);
        EXPECT_TRUE(d1.str == result.str);
    }

    {
        auto result = client->call<std::string>("echo", "test");
        EXPECT_EQ(result, "test");
    }
}
TEST(rpc_test, test_auto_reconnect) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 50); });

    try {
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
        client->enable_auto_reconnect();
        client->set_reconnect_delay(10);
        bool r = client->connect();
        if (!r) {
            EXPECT_TRUE(false && "connect timeout");
            return;
        }
        std::int32_t cnt = 3;
        while (cnt > 0) {
            --cnt;
            try {
                client->call<void>("auto_disconnect", cnt);
            } catch (...) {
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    } catch (const std::exception& e) {
        std::cout << __func__ << ":" << e.what() << std::endl;
    }
}
TEST(rpc_test, test_sub1) {
    Fundamental::Timer check_timer;
    Fundamental::ScopeGuard check_guard(
        [&]() { EXPECT_LE(check_timer.GetDuration<Fundamental::Timer::TimeScale::Millisecond>(), 100); });
    static bool success = true;
    Fundamental::Application::Instance().exitStarted.Connect([&]() { success = (false); });
    do {
        auto client = network::make_guard<rpc_client>();
        client->enable_auto_reconnect();
        client->enable_timeout_check();
        bool r = client->connect("127.0.0.1", "9000");
        if (!r) {
            success = false;
            break;
        }
        std::atomic<std::size_t> target_count = 0;
        client
            ->subscribe("key",
                        [&](string_view data) {
                            msgpack_codec codec;
                            try {
                                auto msg = codec.unpack<std::string>(data.data(), data.size());
                                std::cout << "key_1:" << msg << "\n";
                                target_count++;
                            } catch (const std::exception& e) {
                                std::cerr << e.what() << '\n';
                                success = false;
                            }
                        })
            .get();
        client
            ->subscribe("key_p",
                        [&](string_view data) {
                            msgpack_codec codec;
                            try {
                                auto msg = codec.unpack<person>(data.data(), data.size());
                                std::cout << "key_p:" << msg.name << "\n";
                                target_count++;
                            } catch (const std::exception& e) {
                                std::cerr << e.what() << '\n';
                                success = false;
                            }
                        })
            .get();
        auto client2 = network::make_guard<rpc_client>();
        client2->enable_auto_reconnect();
        client2->enable_timeout_check();
        r = client2->connect("127.0.0.1", "9000");
        if (!r) {
            success = false;
            break;
        }

        client2
            ->subscribe("key",
                        [&](string_view data) {
                            msgpack_codec codec;
                            try {
                                auto msg = codec.unpack<std::string>(data.data(), data.size());
                                std::cout << "key2:" << msg << "\n";
                                target_count++;
                                if (target_count.load() > 4) {
                                    client2->unsubscribe("key");
                                }
                            } catch (const std::exception& e) {
                                success = false;
                                std::cerr << e.what() << '\n';
                            }
                        })
            .get();

        auto client3 = network::make_guard<rpc_client>();
        client3->enable_auto_reconnect();
        client3->enable_timeout_check();
        r = client3->connect("127.0.0.1", "9000");
        if (!r) {
            success = false;
            break;
        }

        person p { 10, "jack_client", 21 };
        client3->publish("key", "publish msg from client").get();
        while (success && target_count.load() < 10)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (0);
    EXPECT_EQ(success, true);
}

TEST(rpc_test, basice_rpc_stream_test) {
    auto client             = network::make_guard<rpc_client>();
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r && client->has_connected());
    auto ptr = client->upgrade_to_stream("test_stream");
    EXPECT_TRUE(ptr != nullptr);
}

TEST(rpc_test, basice_rpc_stream_read_write) {
    auto client             = network::make_guard<rpc_client>();
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r && client->has_connected());
    auto stream = client->upgrade_to_stream("test_stream");
    EXPECT_TRUE(stream != nullptr);
    person p;
    p.id            = 0;
    p.age           = 10;
    p.name          = "jack ";
    std::size_t cnt = 0;
    while (cnt < 5) {
        p.id = cnt;
        p.age += cnt;
        p.name += std::to_string(cnt);
        EXPECT_TRUE(stream->Write(p));
        ++cnt;
    }
    EXPECT_TRUE(stream->WriteDone());
    std::size_t read_cnt = 0;
    while (stream->Read(p, 0)) {
        ++read_cnt;
        FINFO("id:{},age:{},name:{}", p.id, p.age, p.name);
    }
    EXPECT_TRUE(read_cnt == cnt);
    EXPECT_TRUE(!stream->Finish(0));
}

TEST(rpc_test, basice_rpc_stream_read_only) {
    auto client             = network::make_guard<rpc_client>();
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r && client->has_connected());
    auto stream = client->upgrade_to_stream("test_read_stream");
    EXPECT_TRUE(stream != nullptr);
    person p;
    p.id            = 0;
    p.age           = 10;
    p.name          = "jack ";
    std::size_t cnt = 0;
    // test delay read
    EXPECT_TRUE(!stream->Read(p, 10));
    while (stream->Read(p, 0)) {
        FINFO("id:{},age:{},name:{}", p.id, p.age, p.name);
    }
    while (cnt < 2) {
        p.id = cnt;
        p.age += cnt;
        p.name += std::to_string(cnt);
        EXPECT_TRUE(stream->Write(p));
        ++cnt;
    }
    EXPECT_TRUE(stream->WriteDone());

    EXPECT_TRUE(!stream->Finish(0));
}

TEST(rpc_test, basice_rpc_stream_write_only) {
    auto client             = network::make_guard<rpc_client>();
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r && client->has_connected());
    auto stream = client->upgrade_to_stream("test_write_stream");
    EXPECT_TRUE(stream != nullptr);
    person p;
    p.id            = 0;
    p.age           = 10;
    p.name          = "jack ";
    std::size_t cnt = 0;
    while (cnt < 2) {
        p.id = cnt;
        p.age += cnt;
        p.name += std::to_string(cnt);
        EXPECT_TRUE(stream->Write(p));
        ++cnt;
    }
    EXPECT_TRUE(stream->WriteDone());
    while (stream->Read(p, 0)) {
        FINFO("id:{},age:{},name:{}", p.id, p.age, p.name);
    }
    EXPECT_TRUE(!stream->Finish(0));
}
    #endif
TEST(rpc_test, test_broken_rpc_stream) {
    auto client             = network::make_guard<rpc_client>();
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r && client->has_connected());
    auto stream = client->upgrade_to_stream("test_broken_stream");
    EXPECT_TRUE(stream != nullptr);
    person p;
    std::size_t cnt = 0;
    while (cnt < 2) {
        stream->Write(p);
        ++cnt;
    }
    stream->WriteDone();
    while (stream->Read(p, 0)) {
    }
    EXPECT_TRUE(stream->Finish(0));
}

TEST(rpc_test, test_call_rpc_stream_with_no_stream_action) {
    auto client             = network::make_guard<rpc_client>();
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r && client->has_connected());
    auto func = [&]() { client->call<5000, void>("test_echo_stream"); };
    EXPECT_THROW(func(), std::logic_error);
}

    #if 1
TEST(rpc_test, test_echo_stream) {
    auto client             = network::make_guard<rpc_client>();
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r && client->has_connected());
    auto stream = client->upgrade_to_stream("test_echo_stream");
    EXPECT_TRUE(stream != nullptr);
    std::size_t cnt  = 10;
    std::string base = "msg ";
    while (cnt != 0) {
        EXPECT_TRUE(stream->Write(base + std::to_string(cnt)));
        --cnt;
        std::string tmp;
        EXPECT_TRUE(stream->Read(tmp));
        FINFO("echo msg:{}", tmp);
    }
    EXPECT_TRUE(stream->WriteDone());
    EXPECT_TRUE(!stream->Finish(0));
}

TEST(rpc_test, test_obj_echo) {
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    client->set_proxy(gen_pipe_proxy());
    bool r = client->connect();
    if (!r) {
        EXPECT_TRUE(false && "connect timeout");
        return;
    }
    std::int32_t c = 0;
    std::string str;
    str = std::string(781, 'a');
    try {
        auto ret = client->call<100, std::string>("echo", str);
        if (str != ret) {
            FERR("error finished {}", c);
        }
    } catch (const std::exception& e) {
        FERR("exception {}->{}", c, e.what());
    }
    std::int32_t max_call_times = 20;
    while (c < max_call_times) {
        str.push_back('a');
        try {
            auto ret = client->call<100, std::string>("echo", str);
            if (str != ret) {
                FERR("error finished {}", c);
                break;
            }
        } catch (const std::exception& e) {
            FERR("exception {}->{}", c, e.what());
            break;
        }
        ++c;
    }
    EXPECT_TRUE(c >= max_call_times);
}

TEST(rpc_test, test_timeout_echo) {
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    bool r      = client->connect();
    client->enable_timeout_check(true, 1);
    if (!r) {
        EXPECT_TRUE(false && "connect timeout");
        return;
    }

    {
        auto test_str = std::string(1024 * 1024 * 10, 'a');
        auto result   = client->async_timeout_call("delay_echo", 200, test_str, 400);
        EXPECT_ANY_THROW(result.get());
    }
}

TEST(rpc_test, test_echo_stream_mutithread) {
    std::vector<Fundamental::ThreadPoolTaskToken<void>> tasks;
    auto nums      = 40;
    auto task_func = []() {
        auto client = network::make_guard<rpc_client>();

        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        EXPECT_TRUE(stream != nullptr);
        std::size_t cnt  = 5;
        std::string base = "msg ";
        while (cnt != 0) {
            EXPECT_TRUE(stream->Write(base + std::to_string(cnt)));
            --cnt;
            std::string tmp;
            EXPECT_TRUE(stream->Read(tmp));
            FINFO("mutithread echo msg:{}", tmp);
        }
        EXPECT_TRUE(stream->WriteDone());
        EXPECT_TRUE(!stream->Finish(0));
    };
    while (nums > 0) {
        tasks.emplace_back(s_test_pool.Enqueue(task_func));
        nums--;
    }
    for (auto& f : tasks)
        EXPECT_NO_THROW(f.resultFuture.get());
}

TEST(rpc_test, test_echo_stream_proxy_mutithread) {
    std::vector<Fundamental::ThreadPoolTaskToken<void>> tasks;
    auto nums      = 40;
    auto task_func = []() {
        auto client = network::make_guard<rpc_client>();
        client->set_proxy(gen_pipe_proxy());
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        EXPECT_TRUE(stream != nullptr);
        std::size_t cnt  = 5;
        std::string base = "msg ";
        while (cnt != 0) {
            EXPECT_TRUE(stream->Write(base + std::to_string(cnt)));
            --cnt;
            std::string tmp;
            EXPECT_TRUE(stream->Read(tmp));
            FINFO("mutithread echo msg:{}", tmp);
        }
        EXPECT_TRUE(stream->WriteDone());
        EXPECT_TRUE(!stream->Finish(0));
    };
    while (nums > 0) {
        tasks.emplace_back(s_test_pool.Enqueue(task_func));
        nums--;
    }
    for (auto& f : tasks)
        EXPECT_NO_THROW(f.resultFuture.get());
}

TEST(rpc_test, test_control_stream) {
    {
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_control_stream");
        EXPECT_TRUE(stream != nullptr);
        std::string base = "msg ";
        DelayControlStream echo_request;
        echo_request.cmd           = "echo";
        echo_request.msg           = "echo msg";
        echo_request.process_delay = 200;
        DelayControlStream set_request;
        set_request.cmd           = "set";
        set_request.process_delay = 100; // 5s
        set_request.msg           = "";
        stream->Write(set_request);
        // stream->EnableAutoHeartBeat(true,30);
        EXPECT_TRUE(stream->Write(echo_request));
        EXPECT_TRUE(stream->WriteDone());
        std::string echo_msg;
        EXPECT_FALSE(stream->Read(echo_msg, 0));
        // error code !=0
        EXPECT_TRUE(stream->Finish(0));
    }
    {
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_control_stream");
        EXPECT_TRUE(stream != nullptr);
        std::string base = "msg ";
        DelayControlStream echo_request;
        echo_request.cmd           = "echo";
        echo_request.msg           = "echo msg";
        echo_request.process_delay = 200;
        DelayControlStream set_request;
        set_request.cmd           = "set";
        set_request.process_delay = 100; // 5s
        set_request.msg           = "";
        stream->Write(set_request);
        stream->EnableAutoHeartBeat(true, 30);
        EXPECT_TRUE(stream->Write(echo_request));
        EXPECT_TRUE(stream->WriteDone());
        std::string echo_msg;
        EXPECT_TRUE(stream->Read(echo_msg, 0));
        // error code !=0
        EXPECT_FALSE(stream->Finish(0));
    }
    { // test read some
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_control_stream");
        EXPECT_TRUE(stream != nullptr);
        std::string base = "msg ";
        DelayControlStream echo_request;
        echo_request.cmd           = "echo";
        echo_request.msg           = std::string(1024 * 1024 * 10, 'a');
        echo_request.process_delay = 4;
        DelayControlStream set_request;
        set_request.cmd           = "set";
        set_request.process_delay = 40;
        set_request.msg           = "";
        stream->Write(set_request);
        EXPECT_TRUE(stream->Write(echo_request));
        EXPECT_TRUE(stream->WriteDone());
        std::string echo_msg;
        EXPECT_TRUE(stream->Read(echo_msg, 0));
        // error code !=0
        EXPECT_FALSE(stream->Finish(0));
    }
    { // test read some
        auto client = network::make_guard<rpc_client>();
        client->config_tcp_no_delay();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_control_stream");
        stream->EnableAutoHeartBeat(true, 5);
        EXPECT_TRUE(stream != nullptr);
        std::string base = "msg ";
        DelayControlStream echo_request;
        echo_request.cmd           = "echo";
        echo_request.msg           = std::string(1024 * 1024 * 10, 'a');
        echo_request.process_delay = 4;
        DelayControlStream set_request;
        set_request.cmd           = "set";
        set_request.process_delay = 6;
        set_request.msg           = "";
        stream->Write(set_request);
        EXPECT_TRUE(stream->Write(echo_request));
        EXPECT_TRUE(stream->WriteDone());
        std::string echo_msg;
        EXPECT_TRUE(stream->Read(echo_msg, 0));
        // error code !=0
        EXPECT_FALSE(stream->Finish(0));
    }
    { // test read some
        auto client = network::make_guard<rpc_client>();
        client->config_tcp_no_delay();
        [[maybe_unused]] bool r = client->connect("::", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_control_stream");
        EXPECT_TRUE(stream != nullptr);
        std::string base = "msg ";
        DelayControlStream echo_request;
        echo_request.cmd           = "echo";
        echo_request.msg           = std::string(1024 * 1024 * 10, 'a');
        echo_request.process_delay = 4;
        DelayControlStream set_request;
        set_request.cmd           = "set";
        set_request.process_delay = 6;
        set_request.msg           = "";
        stream->Write(set_request);
        stream->Write(echo_request);
        stream->WriteDone();
        std::string echo_msg;
        EXPECT_FALSE(stream->Read(echo_msg, 0));
        // error code !=0
        EXPECT_TRUE(stream->Finish(0));
    }
}
    #endif
    #if !defined(NETWORK_DISABLE_SSL) && 1
TEST(rpc_test, test_ssl) {
    std::string ssl_token                              = "ssl";
    std::string proxy_token                            = "proxy";
    std::string preload_token                          = "preload";
    std::vector<std::unordered_set<std::string>> tasks = {
        {}, { ssl_token }, { proxy_token }, { ssl_token, proxy_token, preload_token }
    };
    auto enable_no_ssl = ::getenv("disable_no_ssl") == nullptr;
    for (auto& test_tokens : tasks) {
        auto client     = network::make_guard<rpc_client>("127.0.0.1", "9000");
        auto ssl_iter   = test_tokens.find(ssl_token);
        auto proxy_iter = test_tokens.find(proxy_token);
        auto test_tag   = Fundamental::StringFormat("{}[{}] {}[{}]", ssl_token, ssl_iter != test_tokens.end(),
                                                    proxy_token, proxy_iter != test_tokens.end());
        if (ssl_iter != test_tokens.end()) {
            network_client_ssl_config config { "local.crt", "local.key", "ca_root.crt" };
            if (test_tokens.count(preload_token)) config.preload();
            client->enable_ssl(config);
        }
        if (proxy_iter != test_tokens.end()) {
            client->set_proxy(gen_pipe_proxy());
        }

        try {

            bool r = client->connect();
            if (!r) {

                EXPECT_TRUE((!enable_no_ssl && ssl_iter == test_tokens.end()) && "connect failed");
                continue;
            }
            FINFO("connect success test {}", test_tag);
            client->call<std::string>("echo", test_tag);
            FINFO("finished test {}", test_tag);
        } catch (const std::exception& e) {
            FERR("{} test {} failed:{} ", __func__, test_tag, e.what());
            EXPECT_TRUE((!enable_no_ssl && ssl_iter == test_tokens.end()) && "protocal error");
        }
    }
    // test no ca client
    bool verify_client = ::getenv("verify_client") != nullptr;
    do {
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");

        client->enable_ssl(network_client_ssl_config { "", "", "ca_root.crt" });
        auto test_tag = Fundamental::StringFormat("verify:{}", verify_client);
        try {

            bool r = client->connect();
            if (!r) {

                EXPECT_TRUE((verify_client) && "connect failed");
                break;
            }

            FINFO("connect success test {}", test_tag);
            client->call<std::string>("echo", test_tag);
            FINFO("finished test {}", test_tag);
        } catch (const std::exception& e) {
            FERR("{} test {} failed:{} ", __func__, test_tag, e.what());
            EXPECT_TRUE((verify_client) && "protocal error");
        }
    } while (0);
}

TEST(rpc_test, test_ssl_proxy_echo_stream) {
    auto client = network::make_guard<rpc_client>();
    client->enable_ssl(network_client_ssl_config { "local.crt", "local.key", "ca_root.crt" });
    client->set_proxy(gen_pipe_proxy());
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r && client->has_connected());
    auto stream = client->upgrade_to_stream("test_echo_stream");
    EXPECT_TRUE(stream != nullptr);
    std::size_t cnt  = 5;
    std::string base = "msg ";
    while (cnt != 0) {
        EXPECT_TRUE(stream->Write(base + std::to_string(cnt)));
        --cnt;
        std::string tmp;
        EXPECT_TRUE(stream->Read(tmp));
        FINFO("ssl/proxy echo msg:{}", tmp);
    }
    EXPECT_TRUE(stream->WriteDone());
    EXPECT_TRUE(!stream->Finish(0));
}
TEST(rpc_test, test_ssl_concept) {
    bool verify_client = ::getenv("verify_client") != nullptr;
    bool enable_no_ssl = ::getenv("disable_no_ssl") == nullptr;
    // sudo tcpdump -i any -n -vv -X port 9000
    {
        auto client = network::make_guard<rpc_client>();
        client->enable_ssl(network_client_ssl_config { "local.crt", "local.key", "ca_root.crt", false },
                           network::rpc_service::rpc_client_ssl_level_optional);
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        EXPECT_TRUE(stream != nullptr);
        std::string base = "1111";
        EXPECT_TRUE(stream->Write(base));
        EXPECT_TRUE(stream->WriteDone());
        EXPECT_TRUE(!stream->Finish(0));
    }
    {
        auto client = network::make_guard<rpc_client>();
        client->enable_ssl(network_client_ssl_config { "client_none.crt", "local.key", "ca_root.crt", false },
                           network::rpc_service::rpc_client_ssl_level_optional);
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        EXPECT_TRUE(stream != nullptr);
        std::string base = "1111";
        stream->Write(base);
        stream->WriteDone();
        auto ec = stream->Finish(0);
        if (verify_client) {
            EXPECT_TRUE(ec);
        } else {
            EXPECT_TRUE(!ec);
        }
    }
    {
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        EXPECT_TRUE(stream != nullptr);
        std::string base = "1111";
        stream->Write(base);
        stream->WriteDone();
        if (enable_no_ssl) {
            EXPECT_TRUE(!stream->Finish(0));
        } else {
            EXPECT_TRUE(stream->Finish(0));
        }
    }
}

TEST(rpc_test, test_ssl_proxy_echo_stream_mutithread) {
    std::vector<Fundamental::ThreadPoolTaskToken<void>> tasks;
    auto nums      = s_test_pool.Count();
    auto task_func = []() {
        auto client = network::make_guard<rpc_client>();
        client->enable_ssl(network_client_ssl_config { "local.crt", "local.key", "ca_root.crt" });
        client->set_proxy(gen_pipe_proxy());
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        EXPECT_TRUE(stream != nullptr);
        std::size_t cnt  = 5;
        std::string base = "msg ";
        while (cnt != 0) {
            EXPECT_TRUE(stream->Write(base + std::to_string(cnt)));
            --cnt;
            std::string tmp;
            EXPECT_TRUE(stream->Read(tmp));
            FINFO("ssl/proxy echo msg:{}", tmp);
        }
        EXPECT_TRUE(stream->WriteDone());
        EXPECT_TRUE(!stream->Finish(0));
    };
    while (nums > 0) {
        tasks.emplace_back(s_test_pool.Enqueue(task_func));
        nums--;
    }
    for (auto& f : tasks)
        EXPECT_NO_THROW(f.resultFuture.get());
}
    #endif

TEST(rpc_test, test_void_stream) {
    auto client             = network::make_guard<rpc_client>();
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_TRUE(r && client->has_connected());
    auto stream = client->upgrade_to_stream("test_void_stream");
    EXPECT_TRUE(stream != nullptr);
    EXPECT_TRUE(stream->Write(1));
    EXPECT_TRUE(stream->ReadEmpty());

    std::size_t cnt = 10;
    while (cnt != 0) {

        --cnt;
        EXPECT_TRUE(stream->WriteEmpty());
        EXPECT_TRUE(stream->ReadEmpty());
    }
    EXPECT_TRUE(stream->WriteDone());
    EXPECT_TRUE(!stream->Finish(0));
}
TEST(rpc_test, test_proxy_list) {
    auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
    // add server test
    client->append_proxy(gen_pipe_add_server_proxy("/ws_proxy_dynamic"));
    client->append_proxy(gen_pipe_proxy());
    auto ws_upgrade = proxy::ws_upgrade_imp::make_shared("/ws_proxy_dynamic", "127.0.0.1");
    client->append_proxy(ws_upgrade);
    auto socks5_proxy = SocksV5::socks5_proxy_imp::make_shared("127.0.0.1", 9000, "", "");
    client->append_proxy(socks5_proxy);
    bool r = client->connect();
    if (!r) {
        EXPECT_TRUE(false && "connect timeout");
        return;
    }
    std::int32_t c = 0;
    std::string str;
    str = std::string(10, 'a');
    std::string ret;
    try {
        ret = client->call<100, std::string>("echo", str);
    } catch (const std::exception& e) {
        FERR("exception {}->{}", c, e.what());
    }
    EXPECT_EQ(ret, str);
}

TEST(rpc_test, test_proxy_prefix_path) {
    {
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
        client->append_proxy(gen_pipe_proxy("/nginx/ws_proxy"));
        bool r = client->connect();
        EXPECT_TRUE(r);
    }
    {
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
        client->append_proxy(gen_pipe_proxy("/nginx/invalid_prefix/nginx/test_remove/ws_proxy"));
        bool r = client->connect();
        EXPECT_TRUE(r);
    }
    { // test invalid path
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
        client->append_proxy(gen_pipe_proxy("/nginx/invalid_prefix/nginx/test_remove///ws_proxy"));
        bool r = client->connect();
        EXPECT_FALSE(r);
    }
    { // test forward limit
        auto client = network::make_guard<rpc_client>("127.0.0.1", "9000");
        client->append_proxy(gen_pipe_proxy("/ws_proxy_next_layer"));
        bool r = client->connect();
        EXPECT_TRUE(r);
    }
}

TEST(rpc_test, test_echo_stream_limit) {
    { // nomal_case
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_delay_limit_stream", 10000, 1000, 1);
        stream->EnableAutoHeartBeat(true, 100);
        EXPECT_TRUE(stream != nullptr);
        std::string v(2 * 1024 * 1024, '1');

        // normal
        std::size_t send_cnt = 0;
        while (true) {
            if (!stream->Write(v, 40)) break;
            ++send_cnt;
            if (send_cnt > 30) break;
        }
        EXPECT_TRUE(stream->WriteDone());
        EXPECT_TRUE(!stream->Finish(0));
        EXPECT_TRUE(send_cnt > 30);
    }
    { // client write + server read limit
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_delay_limit_stream", 10000, 1000, 1);
        stream->EnableAutoHeartBeat(true, 100);
        EXPECT_TRUE(stream != nullptr);
        std::string v(2 * 1024 * 1024, '1');

        std::thread t([&]() {
            std::string recv_v;
            std::size_t cnt = 0;
            while (stream->Read(recv_v, 40)) {
                ++cnt;
            }
        });
        // normal
        std::size_t send_cnt = 0;
        while (true) {
            if (!stream->Write(v, 10)) break;
            ++send_cnt;
        }
        EXPECT_TRUE(stream->WriteDone());
        EXPECT_TRUE(!stream->Finish(0));
        EXPECT_TRUE(send_cnt < 25);
        t.join();
    }
    { // client read + server write limit
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_delay_limit_stream", 10000, 1, 10);
        stream->EnableAutoHeartBeat(true, 100);
        EXPECT_TRUE(stream != nullptr);
        std::string v(2 * 1024 * 1024, '1');
        // normal
        std::size_t send_cnt = 0;
        while (true) {
            if (!stream->Write(v, 10)) break;
            ++send_cnt;
        }
        EXPECT_TRUE(stream->WriteDone());
        EXPECT_TRUE(!stream->Finish(0));
        EXPECT_TRUE(send_cnt < 25);
    }
    { // client write + server read limit with proxy
      // broken by read/write timeout
        auto client = network::make_guard<rpc_client>();
        client->set_proxy(gen_pipe_proxy());
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");

        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_delay_limit_stream", 10000, 1000, 1);
        stream->EnableAutoHeartBeat(true, 100);
        EXPECT_TRUE(stream != nullptr);
        std::string v(2 * 1024 * 1024, '1');

        std::thread t([&]() {
            std::string recv_v;
            std::size_t cnt = 0;
            while (stream->Read(recv_v, 40)) {
                ++cnt;
            }
        });
        // normal
        std::size_t send_cnt = 0;
        while (true) {
            if (!stream->Write(v, 10)) break;
            ++send_cnt;
        }
        EXPECT_TRUE(send_cnt < 25);
        EXPECT_TRUE(stream->WriteDone());
        auto ec = stream->Finish(0);
        EXPECT_EQ(ec.value(), static_cast<std::int32_t>(network::rpc_service::error::rpc_errors::rpc_success));

        t.join();
    }
    { // client read + server write limit with proxy
        auto client = network::make_guard<rpc_client>();

        auto ws_upgrade = proxy::ws_upgrade_imp::make_shared("/ws_proxy", "127.0.0.1");
        client->append_proxy(ws_upgrade);

        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_delay_limit_stream", 10000, 1, 2);
        stream->EnableAutoHeartBeat(true, 200);
        EXPECT_TRUE(stream != nullptr);
        std::string v(1024 * 1024, '1');
        // normal
        std::size_t send_cnt = 0;
        while (true) {
            if (!stream->Write(v, 5000)) break;
            ++send_cnt;
        }
        stream->WriteDone();
        EXPECT_TRUE(stream->Finish(0));
    }
    { // client read + server write limit with pipe proxy
        auto client = network::make_guard<rpc_client>();
        client->set_proxy(gen_pipe_proxy());

        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_delay_limit_stream", 10000, 1, 2);
        stream->EnableAutoHeartBeat(true, 200);
        EXPECT_TRUE(stream != nullptr);
        std::string v(1024 * 1024, '1');
        // normal
        std::size_t send_cnt = 0;
        while (true) {
            if (!stream->Write(v, 5000)) break;
            ++send_cnt;
        }
        stream->WriteDone();
        EXPECT_TRUE(stream->Finish(0));
    }
}

TEST(rpc_test, test_proxy_reconnect) {
    network::proxy::rpc_forward_connection::kMaxReconnectCnts           = 2;
    network::proxy::rpc_forward_connection::kReconnectRetryIntervalMsec = 10;
    Fundamental::ScopeGuard g([&]() {
        network::proxy::rpc_forward_connection::kMaxReconnectCnts = 0;
        network::proxy::rpc_forward_connection::kReconnectRetryIntervalMsec =
            network::proxy::rpc_forward_connection::kDefaultReconnectRetryIntervalMsec;
    });
    auto client     = network::make_guard<rpc_client>();
    auto ws_upgrade = proxy::ws_upgrade_imp::make_shared("/ws_proxy9001", "127.0.0.1");
    client->append_proxy(ws_upgrade);
    [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
    EXPECT_FALSE(r && client->has_connected());
}
TEST(rpc_test, test_buffer_limit) {
    std::string buffer1(128 * 1024 * 1024, 'c');
    {
        auto client = network::make_guard<rpc_client>();
        client->set_proxy(gen_pipe_proxy());
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");

        EXPECT_TRUE(r && client->has_connected());

        {
            std::string ret;
            try {
                ret = client->call<5000, std::string>("echo", buffer1);
            } catch (const std::exception& e) {
                FERR("exception {}", e.what());
            }
            EXPECT_TRUE(ret == buffer1);
        }
    }
    {
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");
        client->set_proxy(gen_pipe_proxy());
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        EXPECT_TRUE(stream != nullptr);
        EXPECT_TRUE(stream->Write(buffer1));

        std::string tmp;
        EXPECT_TRUE(stream->Read(tmp));
        EXPECT_TRUE(tmp == buffer1);
        EXPECT_TRUE(stream->WriteDone());
        EXPECT_TRUE(!stream->Finish(0));
    }
}

TEST(rpc_test, test_speed_limit) {
    std::string buffer1(1024, 'c');
    { // no limit send read
        auto client = network::make_guard<rpc_client>();
        client->set_proxy(gen_pipe_proxy());
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");

        EXPECT_TRUE(r && client->has_connected());

        {
            std::string ret;
            try {
                ret = client->call<100, std::string>("echo", buffer1);
            } catch (const std::exception& e) {
                FERR("exception {}", e.what());
            }
            EXPECT_TRUE(ret == buffer1);
        }
    }
    { //  limit send read
        network::proxy::rpc_forward_connection::kForwardSpeedLimitRateBytesPerSec = 100;
        auto client                                                               = network::make_guard<rpc_client>();
        client->set_proxy(gen_pipe_proxy());
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9000");

        EXPECT_TRUE(r && client->has_connected());

        {
            std::string ret;
            try {
                ret = client->call<100, std::string>("echo", buffer1);
            } catch (const std::exception& e) {
                FERR("exception {}", e.what());
            }
            EXPECT_TRUE(ret != buffer1);
        }
        network::proxy::rpc_forward_connection::kForwardSpeedLimitRateBytesPerSec = 0;
    }
}

TEST(rpc_test, test_port_proxy) {
    std::string buffer1(1024, 'c');
    {//test ws port proxy with ssl
        rpc_client_forward_config forward_config;
        forward_config.ssl_config.ca_certificate_path = "ca_root.crt";
        forward_config.ssl_config.private_key_path    = "local.key";
        forward_config.ssl_config.certificate_path    = "local.crt";
        forward_config.ssl_config.disable_ssl         = false;
        auto server                                   = proxy::ws_port_pipe_server::make_shared(9001);
        server->set_forward_config(forward_config, "127.0.0.1", "9000", "/ws_proxy");
        server->start();
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9001");
        client->set_proxy(gen_pipe_proxy());
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        std::string response;
        EXPECT_TRUE(stream->Write(buffer1, 200));
        EXPECT_TRUE(stream->Read(response, 200));
        EXPECT_EQ(buffer1, response);
        server->stop();
        stream->WriteDone();
        auto ec = stream->Finish(0);
        FINFO("result v:{}", ec.message());
        EXPECT_TRUE(ec.value() != 0);
    }
    {//test ws port proxy with none ssl
        rpc_client_forward_config forward_config;
        forward_config.ssl_config.disable_ssl = true;
        auto server                           = proxy::ws_port_pipe_server::make_shared(9001);
        server->set_forward_config(forward_config, "127.0.0.1", "9000", "/ws_proxy");
        server->start();
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9001");
        client->set_proxy(gen_pipe_proxy());
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        std::string response;
        EXPECT_TRUE(stream->Write(buffer1, 200));
        EXPECT_TRUE(stream->Read(response, 200));
        EXPECT_EQ(buffer1, response);
        server->stop();
        stream->WriteDone();
        auto ec = stream->Finish(0);
        FINFO("result v:{}",ec.message());
        EXPECT_TRUE(ec.value() != 0);
    }
        {//test pipe port proxy with ssl
        rpc_client_forward_config forward_config;
        forward_config.ssl_config.ca_certificate_path = "ca_root.crt";
        forward_config.ssl_config.private_key_path    = "local.key";
        forward_config.ssl_config.certificate_path    = "local.crt";
        forward_config.ssl_config.disable_ssl         = false;
        auto server                                   = proxy::ws_port_pipe_server::make_shared(9001);
        server->set_forward_config(forward_config, "127.0.0.1", "9000", "/ws_proxy","127.0.0.1", "9000");
        server->start();
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9001");
        client->set_proxy(gen_pipe_proxy());
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        std::string response;
        EXPECT_TRUE(stream->Write(buffer1, 200));
        EXPECT_TRUE(stream->Read(response, 200));
        EXPECT_EQ(buffer1, response);
        server->stop();
        stream->WriteDone();
        auto ec = stream->Finish(0);
        FINFO("result v:{}", ec.message());
        EXPECT_TRUE(ec.value() != 0);
    }
    {//test pipe port proxy with none ssl
        rpc_client_forward_config forward_config;
        forward_config.ssl_config.disable_ssl = true;
        auto server                           = proxy::ws_port_pipe_server::make_shared(9001);
        server->set_forward_config(forward_config, "127.0.0.1", "9000", "/ws_proxy","127.0.0.1", "9000");
        server->start();
        auto client             = network::make_guard<rpc_client>();
        [[maybe_unused]] bool r = client->connect("127.0.0.1", "9001");
        client->set_proxy(gen_pipe_proxy());
        EXPECT_TRUE(r && client->has_connected());
        auto stream = client->upgrade_to_stream("test_echo_stream");
        std::string response;
        EXPECT_TRUE(stream->Write(buffer1, 200));
        EXPECT_TRUE(stream->Read(response, 200));
        EXPECT_EQ(buffer1, response);
        server->stop();
        stream->WriteDone();
        auto ec = stream->Finish(0);
        FINFO("result v:{}",ec.message());
        EXPECT_TRUE(ec.value() != 0);
    }
}
#endif


int main(int argc, char** argv) {
    int mode = 0;
    if (argc > 1) mode = std::stoi(argv[1]);

    Fundamental::fs::SwitchToProgramDir(argv[0]);
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::debug;
    options.logFormat    = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    Fundamental::Logger::Initialize(std::move(options));
    s_test_pool.InitThreadPool(Fundamental::ThreadPoolConfig { false });
    s_test_pool.Spawn(4);
    if (mode == 0) {
        ::testing::InitGoogleTest(&argc, argv);
        run_server();
        auto ret = RUN_ALL_TESTS();
        exit_server();
        FINFO("finish all test");
        return ret;
    } else if (mode == 1) {
        std::promise<void> sync_p;
        server_task(sync_p);
        sync_p.get_future().get();
        exit_server();
    } else {
        ::testing::InitGoogleTest(&argc, argv);
        network::io_context_pool::s_excutorNums = 10;
        network::io_context_pool::Instance().start();

        return RUN_ALL_TESTS();
    }
    return 0;
}
