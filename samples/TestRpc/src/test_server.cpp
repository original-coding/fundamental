#include "test_server.h"

#include "rpc/proxy/rpc_forward_connection.hpp"
#include "rpc/rpc_server.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <rttr/registration>
#include <thread>

#include "fundamental/application/application.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/random_generator.hpp"
#include "fundamental/delay_queue/delay_queue.h"
#include "fundamental/thread_pool/thread_pool.h"

using namespace network;
using namespace rpc_service;

static auto& rpc_stream_pool = Fundamental::ThreadPool::Instance<100>();

RTTR_REGISTRATION {
    rttr::registration::class_<person>("person")
        .constructor()(rttr::policy::ctor::as_object)
        .property("id", &person::id)
        .property("age", &person::age)
        .property("name", &person::name);
    rttr::registration::class_<dummy1>("dummy1")
        .constructor()(rttr::policy::ctor::as_object)
        .property("id", &dummy1::id)
        .property("str", &dummy1::str);
    rttr::registration::class_<TestProxyRequest>("TestProxyRequest")
        .constructor()(rttr::policy::ctor::as_object)
        .property("f", &TestProxyRequest::f)
        .property("v", &TestProxyRequest::v)
        .property("strs", &TestProxyRequest::strs);
    rttr::registration::class_<DelayControlStream>("DelayControlStream")
        .constructor()(rttr::policy::ctor::as_object)
        .property("1", &DelayControlStream::cmd)
        .property("2", &DelayControlStream::msg)
        .property("3", &DelayControlStream::process_delay);
}

struct dummy {
    int add(rpc_conn conn, int a, int b) {
        return a + b;
    }
};

static Fundamental::DelayQueue& s_delay_queue = *(Fundamental::Application::Instance().DelayQueue());

std::string translate(rpc_conn conn, const std::string& orignal) {
    std::string temp = orignal;
    for (auto& c : temp) {
        c = static_cast<char>(std::toupper(c));
    }
    return temp;
}

void hello(rpc_conn conn, const std::string& str) {
    auto ptr = conn.lock();
    FINFO("hello from client [{}:{}]:{}", ptr->get_remote_peer_ip(), ptr->get_remote_peer_port(), str);
}

std::string get_person_name(rpc_conn conn, const person& p) {
    return p.name;
}

person get_person(rpc_conn conn) {
    return { 1, "tom", 20 };
}

void upload(rpc_conn conn, const std::string& filename, const std::string& content) {
    std::ofstream file(filename, std::ios::binary);
    file.write(content.data(), content.size());
}

std::string download(rpc_conn conn, const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        return "";
    }

    file.seekg(0, std::ios::end);
    size_t file_len = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string content;
    content.resize(file_len);
    file.read(&content[0], file_len);

    return content;
}

std::string get_name(rpc_conn conn, const person& p) {
    return p.name;
}

void auto_disconnect(rpc_conn conn, std::int32_t v) {
    FINFOS << "auto_disconnect " << v;
    conn.lock()->release_obj();
}

// if you want to response later, you can use async model, you can control when
// to response
void delay_echo(rpc_conn conn, const std::string& src, std::int64_t delay_msec) {
    FINFO("delay echo request {}:{} ", delay_msec, src.substr(0, src.size() > 100 ? 100 : src.size()));
    auto sp = conn.lock();
    sp->set_delay(true);
    auto req_id = sp->request_id(); // note: you need keep the request id at that

    auto h = s_delay_queue.AddDelayTask(
        delay_msec,
        [conn, req_id, src] {
            auto conn_sp = conn.lock();
            if (conn_sp) {
                FINFO("delay echo response {} ", src.substr(0, src.size() > 100 ? 100 : src.size()));
                conn_sp->response(req_id, network::rpc_service::request_type::rpc_res, std::move(src));
            }
        },
        true);
    s_delay_queue.StartDelayTask(h);
}

void test_stream(std::shared_ptr<ServerStreamReadWriter> stream) {
    FWARN("stream start");
    person p;
    while (stream->Read(p, 0)) {
        FDEBUG("id:{},age:{},name:{}", p.id, p.age, p.name);
        p.name += " from server";
        p.age += 10;
        p.id += 10;
        if (!stream->Write(p)) break;
    };
    stream->WriteDone();
    auto ec = stream->Finish(0);
    if (ec) {
        FINFO("rpc failed {}", ec.message());
    } else {
        FINFO("rpc done");
    }
}

void void_stream(rpc_conn conn) {
    auto c = conn.lock();
    auto w = c->InitRpcStream();

    rpc_stream_pool.Enqueue(
        [](decltype(w) stream) {
            FWARN("void stream start");
            person p;
            while (stream->ReadEmpty(0)) {
                if (!stream->WriteEmpty()) break;
            };
            stream->WriteDone();
            auto ec = stream->Finish(0);
            if (ec) {
                FINFO("rpc void_stream failed {}", ec.message());
            } else {
                FINFO("rpc void_stream done");
            }
        },
        w);
}

