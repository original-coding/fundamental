#pragma once

#include "rpc/proxy/rpc_forward_connection.hpp"

namespace network
{
namespace proxy
{
class frp_client_upstream : public std::enable_shared_from_this<frp_client_upstream>,
                            private asio::noncopyable,
                            public proxy::proxy_upstream_interface {
public:
    Fundamental::Signal<void(Fundamental::error_code, std::shared_ptr<frp_client_upstream>)> notify_connect_result;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_client_upstream>(std::forward<Args>(args)...);
    }
    frp_client_upstream(const asio::any_io_executor& executor, const std::string& host, const std::string& service) :
    host(host), service(service), executor_(executor), socket_(executor_), resolver_(executor_) {
    }
    ~frp_client_upstream() {
    }

    void release_obj() override {
        reference_.release();
        asio::post(executor_, [this, ref = shared_from_this()] { close(); });
    }

    void start_async_connect() {
        resolver_.async_resolve(
            host, service,
            [this, ptr = shared_from_this()](const std::error_code& ec,
                                             const decltype(resolver_)::results_type& endpoints) {
                if (!reference_.is_valid()) {
                    return;
                }
                if (ec) {
                    notify_connect_result(
                        Fundamental::error_code(ec, Fundamental::StringFormat("resolver {}:{} failed", host, service)),
                        shared_from_this());
                    return;
                }
                asio::async_connect(
                    socket_, endpoints,
                    [this, ptr = shared_from_this()](const asio::error_code& ec,
                                                     const asio::ip::tcp::endpoint& endpoint) {
                        if (!reference_.is_valid()) {
                            return;
                        }
                        if (ec) {
                            notify_connect_result(
                                Fundamental::error_code(
                                    ec, Fundamental::StringFormat("connect to {}:{} failed", host, service)),
                                shared_from_this());
                            return;
                        }
                        enable_tcp_keep_alive(socket_);
                        handle_transfer_ready();
                    });
            });
    }

    void enable_ssl(network_client_ssl_config client_ssl_config) {
#ifndef NETWORK_DISABLE_SSL
        ssl_config_ = client_ssl_config;
#endif
    }

private:
    void ssl_handshake() {
#ifndef NETWORK_DISABLE_SSL
        asio::ssl::context ssl_context(asio::ssl::context::tlsv13);
        auto* actual_context = &ssl_context;
        try {
            if (ssl_config_.load_exception) std::rethrow_exception(ssl_config_.load_exception);
            if (!ssl_config_.ssl_context) {
                if (!ssl_config_.ca_certificate_path.empty()) {
                    ssl_context.load_verify_file(ssl_config_.ca_certificate_path);
                } else {
                    ssl_context.set_default_verify_paths();
                }
                if (!ssl_config_.private_key_path.empty()) {
                    ssl_context.use_private_key_file(ssl_config_.private_key_path, asio::ssl::context::pem);
                }
                if (!ssl_config_.certificate_path.empty()) {
                    ssl_context.use_certificate_chain_file(ssl_config_.certificate_path);
                }
            } else {
                actual_context = ssl_config_.ssl_context.get();
            }
        } catch (const std::exception& e) {
            notify_connect_result(Fundamental::error_code::make_basic_error(
                                      1, Fundamental::StringFormat("ssl load context failed:{}", e.what())),
                                  shared_from_this());
            return;
        }

        ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket_, *actual_context);
        ssl_stream_->set_verify_mode(asio::ssl::verify_peer);
        SSL_set_tlsext_host_name(ssl_stream_->native_handle(), host.c_str());
        ssl_stream_->async_handshake(asio::ssl::stream_base::client, [this, ptr = shared_from_this()](
                                                                         const asio::error_code& ec) {
            if (!reference_.is_valid()) {
                return;
            }
            if (!ec) {
                frp_protocal_ready();
            } else {
                notify_connect_result(Fundamental::error_code(ec, Fundamental::StringFormat(
                                                                      "ssl handshake to {}:{} failed", host, service)),
                                      shared_from_this());
            }
        });
#endif
    }

    void handle_transfer_ready() {
        if (is_ssl()) {
            ssl_handshake();
        } else {
            frp_protocal_ready();
        }
    }
    void frp_protocal_ready() {
        socket_.set_option(asio::ip::tcp::no_delay(true));
        notify_connect_result(Fundamental::error_code::make_basic_error(
                                  0, Fundamental::StringFormat("ssl handshake to {}:{} success", host, service)),
                              shared_from_this());
    }

    bool is_ssl() const {
#ifndef NETWORK_DISABLE_SSL
        return !ssl_config_.disable_ssl;
#else
        return false;
#endif
    }
    void async_buffers_read(network_read_buffers_t buffers, network_io_handler_t handler) override {
        if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            asio::async_read(*ssl_stream_, std::move(buffers), std::move(handler));
#endif
        } else {
            asio::async_read(socket_, std::move(buffers), std::move(handler));
        }
    }

    void async_buffers_read_some(network_read_buffers_t buffers, network_io_handler_t handler) override {
        if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            ssl_stream_->async_read_some(std::move(buffers), std::move(handler));
#endif
        } else {
            socket_.async_read_some(std::move(buffers), std::move(handler));
        }
    }

    void async_buffers_write(network_write_buffers_t buffers, network_io_handler_t handler) override {
        if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            asio::async_write(*ssl_stream_, std::move(buffers), std::move(handler));
#endif
        } else {
            asio::async_write(socket_, std::move(buffers), std::move(handler));
        }
    }
    void async_buffers_write_some(network_write_buffers_t buffers, network_io_handler_t handler) override {

        if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            ssl_stream_->async_write_some(std::move(buffers), std::move(handler));
#endif
        } else {
            socket_.async_write_some(std::move(buffers), std::move(handler));
        }
    }
    const asio::any_io_executor& get_current_executor() override {
        return executor_;
    }

    void close() {

        if (!socket_.is_open()) return;
        auto final_clear_function = [this, ptr = shared_from_this()]() {
            asio::error_code ec;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        };

#ifndef NETWORK_DISABLE_SSL
        if (ssl_stream_) {
            ::asio::dispatch(ssl_stream_->get_executor(), final_clear_function);
            return;
        }
#endif
        final_clear_function();
    }

private:
    network_data_reference reference_;
    const std::string host;
    const std::string service;

    const asio::any_io_executor& executor_;
    ::asio::ip::tcp::socket socket_;
#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_ = nullptr;
    network_client_ssl_config ssl_config_;
#endif
    asio::ip::tcp::resolver resolver_;
};

} // namespace proxy
} // namespace network