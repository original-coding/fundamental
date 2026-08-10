#pragma once

#include "frp_command.hpp"
#include "frp_config_types.hpp"

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

class frp_signal_session;

class frp_public_server : public std::enable_shared_from_this<frp_public_server>, private asio::noncopyable {
    friend class frp_signal_session;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_public_server>(std::forward<Args>(args)...);
    }

    explicit frp_public_server(frp_public_server_config config);
    ~frp_public_server() = default;

    void start();
    void release_obj();

    bool verify_auth_digest(std::string_view nonce, std::string_view digest) const;
    bool register_client_services(frp_signal_session& session,
                                  const frp_register_services_data& request,
                                  std::string& error_message);

    void register_data_channel(frp_signal_session& session, const frp_channel_open_request_data& request);
    std::vector<frp_visible_service_data> list_services_for_subscriber(frp_signal_session& session,
                                                                       const std::vector<std::string>& register_keys,
                                                                       std::string& error_message) const;
    void forward_data(const std::string& uuid, std::string packet);

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

    void remove_session(const std::string& uuid, const std::string& connection_uuid);

private:
    network_data_reference reference_;
    frp_public_server_config config_;
    tcp::acceptor acceptor_;
    std::atomic_bool has_started_ = false;
    std::vector<std::shared_ptr<udp_server_state>> udp_servers_;

#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::context> ssl_context_;
#endif

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::weak_ptr<frp_signal_session>>>
        sessions_by_uuid_;

    std::unordered_map<std::string, std::unordered_set<std::string>> allowed_register_keys_cache;
};

class frp_signal_session : public std::enable_shared_from_this<frp_signal_session>, private asio::noncopyable {
    friend class frp_public_server;

public:
    using upgrade_forward_function = std::function<bool(const void*, std::size_t)>;
    using upgrade_release_function = std::function<void()>;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_signal_session>(std::forward<Args>(args)...);
    }

    frp_signal_session(::asio::ip::tcp::socket&& socket, frp_public_server& owner);
    ~frp_signal_session();

    void start();
    void release_obj();

#ifndef NETWORK_DISABLE_SSL
    void enable_ssl(asio::ssl::context& ssl_context);
#endif

    void enable_timeout(std::uint32_t timeout_sec);

    template <typename CommandData>
    void send_command(const CommandData& data) {
        auto packet = packet_frp_command_data(data);
        if (!packet) return;
        send_raw(packet);
    }

    bool is_authenticated() const {
        return authenticated_;
    }
    const std::string& get_uuid() const {
        return uuid_;
    }
    std::uint8_t get_nat_type() const {
        return nat_type_;
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
    void start_data_forward_read_loop();
    void process_command(std::string payload);
    void handle_initial_phase(const frp_command_base& command, std::string payload);
    void handle_server_hello_phase(const frp_command_base& command, std::string payload);
    void handle_authenticated_phase(const frp_command_base& command, std::string payload);
    void handle_register_services_phase(const frp_register_services_data& request);
    void handle_subscribe_services_phase(const frp_subscribe_services_data& request);
    void handle_channel_open_phase(const frp_channel_open_request_data& request);
    void handle_unknown_command(std::uint32_t command, const char* phase);
    void send_auth_failure_and_close(const std::string& message);
    void close_socket();
    void ssl_handshake();
    void send_raw(const std::shared_ptr<std::string>& packet);
    void send_raw(std::string packet);
    void send_raw(const void *data,std::size_t len);
    void upgrade(const upgrade_forward_function& forward_function, const upgrade_release_function& release_function);
    void reset_timeout_timer();

private:
    friend class frp_public_server;

    network_data_reference reference_;
    ::asio::ip::tcp::socket socket_;
    const asio::any_io_executor executor_;
    frp_public_server& owner_;
    std::uint32_t timeout_sec_ = 0;
    ::asio::steady_timer timeout_timer_;
    std::deque<std::shared_ptr<std::string>> write_queue_;
    std::array<std::uint8_t, 4> header_buf_ {};
    std::array<char, 16 * 1024> raw_read_buf_ {};
    std::string payload_;
    std::string server_nonce_;
    session_mode mode_           = session_mode::undecided;
    std::string uuid_            = "#not set#";
    std::string connection_uuid_ = "*";
    std::uint8_t nat_type_       = frp_nat_type_disabled;
    std::uint32_t startup_rtt_ms = 100;
    std::vector<frp_service_group> groups;

    bool authenticated_ = false;
    // 未知/异常命令计数熔断：超过阈值才释放，避免服务端成为重连风暴的主动方（ticket 03）
    std::size_t bad_command_cnt_ = 0;
    static constexpr std::size_t kMaxBadCommandCount = 10;
    upgrade_forward_function forward_cb_;
    upgrade_release_function release_cb_;
#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_;
    asio::ssl::context* ssl_context_ref_ = nullptr;
#endif
};

} // namespace network::proxy
