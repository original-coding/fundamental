#pragma once

#include "frp_client_upstream.hpp"
#include "frp_config.hpp"
#include "frp_runtime_command.hpp"

#include "network/network.hpp"

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>

namespace network::proxy
{

class frp_runtime_signal_client_channel : public std::enable_shared_from_this<frp_runtime_signal_client_channel>,
                                          private asio::noncopyable {
public:
    using connect_callback_t = std::function<void()>;
    using disconnect_callback_t = std::function<void()>;
    using command_callback_t = std::function<void(const frp_runtime_command_base&, std::string)>;

    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_runtime_signal_client_channel>(std::forward<Args>(args)...);
    }

    frp_runtime_signal_client_channel(const asio::any_io_executor& executor,
                                      std::string host,
                                      std::string service);
    ~frp_runtime_signal_client_channel() = default;

    void enable_ssl(network_client_ssl_config config);
    void set_on_connected(connect_callback_t cb);
    void set_on_disconnected(disconnect_callback_t cb);
    void set_on_command(command_callback_t cb);
    void start();
    void release_obj();

    template <typename CommandData>
    void send_command(const CommandData& data) {
        auto packet = packet_frp_runtime_command_data(data);
        asio::post(executor_, [this, self = shared_from_this(), packet = std::move(packet)]() mutable {
            if (!reference_.is_valid() || !upstream_) return;
            write_queue_.push_back(std::move(packet));
            if (write_queue_.size() == 1) {
                do_write();
            }
        });
    }

private:
    void read_next_command();
    void do_write();
    void notify_disconnect_once();

private:
    network_data_reference reference_;
    const asio::any_io_executor executor_;
    const std::string host_;
    const std::string service_;
    std::shared_ptr<frp_client_upstream> upstream_;
    std::shared_ptr<proxy_upstream_interface> transport_;
    network_client_ssl_config ssl_config_;
    std::deque<std::shared_ptr<std::string>> write_queue_;
    std::array<std::uint8_t, 4> header_buf_ {};
    std::string payload_;
    connect_callback_t on_connected_;
    disconnect_callback_t on_disconnected_;
    command_callback_t on_command_;
    bool disconnect_notified_ = false;
};

class frp_runtime_data_client_channel : public std::enable_shared_from_this<frp_runtime_data_client_channel>,
                                        private asio::noncopyable {
public:
    using connect_callback_t = std::function<void()>;
    using disconnect_callback_t = std::function<void()>;
    using data_callback_t = std::function<void(std::string)>;

    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_runtime_data_client_channel>(std::forward<Args>(args)...);
    }

    frp_runtime_data_client_channel(const asio::any_io_executor& executor,
                                    std::string host,
                                    std::string service,
                                    std::uint32_t flow_id,
                                    std::string uuid);
    ~frp_runtime_data_client_channel() = default;

    void enable_ssl(network_client_ssl_config config);
    void set_on_connected(connect_callback_t cb);
    void set_on_disconnected(disconnect_callback_t cb);
    void set_on_data(data_callback_t cb);
    void start();
    void release_obj();
    void send_bytes(const std::shared_ptr<std::string>& data);
    std::string local_endpoint_string() const;
    std::string remote_endpoint_string() const;

private:
    void read_next_chunk();
    void do_write();
    void notify_disconnect_once();

private:
    network_data_reference reference_;
    const asio::any_io_executor executor_;
    const std::string host_;
    const std::string service_;
    const std::uint32_t flow_id_;
    const std::string uuid_;
    std::shared_ptr<frp_client_upstream> upstream_;
    std::shared_ptr<proxy_upstream_interface> transport_;
    network_client_ssl_config ssl_config_;
    std::array<char, 16 * 1024> read_buf_ {};
    std::deque<std::shared_ptr<std::string>> write_queue_;
    connect_callback_t on_connected_;
    disconnect_callback_t on_disconnected_;
    data_callback_t on_data_;
    bool disconnect_notified_ = false;
};

class frp_runtime_provider_agent : public std::enable_shared_from_this<frp_runtime_provider_agent>,
                                   private asio::noncopyable {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_runtime_provider_agent>(std::forward<Args>(args)...);
    }

    explicit frp_runtime_provider_agent(frp_provider_config config);
    ~frp_runtime_provider_agent() = default;

    void start();
    void release_obj();