void test_read_stream(rpc_conn conn) {
    auto c = conn.lock();
    auto w = c->InitRpcStream();

    rpc_stream_pool.Enqueue(
        [](decltype(w) stream) {
            person p;
            p.id   = 0;
            p.age  = 1;
            p.name = "111";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stream->Write(p);
            stream->WriteDone();
            auto ec = stream->Finish(0);
            if (ec) {
                FINFO("rpc failed {}", ec.message());
            } else {
                FINFO("rpc done");
            }
        },
        w);
}

void test_write_stream(rpc_conn conn) {
    auto c = conn.lock();
    auto w = c->InitRpcStream();

    rpc_stream_pool.Enqueue(
        [](decltype(w) stream) {
            person p;
            stream->WriteDone();
            while (stream->Read(p, 0)) {
                FINFO("write {} {} {}", p.id, p.age, p.name);
            }

            auto ec = stream->Finish(0);
            if (ec) {
                FINFO("rpc failed {}", ec.message());
            } else {
                FINFO("rpc done");
            }
        },
        w);
}

void test_broken_stream(rpc_conn conn) {
    auto c = conn.lock();
    auto w = c->InitRpcStream();

    rpc_stream_pool.Enqueue(
        [](decltype(w) stream) {
            // just release
            stream->release_obj();
        },
        w);
}
void test_abort_stream(rpc_conn conn) {
    auto c = conn.lock();
    auto w = c->InitRpcStream();

    rpc_stream_pool.Enqueue(
        [](decltype(w) stream) {
            bool aborted = false;
            std::mutex lock;
            std::condition_variable cv;
            stream->notify_stream_abort.Connect([&]() {
                FERR("stream aborted");
                std::scoped_lock<std::mutex> locker(lock);
                aborted = true;
                cv.notify_one();
            });
            stream->EnableTimeoutCheck(1000);
            stream->WriteDone();
            auto g   = Fundamental::DefaultNumberGenerator<std::size_t>(0, 10);
            auto cnt = g();
            if (cnt < 5) {
                stream->release_obj();
                return;
            }
            FDEBUG("start abort stream wait");

            { // block wait connection disconneted
                std::unique_lock<std::mutex> locker(lock);
                while (!aborted) {
                    cv.wait_for(locker, std::chrono::milliseconds(10));
                }
            }
            FDEBUG("finish abort stream wait");
            stream->Finish(0);
        },
        w);
}

void test_echo_stream(rpc_conn conn) {
    auto c  = conn.lock();
    auto w  = c->InitRpcStream();
    auto id = c->conn_id();
    FWARN("try start test_echo_stream");
    rpc_stream_pool.Enqueue(
        [id](decltype(w) stream) {
            FWARN("test_echo_stream start {} {}", stream->get_remote_peer_ip(), stream->get_remote_peer_port());
            std::string msg;
            while (stream->Read(msg, 5000)) {
                if (!stream->Write(msg)) break;
            };
            stream->WriteDone();
            auto ec = stream->Finish(5000);
            if (ec) {
                FINFO("rpc failed {} {}", id, ec.message());
            } else {
                FINFO("rpc done {}", id);
            }
        },
        w);
}

void test_echo_delay_limit_stream(rpc_conn conn) {
    auto c  = conn.lock();
    auto w  = c->InitRpcStream();
    auto id = c->conn_id();
    FWARN("try start test_echo_delay_limit_stream");
    rpc_stream_pool.Enqueue(
        [id](decltype(w) stream) {
            FWARN("test_echo_delay_limit_stream start {} {}", stream->get_remote_peer_ip(),
                  stream->get_remote_peer_port());
            std::string msg;
            std::size_t echo_cnt = 0;
            while (stream->Read(msg, 5000)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (!stream->Write(msg, 5000)) break;
                ++echo_cnt;
            };
            stream->WriteDone();
            auto ec = stream->Finish(5000);
            if (ec) {
                FINFO("rpc failed {} {}", id, ec.message());
            } else {
                FINFO("rpc done {}", id);
            }
        },
        w);
}

void test_control_stream(rpc_conn conn) {
    auto c = conn.lock();
    auto w = c->InitRpcStream();
    c->set_tcp_no_delay();
    rpc_stream_pool.Enqueue(
        [](decltype(w) stream) {
            DelayControlStream request;

            while (stream->Read(request, 0)) {
                FDEBUG("test_control_stream request {} {} msg size:{}", request.cmd, request.process_delay,
                       request.msg.size());
                if (request.cmd == "set") {
                    stream->EnableTimeoutCheck(request.process_delay);
                } else if (request.cmd == "echo") {
                    if (request.process_delay > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(request.process_delay));
                    FDEBUG("finish test_control_stream request {} {}", request.cmd, request.process_delay);
                    stream->Write(request.msg);
                } else {
                    break;
                }
            }
            stream->WriteDone();
            auto ec = stream->Finish(0);
            if (ec) {
                FINFO("test_control_stream rpc failed {}", ec.message());
            } else {
                FINFO("test_control_stream rpc done");
            }
        },
        w);
}

std::string echo(rpc_conn conn, const std::string& src) {
    return src;
}

