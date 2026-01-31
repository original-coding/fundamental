

#include "network/network.hpp"
#include "network/rudp/asio_rudp.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "fundamental/application/application.hpp"
#include "fundamental/basic/arg_parser.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"

using namespace network;

using namespace network::rudp;

struct rudp_acceptor : public std::enable_shared_from_this<rudp_acceptor> {
    rudp_acceptor(rudp_handle_t& handle) : rudp_handle(handle) {
    }

    void start(std::function<void(rudp_handle_t)> new_connection__cb) {
        cb = new_connection__cb;
        process_accept();
    }

    void process_accept() {
        if (!*rudp_handle) return;
        async_rudp_accept(rudp_handle, network::io_context_pool::Instance().get_io_context(),
                          [this, ref = weak_from_this()](rudp_handle_t handle, Fundamental::error_code ec) {
                              auto strong = ref.lock();
                              if (!strong) return;
                              if (cb) cb(handle);
                              process_accept();
                          });
    }
    std::atomic<std::size_t> max_size = 0;
    rudp_handle_t& rudp_handle;
    std::function<void(rudp_handle_t)> cb;
};

struct rudp_server_session : public std::enable_shared_from_this<rudp_server_session> {
    static void start_new_session(rudp_handle_t handle) {
        auto session    = std::make_shared<rudp_server_session>();
        session->handle = handle;
        session->read_cache.resize(32 * 1024);
        network::io_context_pool::Instance().notify_sys_signal.Connect(
            [ref = session->weak_from_this()](Fundamental::error_code, int) {
                auto s = ref.lock();
                if (!s) return;
                s->handle->destroy();
            });
        session->perform_read();
    }
    void echo_data(std::size_t read_size) {
        async_rudp_send(handle, read_cache.data(), read_size, [](std::size_t, Fundamental::error_code) {
            // just do nothing
        });
    }
    void perform_read() {
        async_rudp_recv(handle, read_cache.data(), read_cache.size(),
                        [this, ref = shared_from_this()](std::size_t len, Fundamental::error_code ec) {
                            if (len > 0) {
                                echo_data(len);
                                perform_read();
                            }
                        });
    }
    rudp_handle_t handle;
    std::vector<std::uint8_t> read_cache;
};

