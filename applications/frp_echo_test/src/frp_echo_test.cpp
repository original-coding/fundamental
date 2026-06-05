#include "fundamental/application/application.hpp"
#include "fundamental/basic/arg_parser.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/random_generator.hpp"
#include "network/io_context_pool.hpp"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

using asio::ip::tcp;
using asio::ip::udp;

// ---------------------------------------------------------------------------
// TCP Echo server
// ---------------------------------------------------------------------------

class echo_session : public std::enable_shared_from_this<echo_session> {
public:
    explicit echo_session(tcp::socket socket) : socket_(std::move(socket)) {}

    void start() { do_read(); }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(buf_),
            [this, self](std::error_code ec, std::size_t n) {
                if (ec) return;
                do_write(n);
            });
    }

    void do_write(std::size_t n) {
        auto self = shared_from_this();
        asio::async_write(
            socket_, asio::buffer(buf_.data(), n),
            [this, self](std::error_code ec, std::size_t) {
                if (ec) return;
                do_read();
            });
    }

    tcp::socket socket_;
    std::array<char, 65536> buf_{};
};

class echo_server {
public:
    echo_server(asio::io_context& ioc, std::uint16_t port)
        : acceptor_(ioc, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
            if (!ec) {
                FINFO("echo_server accepted connection from {}:{}",
                      socket.remote_endpoint().address().to_string(),
                      socket.remote_endpoint().port());
                std::make_shared<echo_session>(std::move(socket))->start();
            }
            do_accept();
        });
    }

    tcp::acceptor acceptor_;
};

// ---------------------------------------------------------------------------
// TCP Echo client
// ---------------------------------------------------------------------------

static void run_echo_client(const std::string& host,
                             std::uint16_t port,
                             std::size_t count,
                             std::size_t delay_ms) {
    auto size_gen = Fundamental::DefaultNumberGenerator<std::size_t>(64, 4096);
    auto byte_gen = Fundamental::DefaultNumberGenerator<std::uint8_t>();

    std::size_t passed = 0;
    std::size_t failed = 0;

    try {
        asio::io_context ioc;
        tcp::socket sock(ioc);
        tcp::resolver resolver(ioc);
        auto endpoints = resolver.resolve(host, std::to_string(port));
        asio::connect(sock, endpoints);

        for (std::size_t i = 0; i < count; ++i) {
            if (i > 0 && delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }

            // generate random payload
            std::size_t payload_size = size_gen();
            std::string send_buf(payload_size, '\0');
            byte_gen.gen(reinterpret_cast<std::uint8_t*>(send_buf.data()), payload_size);

            // send
            asio::write(sock, asio::buffer(send_buf));

            // recv echo
            std::string recv_buf(payload_size, '\0');
            asio::read(sock, asio::buffer(recv_buf.data(), payload_size));

            if (recv_buf == send_buf) {
                FINFO("echo_client round={}/{} size={} PASS", i + 1, count, payload_size);
                ++passed;
            } else {
                FERR("echo_client round={}/{} size={} FAIL (data mismatch)", i + 1, count, payload_size);
                ++failed;
            }
        }
    } catch (const std::exception& e) {
        FERR("echo_client FAIL ({})", e.what());
        failed = count;
    }

    FINFO("echo_client done: total={} passed={} failed={}", count, passed, failed);
    if (failed == 0) {
        fmt::print("\n[TEST PASSED] all {} rounds echo ok\n\n", count);
    } else {
        fmt::print("\n[TEST FAILED] {}/{} rounds failed\n\n", failed, count);
    }
}

// ---------------------------------------------------------------------------
// UDP Echo server
// ---------------------------------------------------------------------------

class udp_echo_server {
public:
    udp_echo_server(asio::io_context& ioc, std::uint16_t port)
        : socket_(ioc, udp::endpoint(udp::v4(), port)) {
        FINFO("udp_echo_server listening on port {}", port);
        do_receive();
    }

private:
    void do_receive() {
        socket_.async_receive_from(
            asio::buffer(recv_buf_), remote_endpoint_,
            [this](std::error_code ec, std::size_t n) {
                if (ec) {
                    if (ec != asio::error::operation_aborted) {
                        FERR("udp_echo_server recv error: {}", ec.message());
                    }
                    return;
                }
                // Echo back to sender
                socket_.async_send_to(
                    asio::buffer(recv_buf_.data(), n), remote_endpoint_,
                    [this](std::error_code /*ec*/, std::size_t /*sent*/) {
                        do_receive();
                    });
            });
    }

    udp::socket socket_;
    udp::endpoint remote_endpoint_;
    std::array<char, 65536> recv_buf_{};
};

// ---------------------------------------------------------------------------
// UDP Echo client
// ---------------------------------------------------------------------------