dummy1 get_dummy(rpc_conn conn, dummy1 d) {
    return d;
}
template <typename ObjType>
ObjType object_echo(rpc_conn conn, ObjType d) {
    return d;
}

std::unique_ptr<std::thread> s_thread;
static network::proxy::ProxyManager s_manager;
void server_task(std::promise<void>& sync_p) {
    network::rpc_service::connection::sRpcStreamReadQueueLimitSize  = 1;
    network::rpc_service::connection::sRpcStreamWriteQueueLimitSize = 1;
    network::proxy::rpc_forward_connection::kMaxReconnectCnts       = 0;
    auto s_server                                                   = network::make_guard<rpc_server>(9000);
    auto p                                                          = s_server.get();
    auto& server                                                    = *s_server.get();
    network::network_server_ssl_config ssl_config;
    ssl_config.ca_certificate_path = "ca_root.crt";
    ssl_config.disable_ssl         = false;
    ssl_config.private_key_path    = "local.key";
    ssl_config.certificate_path    = "local.crt";
    ssl_config.verify_client       = ::getenv("verify_client") != nullptr;
    ssl_config.enable_no_ssl       = ::getenv("disable_no_ssl") == nullptr;
    server.enable_ssl(ssl_config);

    network::rpc_server_external_config external_config;
    external_config.forward_config.ssl_config.ca_certificate_path = ssl_config.ca_certificate_path;
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
    //external_config.proxy_speed_limit_bytes_per_second            = 300;
    server.set_external_config(external_config);
    dummy d;
    server.register_delay_handler("add", [&d](rpc_conn conn, int x, int y) -> int { return d.add(conn, x, y); });

    server.register_delay_handler("get_dummy", get_dummy);

    server.register_delay_handler("translate", translate);
    server.register_delay_handler("hello", hello);
    server.register_delay_handler("get_person_name", get_person_name);
    server.register_delay_handler("get_person", get_person);
    server.register_delay_handler("upload", upload);
    server.register_delay_handler("download", download);
    server.register_delay_handler("get_name", get_name);
    server.register_handler("delay_echo", delay_echo);
    server.register_delay_handler("echo", echo);
    server.register_handler("auto_disconnect", auto_disconnect);
    server.register_stream_handler("test_stream", test_stream);
    server.register_handler("test_void_stream", void_stream);
    server.register_handler("test_read_stream", test_read_stream);
    server.register_handler("test_write_stream", test_write_stream);
    server.register_handler("test_broken_stream", test_broken_stream);
    server.register_handler("test_echo_stream", test_echo_stream);
    server.register_handler("test_echo_delay_limit_stream", test_echo_delay_limit_stream);
    server.register_handler("test_abort_stream", test_abort_stream);
    server.register_handler("object_echo<int>", object_echo<int>);
    server.register_handler("object_echo<TestProxyRequest>", object_echo<TestProxyRequest>);
    server.register_handler("test_control_stream", test_control_stream);
    server.on_net_err.Connect([](std::shared_ptr<connection> conn, std::string reason) {
        std::cout << "remote client address: " << conn->remote_address() << " networking error, reason: " << reason
                  << "\n";
    });
    auto time_queue = Fundamental::Application::Instance().DelayQueue();
    auto h          = time_queue->AddDelayTask(10, [s_server = s_server.get()] {
        person p { 10, "jack_server", 21 };
        s_server.get()->publish("key", "publish msg from server");
        s_server->publish("key_p", p);
    });
    time_queue->StartDelayTask(h);
    network::init_io_context_pool(10);
    {
        using namespace network::proxy;
        auto& manager = s_manager;
        manager.AddWsProxyRoute("/ws_proxy_baidu", proxy::ProxyHost { "www.baidu.com", "http" });
        manager.AddWsProxyRoute("/ws_proxy", proxy::ProxyHost { "127.0.0.1", "9000" });
        manager.AddWsProxyRoute("/ws_proxy9001", proxy::ProxyHost { "127.0.0.1", "9001" });
        manager.AddWsProxyRoute("/ws_remove_test", proxy::ProxyHost { "www.baidu.com", "http", nullptr, true });

        proxy::ProxyHost test_host;
        test_host.host      = "127.0.0.1";
        test_host.service   = "9000";
        test_host.host_type = proxy::proxy_host_type::ws_forward_proxy;
        manager.AddWsProxyRoute("/ws_proxy_next_layer", test_host);

        manager.AddMatchPrefixPath("/nginx");
        manager.AddMatchPrefixPath("/nginx/test_remove//");
    }
    server.enable_data_proxy(&s_manager);
    server.enable_socks5_proxy(SocksV5::Sock5Handler::make_default_handler());
    server.start();
    rpc_stream_pool.Spawn(5);
    sync_p.set_value();
    Fundamental::Application::Instance().exitStarted.Connect(
        [h, time_queue]() mutable { time_queue->StopDelayTask(h); }, false);
    Fundamental::Application::Instance().Loop();
    FDEBUG("finish loop");
}

void run_server() {
    std::promise<void> sync_p;
    s_thread = std::make_unique<std::thread>([&]() { server_task(sync_p); });
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