private:
    struct provider_flow_runtime;
    void connect_signal_channel();
    void schedule_reconnect();
    void process_command(const frp_runtime_command_base& command, std::string payload);
    void start_flow_endpoint_probe(const std::shared_ptr<provider_flow_runtime>& flow);
    void start_udp_punch(const std::shared_ptr<provider_flow_runtime>& flow);
    void start_provider_p2p_read_loop(const std::shared_ptr<provider_flow_runtime>& flow);
    void schedule_provider_kcp_update(const std::shared_ptr<provider_flow_runtime>& flow);
    void provider_kcp_send(const std::shared_ptr<provider_flow_runtime>& flow, const char* data, std::size_t size);
    void start_provider_backend_connect(const std::shared_ptr<provider_flow_runtime>& flow);
    void start_backend_read_loop(const std::shared_ptr<provider_flow_runtime>& flow);
    void handle_backend_write_queue(const std::shared_ptr<provider_flow_runtime>& flow);
    std::optional<frp_provider_service_config> find_service(std::string_view service_name) const;

private:
    network_data_reference reference_;
    frp_provider_config config_;
    const std::string uuid_;
    asio::steady_timer reconnect_timer_;
    std::shared_ptr<frp_runtime_signal_client_channel> channel_;
    std::unordered_map<std::uint32_t, std::shared_ptr<provider_flow_runtime>> flows_;
    frp_runtime_nat_type probed_nat_type_ = frp_runtime_nat_type_disabled;
};

class frp_runtime_accessor_agent : public std::enable_shared_from_this<frp_runtime_accessor_agent>,
                                   private asio::noncopyable {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_runtime_accessor_agent>(std::forward<Args>(args)...);
    }

    explicit frp_runtime_accessor_agent(frp_accessor_config config);
    ~frp_runtime_accessor_agent() = default;

    void start();
    void release_obj();

private:
    struct accessor_session_context;
    struct listener_runtime {
        std::string service_name;
        std::string listen_host;
        std::uint16_t listen_port = 0;
        std::uint8_t nat_type     = frp_runtime_nat_type_full;
        asio::ip::tcp::acceptor acceptor;

        listener_runtime(const asio::any_io_executor& executor,
                         std::string service_name,
                         std::string listen_host,
                         std::uint16_t listen_port,
                         std::uint8_t nat_type) :
        service_name(std::move(service_name)),
        listen_host(std::move(listen_host)),
        listen_port(listen_port),
        nat_type(nat_type),
        acceptor(executor) {
        }
    };

    void connect_signal_channel();
    void schedule_reconnect();
    void clear_listeners();
    void reconcile_listeners(const std::vector<frp_runtime_visible_service_data>& services);
    void start_accept_loop(const std::shared_ptr<listener_runtime>& listener);
    void process_command(const frp_runtime_command_base& command, std::string payload);
    void request_flow(const std::shared_ptr<accessor_session_context>& session, bool enable_p2p_request);
    void start_flow_endpoint_probe(const std::shared_ptr<accessor_session_context>& session);
    void start_udp_punch(const std::shared_ptr<accessor_session_context>& session);
    void start_accessor_p2p_read_loop(const std::shared_ptr<accessor_session_context>& session);
    void schedule_accessor_kcp_update(const std::shared_ptr<accessor_session_context>& session);
    void accessor_kcp_send(const std::shared_ptr<accessor_session_context>& session, const char* data, std::size_t size);
    void start_local_read_loop(const std::shared_ptr<accessor_session_context>& session);
    void handle_local_write_queue(const std::shared_ptr<accessor_session_context>& session);
    void fail_session(const std::shared_ptr<accessor_session_context>& session, const std::string& reason);

private:
    network_data_reference reference_;
    frp_accessor_config config_;
    const std::string uuid_;
    asio::steady_timer reconnect_timer_;
    std::shared_ptr<frp_runtime_signal_client_channel> channel_;
    std::unordered_map<std::string, std::shared_ptr<listener_runtime>> listeners_;
    std::unordered_map<std::uint32_t, std::shared_ptr<accessor_session_context>> sessions_by_flow_id_;
    std::unordered_map<std::uint64_t, std::shared_ptr<accessor_session_context>> pending_sessions_;
    std::uint64_t next_session_id_ = 1;
    frp_runtime_nat_type probed_nat_type_ = frp_runtime_nat_type_disabled;
};

} // namespace network::proxy
