#pragma once

#include "fundamental/events/event_system.h"
#include "protocal_pipe_proxy_connection.hpp"
#include "ws_port_pipe_forward_connection.hpp"

#include "network/network.hpp"

namespace network
{
namespace proxy
{
using ::asio::ip::tcp;
class ws_port_pipe_server : private asio::noncopyable, public std::enable_shared_from_this<ws_port_pipe_server> {

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<ws_port_pipe_server>(std::forward<Args>(args)...);
    }
    template <typename port_type>
    ws_port_pipe_server(port_type port) : acceptor_(io_context_pool::Instance().get_io_context()) {
        protocal_helper::init_acceptor(acceptor_, static_cast<std::uint16_t>(port));
    }
    ~ws_port_pipe_server() {
    }

    void start() {
        bool expected_value = false;
        if (!has_started_.compare_exchange_strong(expected_value, true)) return;
        FINFO("start ws port forward server on {}:{}", acceptor_.local_endpoint().address().to_string(),
              acceptor_.local_endpoint().port());
        do_accept();
    }

    void stop() {
        release_obj();
    }

    void release_obj() {
        reference_.release();
        bool expected_value = true;
        if (!has_started_.compare_exchange_strong(expected_value, false)) return;
        asio::post(acceptor_.get_executor(), [this, ref = shared_from_this()] {
            try {
                std::error_code ec;
                acceptor_.close(ec);
            } catch (const std::exception& e) {
            }
        });
    }

    void set_forward_config(rpc_client_forward_config forward_config,
                            std::string dst_host,
                            std::string dst_port,
                            std::string api_path,
                            std::string pipe_proxy_host    = "",
                            std::string pipe_proxy_service = "") {
        forward_config_ = forward_config;
        dst_host_       = std::move(dst_host);
        dst_port_       = std::move(dst_port);
        api_path_       = std::move(api_path);
        proxy_host_     = pipe_proxy_host;
        proxy_port_     = pipe_proxy_service;
    }

private:
    void do_accept() {

        acceptor_.async_accept(
            io_context_pool::Instance().get_io_context(),
            [this, ptr = shared_from_this()](asio::error_code ec, asio::ip::tcp::socket socket) {
                if (!reference_.is_valid()) {
                    return;
                }
                if (!acceptor_.is_open()) {
                    return;
                }

                if (ec) {
                    // maybe system error... ignored
                } else {
                    auto new_conn       = ws_port_pipe_upstream::make_shared(std::move(socket));
                    auto release_handle = reference_.notify_release.Connect([con = new_conn->weak_from_this()]() {
                        auto ptr = con.lock();
                        if (ptr) ptr->release_obj();
                    });
                    // unbind
                    new_conn->reference_.notify_release.Connect([release_handle, s = weak_from_this(), this]() {
                        auto ptr = s.lock();
                        if (ptr) reference_.notify_release.DisConnect(release_handle);
                    });
                    std::shared_ptr<ws_port_pipe_forward_connection> new_proxy_instance;
                    if (proxy_host_.empty() || proxy_port_.empty()) {
                        new_proxy_instance = ws_port_pipe_forward_connection::make_shared(
                            std::move(new_conn), dst_host_, dst_port_, api_path_);
                    } else {
                        new_proxy_instance = protocal_pipe_proxy_connection::make_shared(
                            std::move(new_conn), proxy_host_, proxy_port_, dst_host_, dst_port_, api_path_);
                    }
                    if (!forward_config_.ssl_config.disable_ssl) {
                        new_proxy_instance->enable_proxy_ssl(forward_config_.ssl_config);
                    }
                    new_proxy_instance->start();
                }

                do_accept();
            });
    }

    network_data_reference reference_;
    std::atomic_bool has_started_ = false;
    tcp::acceptor acceptor_;
    rpc_client_forward_config forward_config_;
    std::string dst_host_;
    std::string dst_port_;
    std::string api_path_;
    std::string proxy_host_;
    std::string proxy_port_;
};
} // namespace proxy
  // namespace proxy
} // namespace network