int main(int argc, char* argv[]) {
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::debug;
    options.logFormat    = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    Fundamental::Logger::Initialize(std::move(options));
    Fundamental::arg_parser arg_parser { argc, argv, "1.0.1" };
    std::size_t mode            = 0;
    std::size_t port            = 32000;
    std::string host            = "::";
    std::size_t remote_port     = 32000;
    std::string remote_host     = "127.0.0.1";
    std::size_t mtu_size        = 1400;
    std::size_t window_size     = 512;
    std::size_t interval        = 10;
    std::size_t fast_resend_cnt = 2;
    std::size_t cnt             = 4096;
    std::size_t group_size      = 16;

    bool diable_no_cwnd_control = false;
    bool disable_no_delay       = false;

    arg_parser.AddOption("mode", Fundamental::StringFormat("workmode default:{}", mode), 'm',
                         Fundamental::arg_parser::param_type::required_param,
                         "0:multi server 1:client 2:single server");
    arg_parser.AddOption("port", Fundamental::StringFormat("local bind port default:{}", port), 'p',
                         Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("host", Fundamental::StringFormat("local bind host default:{}", host), 'b',
                         Fundamental::arg_parser::param_type::required_param, "domain");
    arg_parser.AddOption("remote_port",
                         Fundamental::StringFormat("connect remote port,just for client mode default:{}", remote_port),
                         'P', Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("remote_host",
                         Fundamental::StringFormat("connect remote host,just for client mode default:{}", remote_host),
                         'B', Fundamental::arg_parser::param_type::required_param, "domain");
    arg_parser.AddOption("mtu_size", Fundamental::StringFormat("network mtu size default:{}", mtu_size), 'M',
                         Fundamental::arg_parser::param_type::required_param, "number [32-32968]");
    arg_parser.AddOption("window_size",
                         Fundamental::StringFormat("rudp cache window size(fragment's nums) default:{}", window_size),
                         'W', Fundamental::arg_parser::param_type::required_param, "number [32-32968]");
    arg_parser.AddOption("interval", Fundamental::StringFormat("rudp update status interval ms default:{}", interval),
                         'i', Fundamental::arg_parser::param_type::required_param, "number [1-5000]");
    arg_parser.AddOption("group_size", Fundamental::StringFormat("rudp send group size ms default:{}", group_size), 'g',
                         Fundamental::arg_parser::param_type::required_param, "number [1-window_size]");
    arg_parser.AddOption(
        "fast_resend_cnt",
        Fundamental::StringFormat("rudp fast resend fragment when it has lost default:{}", fast_resend_cnt), 'R',
        Fundamental::arg_parser::param_type::required_param, "number [0-10]");
    arg_parser.AddOption("cnt", Fundamental::StringFormat("test frame nums ,just for client mode default:{}", cnt), 'I',
                         Fundamental::arg_parser::param_type::required_param, "number");

    arg_parser.AddOption("diable_no_cwnd_control",
                         Fundamental::StringFormat("congestion control switch default:{}", diable_no_cwnd_control), -1,
                         Fundamental::arg_parser::param_type::with_none_param);
    arg_parser.AddOption(
        "disable_no_delay",
        Fundamental::StringFormat("no delay switch which affects the min_rto default:{}", disable_no_delay), -1,
        Fundamental::arg_parser::param_type::with_none_param);
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
    // sys config
    {
        mtu_size               = arg_parser.GetValue("mtu_size", mtu_size);
        window_size            = arg_parser.GetValue("window_size", window_size);
        interval               = arg_parser.GetValue("interval", interval);
        fast_resend_cnt        = arg_parser.GetValue("fast_resend_cnt", fast_resend_cnt);
        diable_no_cwnd_control = arg_parser.HasParam("diable_no_cwnd_control");
        disable_no_delay       = arg_parser.HasParam("disable_no_delay");
        rudp_config_sys(rudp_config_type::RUDP_MTU_SIZE, mtu_size);
        rudp_config_sys(rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 0);
        rudp_config_sys(rudp_config_type::RUDP_MAX_RECV_WINDOW_SIZE, window_size);
        rudp_config_sys(rudp_config_type::RUDP_MAX_SEND_WINDOW_SIZE, window_size);
        rudp_config_sys(rudp_config_type::RUDP_UPDATE_INTERVAL_MS, interval);
        rudp_config_sys(rudp_config_type::RUDP_FASK_RESEND_SKIP_CNT, fast_resend_cnt);
        rudp_config_sys(rudp_config_type::RUDP_ENABLE_NO_CONGESTION_CONTROL, diable_no_cwnd_control ? 0 : 1);
        rudp_config_sys(rudp_config_type::RUDP_ENABLE_NO_DELAY, disable_no_delay ? 0 : 1);
    }

    mode = arg_parser.GetValue("mode", mode);
    port = arg_parser.GetValue("port", port);
    host = arg_parser.GetValue("host", host);
    network::io_context_pool::Instance().start();
    if (mode == 0) {
        FINFO("start rudp server on [{}] {}", host, port);
        Fundamental::error_code ec;
        asio::io_context ios;
        auto server_handler = rudp_create(ios, ec);
        if (ec) {
            FERR("{}", ec);
            return -1;
        }
        rudp_bind(server_handler, port, host, ec);
        if (ec) {
            FERR("{}", ec);
            return -1;
        }
        std::size_t max_connections = 1000;
        rudp_listen(server_handler, max_connections, ec);
        if (ec) {
            FERR("{}", ec);
            return -1;
        }
        auto acceptor                 = std::make_shared<rudp_acceptor>(server_handler);
        acceptor->max_size            = max_connections;
        std::atomic<std::int32_t> cnt = 0;
        acceptor->start([](rudp_handle_t handle) { rudp_server_session::start_new_session(handle); });
        network::io_context_pool::Instance().notify_sys_signal.Connect(
            [&, server_handler](Fundamental::error_code, int) {
                if (acceptor) acceptor.reset();
                server_handler->destroy();
            });
        ios.run();

    } else if (mode == 2) {
        FINFO("start rudp single server on [{}] {}", host, port);
        Fundamental::error_code ec;
        asio::io_context ios;
        auto server_handler = rudp_create(ios, ec);
        if (ec) {
            FERR("{}", ec);
            return -1;
        }
        rudp_bind(server_handler, port, host, ec);
        if (ec) {
            FERR("{}", ec);
            return -1;
        }
        async_rudp_wait_connect(
            server_handler,
            [&](Fundamental::error_code ec) {
                if (ec) {
                    FERR("unique server wait connection failed {}", ec);
                    return;
                }
                FINFO("wait conenction success");
                rudp_server_session::start_new_session(server_handler);
            },
            30000);
        ios.run();
    } else {
        remote_port = arg_parser.GetValue("remote_port", remote_port);
        remote_host = arg_parser.GetValue("remote_host", remote_host);
        cnt         = arg_parser.GetValue("cnt", cnt);
        group_size  = arg_parser.GetValue("group_size", group_size);
        if (port == remote_port) port = remote_port + 1;
        FINFO("start rudp client cnt:{} group_size:{} on [{}] {}  remote {} {}", cnt, group_size, host, port,
              remote_host, remote_port);
        Fundamental::error_code ec;
        auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);

        rudp_bind(client_handler, port, host, ec);
        std::promise<void> connect_promise;
        async_rudp_connect(client_handler, remote_host, remote_port, [&](Fundamental::error_code e) {
            if (e)
                connect_promise.set_exception(e.make_exception_ptr());
            else
                connect_promise.set_value();
        });
        try {
            connect_promise.get_future().wait();
        } catch (const std::exception& e) {
            FERR("connect failed for {}", e.what());
            return -1;
        }
        network::io_context_pool::Instance().notify_sys_signal.Connect(
            [&, client_handler](Fundamental::error_code, int) { client_handler->destroy(); });
        std::size_t data_size             = (mtu_size - kRudpProtocalHeadSize) * group_size;
        std::atomic<std::size_t> recv_cnt = 0;
        auto send_task_token              = Fundamental::ThreadPool::DefaultPool().Enqueue([&]() {
            std::string send_data(data_size, 'c');
            std::promise<void> send_promise;
            std::size_t finish_cnt      = 0;
            std::int64_t max_delay_ms   = 0;
            std::int64_t total_delay_ms = 0;
            std::size_t max_delay_sn    = 0;
            auto loop_func              = [&](auto self, std::size_t left_times) {
                if (left_times == 0) {
                    send_promise.set_value();
                    return;
                }
                --left_times;
                auto now = Fundamental::Timer::GetTimeNow();
                async_rudp_send(
                    client_handler, send_data.data(), send_data.size(),
                    [&, current_times = left_times, self, now](std::size_t len, Fundamental::error_code ec) {
                        if (ec) {
                            send_promise.set_value();
                        } else {
                            ++finish_cnt;
                            auto cost_ms          = Fundamental::Timer::GetTimeNow() - now;
                            auto recv_cnt_current = recv_cnt.load();
                            if (recv_cnt_current < finish_cnt) {
                                auto delay_cnt = finish_cnt - recv_cnt_current;
                                max_delay_sn   = max_delay_sn > delay_cnt ? max_delay_sn : delay_cnt;
                            }
                            total_delay_ms += cost_ms;
                            max_delay_ms = max_delay_ms > cost_ms ? max_delay_ms : cost_ms;
                            self(self, current_times);
                        }
                    });
            };
            loop_func(loop_func, cnt);
            send_promise.get_future().get();
            FINFO("send {} size:{} chunks cost {} ms [speed {} MB/s] max rtt time:{} ms max recv delay sn:{}",
                               finish_cnt, data_size, total_delay_ms,
                               data_size * finish_cnt * 1000 / (1024.0 * 1024.0 * total_delay_ms), max_delay_ms, max_delay_sn);
        });
        auto recv_task_token              = Fundamental::ThreadPool::DefaultPool().Enqueue([&]() {
            std::string recv_data;
            recv_data.resize(data_size);
            std::promise<void> recv_promise;
            auto loop_func = [&](auto self, std::size_t left_times) {
                if (left_times == 0) {
                    recv_promise.set_value();
                    return;
                }
                --left_times;
                async_rudp_recv(client_handler, recv_data.data(), recv_data.size(),
                                             [&, current_times = left_times, self](std::size_t, Fundamental::error_code ec) {
                                    if (ec) {
                                        recv_promise.set_value();
                                    } else {
                                        recv_cnt.fetch_add(1);
                                        self(self, current_times);
                                    }
                                });
            };
            loop_func(loop_func, cnt);
            recv_promise.get_future().get();
        });
        send_task_token.resultFuture.wait();
        recv_task_token.resultFuture.wait();
    }
    return 0;
}