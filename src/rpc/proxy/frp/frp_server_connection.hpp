#pragma once

#include "frp_command.hpp"
#include "rpc/proxy/rpc_forward_connection.hpp"

namespace network
{
namespace proxy
{
class frp_port_proxy_session;
class frp_basic_connection : public std::enable_shared_from_this<frp_basic_connection>,
                             private asio::noncopyable,
                             public proxy::proxy_upstream_interface {
    friend class frp_port_proxy_session;
    Fundamental::Signal<void(std::string)> notify_data_pending;

public:
    Fundamental::Signal<void(std::shared_ptr<frp_basic_connection>)> notify_connection_ready;
    using verify_port_func = std::function<bool(std::uint16_t)>;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_basic_connection>(std::forward<Args>(args)...);
    }
    frp_basic_connection(::asio::ip::tcp::socket&& socket, verify_port_func verify_func) :
    socket_(std::move(socket)), executor_(socket_.get_executor()), verify_func_(verify_func) {
        enable_tcp_keep_alive(socket_);
    }
    ~frp_basic_connection() {
    }

    void release_obj() override {
        reference_.release();
        asio::post(executor_, [this, ref = shared_from_this()] { close(); });
    }

    void start() {
        if (is_ssl()) {
            ssl_handshake();
        } else {
            start_protocal();
        }
    }

#ifndef NETWORK_DISABLE_SSL
    void enable_ssl(asio::ssl::context& ssl_context) {
        ssl_context_ref = &ssl_context;
    }
#endif
    std::uint16_t get_proxy_port() const {
        return proxy_port;
    }
    std::string get_frp_id() const {
        return frp_id;
    }

private:
    void start_proxy_session_waiting(std::shared_ptr<frp_port_proxy_session> session);

    void finish_proxy_session_waiting(std::function<void(std::string)> wait_finish_cb) {
        if (!wait_finish_cb) return;
        std::scoped_lock<std::mutex> locker(preread_mutex);
        if (!preread_cache.empty())
            wait_finish_cb(preread_cache);
        else {
            notify_data_pending.Connect(wait_finish_cb);
        }
    }

private:
    void process_peer_command(frp_command_type next_command);
    void process_frp_setup(std::string commad_data);
    void send_frp_setup_response();
    void start_protocal() {
        process_peer_command(frp_command_type::frp_setup_request_command);
    }

    void ssl_handshake();

    bool is_ssl() const {
#ifndef NETWORK_DISABLE_SSL
        return ssl_context_ref != nullptr;
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
    ::asio::ip::tcp::socket socket_;
    const asio::any_io_executor& executor_;

#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_ = nullptr;
    asio::ssl::context* ssl_context_ref                                    = nullptr;
#endif
    verify_port_func verify_func_;
    std::uint16_t proxy_port = 0;
    std::string frp_id;
    std::uint8_t head_buf[4];
    std::string payload;
    std::shared_ptr<frp_port_proxy_session> ref_port_session;
    std::mutex preread_mutex;
    char preread_buf[1];
    std::string preread_cache;
};

class frp_port_proxy_connection : public rpc_forward_connection {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_port_proxy_connection>(std::forward<Args>(args)...);
    }
    explicit frp_port_proxy_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                       ::asio::ip::tcp::socket&& downstream_socket);
    void delay_client_read_init(std::string preread_data);

protected:
    void process_protocal() override;
};

class frp_port_proxy_session : public std::enable_shared_from_this<frp_port_proxy_session>, private asio::noncopyable {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_port_proxy_session>(std::forward<Args>(args)...);
    }
    explicit frp_port_proxy_session(std::uint16_t proxy_port, const std::string& frp_id);
    void push_new_session(std::shared_ptr<frp_basic_connection> new_session_connection);
    const std::string& get_frp_id() const {
        return frp_id_;
    };
    ~frp_port_proxy_session();

private:
    void try_accept();

protected:
    ::asio::ip::tcp::acceptor acceptor_;
    const std::string frp_id_;
    std::mutex pending_mutex;
    std::queue<std::weak_ptr<frp_basic_connection>> pending_requests;
    std::atomic_flag accept_flag = ATOMIC_FLAG_INIT;
};

} // namespace proxy
} // namespace network