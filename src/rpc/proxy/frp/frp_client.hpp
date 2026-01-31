#pragma once

#include "frp_client_upstream.hpp"
#include "frp_command.hpp"

#include "rpc/proxy/rpc_forward_connection.hpp"

namespace network
{
namespace proxy
{
class frp_client_session;
class frp_client;

class frp_client : public std::enable_shared_from_this<frp_client>, private asio::noncopyable {
    friend class frp_client_session;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_client>(std::forward<Args>(args)...);
    }
    frp_client(const std::string& host,
               const std::string& service,
               std::uint16_t want_port,
               const std::string& proxy_host,
               const std::string& proxy_service);
    ~frp_client() {
    }

    void release_obj() {
        reference_.release();
        delay_retry_timer.cancel();
        bool expected_value = true;
        if (!has_started_.compare_exchange_strong(expected_value, false)) return;
        asio::post(executor_, [this, ref = shared_from_this()] {
            if (pending_client) {
                pending_client->release_obj();
                pending_client = nullptr;
            }
        });
    }

    void start() {
        bool expected_value = false;
        if (!has_started_.compare_exchange_strong(expected_value, true)) return;
        asio::post(executor_, [this, ref = shared_from_this()] { setup_frp_session(); });
    }

    void stop() {
        release_obj();
    }

    void enable_ssl(network_client_ssl_config client_ssl_config) {
#ifndef NETWORK_DISABLE_SSL
        ssl_config_ = client_ssl_config;
#endif
    }

private:
    bool is_ssl() const {
#ifndef NETWORK_DISABLE_SSL
        return !ssl_config_.disable_ssl;
#else
        return false;
#endif
    }
    void setup_frp_session();
    void delay_setup_frp_session();

private:
    network_data_reference reference_;
    const std::string host;
    const std::string service;
    const std::uint16_t want_proxy_port = 0;

    const std::string proxy_host;
    const std::string proxy_service;
    const std::string frp_id;
#ifndef NETWORK_DISABLE_SSL
    network_client_ssl_config ssl_config_;
#endif
    ::asio::steady_timer delay_retry_timer;
    const asio::any_io_executor& executor_;
    std::atomic_bool has_started_ = false;
    std::shared_ptr<frp_client_upstream> pending_client;
};

class frp_client_session : public rpc_forward_connection {
    friend class frp_client;
    Fundamental::Signal<void(std::uint16_t)> notify_setup_port;
    Fundamental::Signal<void(bool)> notify_accept_result;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_client_session>(std::forward<Args>(args)...);
    }
    explicit frp_client_session(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                const std::string& frp_id,
                                const std::string& dst_host,
                                const std::string& dst_port,
                                std::uint16_t proxy_port);
    ~frp_client_session();

protected:
    void StartProtocal() override;
    void HandleConnectSuccess() override;
    void process_protocal() override;

private:
    void send_frp_setup();
    void process_peer_command(frp_command_type next_command);
    void process_frp_setup_response(std::string commad_data);
    void process_frp_accept_signal(std::string command_data);
    void try_notify_accept_result(bool b_success);

protected:
    const std::string frp_id;
    std::uint8_t head_buf[4];
    std::string payload;
    std::atomic_bool has_notify_accept_result = false;
    std::uint16_t proxy_port                  = 0;
    std::size_t origin_timeout_msec           = 0;
};

} // namespace proxy
} // namespace network