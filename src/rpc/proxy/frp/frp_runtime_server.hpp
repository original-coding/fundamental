#pragma once

#include "frp_config.hpp"
#include "frp_runtime_command.hpp"

#include "network/network.hpp"

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace network::proxy
{

using ::asio::ip::tcp;
using ::asio::ip::udp;

class frp_runtime_signal_session;

struct frp_runtime_service_directory_entry {
    std::string service_name;
    std::string provider_uuid;
    std::uint8_t provider_nat_type = frp_runtime_nat_type_disabled;
    std::uint8_t service_type      = frp_runtime_service_tcp;
    bool enable_p2p                = true;
};

class frp_runtime_public_server : public std::enable_shared_from_this<frp_runtime_public_server>,
                                  private asio::noncopyable {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_runtime_public_server>(std::forward<Args>(args)...);
    }

    explicit frp_runtime_public_server(frp_public_server_config config);
    ~frp_runtime_public_server() = default;

    void start();
    void release_obj();

    bool verify_auth_digest(std::string_view nonce, std::string_view digest) const;
    bool register_client_services(const std::shared_ptr<frp_runtime_signal_session>& session,
                                  const frp_runtime_register_services_data& request,
                                  std::string& error_message);
    std::vector<frp_runtime_visible_service_data> list_services_for_subscriber(
        const std::shared_ptr<frp_runtime_signal_session>& session,
        const std::vector<std::string>& register_keys,
        std::string& error_message) const;
    frp_runtime_create_flow_response_data create_flow(const std::shared_ptr<frp_runtime_signal_session>& accessor_session,
                                                      const frp_runtime_create_flow_request_data& request);
    bool provider_mark_flow_ready(const std::shared_ptr<frp_runtime_signal_session>& provider_session,
                                  std::uint32_t flow_id,
                                  std::string& error_message);
    bool forward_flow_failed(const std::shared_ptr<frp_runtime_signal_session>& session,
                             const frp_runtime_flow_failed_data& data,
                             std::string& error_message);
    bool forward_flow_closed(const std::shared_ptr<frp_runtime_signal_session>& session,
                             const frp_runtime_flow_closed_data& data,
                             std::string& error_message);
    bool forward_flow_data(const std::shared_ptr<frp_runtime_signal_session>& session,
                           const frp_runtime_flow_data_data& data,
                           std::string& error_message);
    bool bind_data_session(const std::shared_ptr<frp_runtime_signal_session>& session,
                           const frp_runtime_data_open_data& data,
                           std::string& error_message);
    bool forward_flow_bytes(const std::shared_ptr<frp_runtime_signal_session>& session,
                            const std::shared_ptr<std::string>& data,
                            std::string& error_message);
    void relay_punch_message(const std::shared_ptr<frp_runtime_signal_session>& session,
                             const frp_proxy_command_data& base,
                             const std::string& raw_payload);
    void release_session_state(const frp_runtime_signal_session* session_ptr);

    const frp_public_server_config& get_config() const {
        return config_;
    }

#ifndef NETWORK_DISABLE_SSL
    asio::ssl::context* get_ssl_context() {
        return ssl_context_.get();
    }
#endif

private:
    void do_accept();
    void start_udp_servers();
    void start_udp_receive(std::size_t index);
    void configure_ssl();

    struct udp_server_state {
        explicit udp_server_state(const asio::any_io_executor& executor) : socket(executor) {
        }
        udp::socket socket;
        udp::endpoint remote_endpoint;
        std::array<char, 2048> read_buf {};
    };

    struct client_runtime_state {
        std::string uuid;
        std::string register_key;
        std::uint8_t nat_type = frp_runtime_nat_type_disabled;
        std::uint32_t startup_rtt_ms = 100;
        std::unordered_set<std::string> services;
        std::weak_ptr<frp_runtime_signal_session> session;
    };

    struct flow_runtime_state {
        std::uint32_t flow_id = 0;
        std::string service_name;
        bool enable_p2p = true;
        std::string provider_uuid;
        std::string accessor_uuid;
        std::uint8_t transport = frp_runtime_transport_invalid;
        bool provider_ready = false;
        bool transport_ready = false;
        std::weak_ptr<frp_runtime_signal_session> provider_data_session;
        std::weak_ptr<frp_runtime_signal_session> accessor_data_session;
    };

    void clear_session_state_locked(const std::shared_ptr<frp_runtime_signal_session>& session);
    void clear_client_state_locked(const std::string& uuid);

private:
    network_data_reference reference_;
    frp_public_server_config config_;
    tcp::acceptor acceptor_;
    std::atomic_bool has_started_ = false;
    std::vector<std::shared_ptr<udp_server_state>> udp_servers_;

#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::context> ssl_context_;
#endif

    mutable std::mutex runtime_mutex_;
    std::unordered_map<std::string, std::weak_ptr<frp_runtime_signal_session>> sessions_by_uuid_;
    std::unordered_map<std::string, client_runtime_state> clients_by_uuid_;
    std::unordered_map<std::string, std::unordered_map<std::string, frp_runtime_service_directory_entry>>
        services_by_register_key_;
    std::unordered_set<std::string> allowed_register_keys_;
    std::unordered_map<std::uint32_t, flow_runtime_state> flows_by_id_;
    std::atomic_uint32_t next_flow_id_ = 1;
};

