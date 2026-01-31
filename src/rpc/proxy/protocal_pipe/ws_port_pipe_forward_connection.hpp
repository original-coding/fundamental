#pragma once
#include "forward_pipe_codec.hpp"
#include "network/network.hpp"
#include "protocal_pipe_connection.hpp"

#include "rpc/proxy/rpc_forward_connection.hpp"
#include "rpc/proxy/socks5/common.h"

namespace network
{
namespace proxy
{
class ws_port_pipe_server;
class ws_port_pipe_upstream : public std::enable_shared_from_this<ws_port_pipe_upstream>,
                              private asio::noncopyable,
                              public proxy::proxy_upstream_interface {
    friend class ws_port_pipe_server;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<ws_port_pipe_upstream>(std::forward<Args>(args)...);
    }
    ws_port_pipe_upstream(::asio::ip::tcp::socket socket) :
    socket_(std::move(socket)), executor_(socket_.get_executor()) {
        enable_tcp_keep_alive(socket_);
    }
    ~ws_port_pipe_upstream() {
    }

    void release_obj() override {
        reference_.release();
        asio::post(executor_, [this, ref = shared_from_this()] { close(); });
    }

private:
    void async_buffers_read(network_read_buffers_t buffers, network_io_handler_t handler) override {
        asio::async_read(socket_, std::move(buffers), std::move(handler));
    }

    void async_buffers_read_some(network_read_buffers_t buffers, network_io_handler_t handler) override {
        socket_.async_read_some(std::move(buffers), std::move(handler));
    }

    void async_buffers_write(network_write_buffers_t buffers, network_io_handler_t handler) override {
        asio::async_write(socket_, std::move(buffers), std::move(handler));
    }
    void async_buffers_write_some(network_write_buffers_t buffers, network_io_handler_t handler) override {

        socket_.async_write_some(std::move(buffers), std::move(handler));
    }
    const asio::any_io_executor& get_current_executor() override {
        return executor_;
    }

    void close() {

        asio::error_code ignored_ec;
        socket_.shutdown(::asio::ip::tcp::socket::shutdown_both, ignored_ec);
        socket_.close(ignored_ec);
    }
    network_data_reference reference_;
    ::asio::ip::tcp::socket socket_;
    const asio::any_io_executor& executor_;
};
// connect proxy host ->[ssl_handle shake] ->ws proxy handshake->raw proxy
class ws_port_pipe_forward_connection : public rpc_forward_connection {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<ws_port_pipe_forward_connection>(std::forward<Args>(args)...);
    }
    explicit ws_port_pipe_forward_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                             std::string dst_host,
                                             std::string dst_port,
                                             std::string api_path) :
    rpc_forward_connection(std::move(ref_connection), "") {
        request_context.dst_host    = dst_host;
        request_context.dst_service = dst_port;
        request_context.route_path  = api_path;
        proxy_host                  = dst_host;
        proxy_service               = dst_port;
    }
    void enable_proxy_ssl(network_client_ssl_config client_ssl_config) {
        enable_ssl(client_ssl_config);
    }

protected:
    void process_protocal() override {
        StartProtocal();
    }
    void StartProtocal() override {
        StartDnsResolve(proxy_host, proxy_service);
    }
    void StartForward() override {
        process_pipe_handshake();
    }

protected:
    virtual void process_pipe_handshake();
    void do_pipe_proxy() {
        StartClientRead();
        rpc_forward_connection::StartForward();
    }

protected:
    forward::forward_request_context request_context;
};
} // namespace proxy
} // namespace network