static void run_udp_echo_client(const std::string& host,
                                 std::uint16_t port,
                                 std::size_t count,
                                 std::size_t delay_ms) {
    auto byte_gen = Fundamental::DefaultNumberGenerator<std::uint8_t>();

    std::size_t passed = 0;
    std::size_t failed = 0;

    try {
        asio::io_context ioc;
        udp::socket sock(ioc);
        sock.open(udp::v4());
        sock.bind(udp::endpoint(udp::v4(), 0));   // bind to a fixed ephemeral port

        udp::resolver resolver(ioc);
        auto endpoints = resolver.resolve(udp::v4(), host, std::to_string(port));
        auto server_ep = *endpoints.begin();

        for (std::size_t i = 0; i < count; ++i) {
            if (i > 0 && delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }

            // Generate a small random payload (keep under typical MTU)
            std::size_t payload_size = byte_gen() % 256 + 16;
            std::string send_buf(payload_size, '\0');
            byte_gen.gen(reinterpret_cast<std::uint8_t*>(send_buf.data()), payload_size);

            // Send
            std::error_code ec;
            sock.send_to(asio::buffer(send_buf), server_ep, 0, ec);
            if (ec) {
                FERR("udp_echo_client round={}/{} send FAIL ({})", i + 1, count, ec.message());
                ++failed;
                continue;
            }

            // Receive echo (blocking, 5s timeout via native socket option)
            std::array<char, 65536> recv_buf{};
            udp::endpoint sender_ep;
            std::size_t recv_len = 0;
            struct timeval tv;
            tv.tv_sec = 5; tv.tv_usec = 0;
            setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
            recv_len = sock.receive_from(asio::buffer(recv_buf), sender_ep, 0, ec);
            if (ec || recv_len == 0) {
                FERR("udp_echo_client round={}/{} recv TIMEOUT ec={}", i + 1, count, ec.message());
                ++failed;
                continue;
            }

            if (recv_len == payload_size &&
                std::memcmp(send_buf.data(), recv_buf.data(), payload_size) == 0) {
                FINFO("udp_echo_client round={}/{} size={} PASS", i + 1, count, payload_size);
                ++passed;
            } else {
                FERR("udp_echo_client round={}/{} size={} FAIL (data mismatch send={} recv={})",
                     i + 1, count, payload_size, recv_len);
                ++failed;
            }
        }
    } catch (const std::exception& e) {
        FERR("udp_echo_client FAIL ({})", e.what());
        failed = count;
    }

    FINFO("udp_echo_client done: total={} passed={} failed={}", count, passed, failed);
    if (failed == 0) {
        fmt::print("\n[TEST PASSED] all {} rounds udp echo ok\n\n", count);
    } else {
        fmt::print("\n[TEST FAILED] {}/{} rounds failed\n\n", failed, count);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::debug;
    options.logFormat    = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    Fundamental::Logger::Initialize(std::move(options));

    Fundamental::arg_parser arg_parser { argc, argv, "1.0.1" };

    std::string mode       = "server";
    std::string host       = "127.0.0.1";
    std::size_t port       = 19000;
    std::size_t count      = 10;
    std::size_t delay_ms   = 0;

    arg_parser.AddOption("mode",
        "run mode: server | client | udp-server | udp-client", 'm',
        Fundamental::arg_parser::param_type::required_param, "mode");
    arg_parser.AddOption("host",
        Fundamental::StringFormat("server host for client mode (default: {})", host), 'H',
        Fundamental::arg_parser::param_type::required_param, "host");
    arg_parser.AddOption("port",
        Fundamental::StringFormat("listen/connect port (default: {})", port), 'p',
        Fundamental::arg_parser::param_type::required_param, "port");
    arg_parser.AddOption("count",
        Fundamental::StringFormat("client: number of echo rounds (default: {})", count), 'n',
        Fundamental::arg_parser::param_type::required_param, "count");
    arg_parser.AddOption("delay",
        Fundamental::StringFormat("client: delay between rounds in ms (default: {})", delay_ms), 'd',
        Fundamental::arg_parser::param_type::required_param, "ms");

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

    mode     = arg_parser.GetValue("mode", mode);
    host     = arg_parser.GetValue("host", host);
    port     = arg_parser.GetValue("port", port);
    count    = arg_parser.GetValue("count", count);
    delay_ms = arg_parser.GetValue("delay", delay_ms);

    if (mode == "client") {
        run_echo_client(host, static_cast<std::uint16_t>(port), count, delay_ms);
        return 0;
    }

    if (mode == "udp-client") {
        run_udp_echo_client(host, static_cast<std::uint16_t>(port), count, delay_ms);
        return 0;
    }

    network::init_io_context_pool(2);
    auto& ioc = network::io_context_pool::Instance().get_io_context();

    if (mode == "udp-server") {
        udp_echo_server server(ioc, static_cast<std::uint16_t>(port));
        Fundamental::Application::Instance().Loop();
        Fundamental::Application::Instance().Exit();
        return 0;
    }

    // default: tcp server
    echo_server server(ioc, static_cast<std::uint16_t>(port));
    FINFO("echo_server listening on port {}", port);
    Fundamental::Application::Instance().Loop();
    Fundamental::Application::Instance().Exit();
    return 0;
}