class frp_runtime_signal_session : public std::enable_shared_from_this<frp_runtime_signal_session>,
                                   private asio::noncopyable {
public:

    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_runtime_signal_session>(std::forward<Args>(args)...);
    }

    frp_runtime_signal_session(::asio::ip::tcp::socket&& socket, std::shared_ptr<frp_runtime_public_server> owner);
    ~frp_runtime_signal_session() = default;

    void start();
    void release_obj();

#ifndef NETWORK_DISABLE_SSL
    void enable_ssl(asio::ssl::context& ssl_context);
#endif

    template <typename CommandData>
    void send_command(const CommandData& data) {
        auto packet = packet_frp_runtime_command_data(data);
        if (!packet) return;
        asio::post(executor_, [this, self = shared_from_this(), packet = std::move(packet)]() mutable {
            if (!reference_.is_valid()) return;
            write_queue_.push_back(std::move(packet));
            if (write_queue_.size() == 1) {
                do_write();
            }
        });
    }

    bool is_authenticated() const {
        return authenticated_;
    }
    const std::string& get_uuid() const {
        return uuid_;
    }
    const std::string& get_register_key() const {
        return register_key_;
    }
    std::uint8_t get_nat_type() const {
        return nat_type_;
    }
    std::uint32_t get_flow_id() const {
        return flow_id_;
    }
    bool is_data_session() const {
        return mode_ == session_mode::data;
    }

private:
    enum class session_mode : std::uint8_t
    {
        undecided = 0,
        signal,
        data
    };

    void do_write();
    void start_protocol();
    void read_next_command();
    void start_data_read_loop();
    void process_command(std::string payload);
    void handle_initial_phase(const frp_runtime_command_base& command, std::string payload);
    void handle_server_hello_phase(const frp_runtime_command_base& command, std::string payload);
    void handle_authenticated_phase(const frp_runtime_command_base& command, std::string payload);
    void handle_data_open_phase(const frp_runtime_data_open_data& request);
    void handle_register_services_phase(const frp_runtime_register_services_data& request);
    void handle_subscribe_services_phase(const frp_runtime_subscribe_services_data& request);
    void handle_create_flow_phase(const frp_runtime_create_flow_request_data& request);
    void handle_flow_ready_phase(const frp_runtime_flow_ready_data& request);
    void handle_flow_failed_phase(const frp_runtime_flow_failed_data& request);
    void handle_flow_data_phase(const frp_runtime_flow_data_data& request);
    void handle_flow_closed_phase(const frp_runtime_flow_closed_data& request);
    void send_auth_failure_and_close(const std::string& message);
    void close_socket();
    void ssl_handshake();
    void send_raw(const std::shared_ptr<std::string>& packet);

private:
    friend class frp_runtime_public_server;

    network_data_reference reference_;
    ::asio::ip::tcp::socket socket_;
    const asio::any_io_executor executor_;
    std::shared_ptr<frp_runtime_public_server> owner_;
    std::deque<std::shared_ptr<std::string>> write_queue_;
    std::array<std::uint8_t, 4> header_buf_ {};
    std::array<char, 16 * 1024> raw_read_buf_ {};
    std::string payload_;
    std::string server_nonce_;
    std::uint32_t flow_id_      = 0;
    session_mode mode_          = session_mode::undecided;
    std::string uuid_;
    std::string register_key_;
    std::uint8_t nat_type_        = frp_runtime_nat_type_disabled;
    bool authenticated_         = false;
    bool joined_                = false;

#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_;
    asio::ssl::context* ssl_context_ref_ = nullptr;
#endif
};

} // namespace network::proxy
