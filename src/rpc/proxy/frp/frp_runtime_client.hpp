#pragma once

#include "frp_client_upstream.hpp"
#include "frp_config.hpp"
#include "frp_runtime_command.hpp"

#include "network/network.hpp"

#include <array>
#include <cstring>
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
        if (!packet) return;
        asio::post(executor_, [this, self = shared_from_this(), packet = std::move(packet)]() mutable {
            if (!reference_.is_valid() || !upstream_) return;
            write_queue_.push_back(std::move(packet));
            if (write_queue_.size() == 1) {
                do_write();
            }
        });
    }

    // Send a pre-serialized JSON payload (used by frp_punch_engine)
    void send_raw_json(const std::string& json) {
        auto packet = std::make_shared<std::string>();
        std::uint32_t data_size = static_cast<std::uint32_t>(json.size());
        packet->resize(4 + data_size);
        Fundamental::net_buffer_copy(&data_size, packet->data(), 4);
        std::memcpy(packet->data() + 4, json.data(), data_size);
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

class frp_runtime_unified_client_agent
    : public std::enable_shared_from_this<frp_runtime_unified_client_agent>,
      private asio::noncopyable {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_runtime_unified_client_agent>(std::forward<Args>(args)...);
    }

    explicit frp_runtime_unified_client_agent(frp_proxy_client_config config);
    ~frp_runtime_unified_client_agent() = default;

    void start();
    void release_obj();

private:
    // --- structs ---
    struct provider_flow_runtime;
    struct accessor_session_context;
    struct listener_runtime;

    // --- signal ---
    void connect_signal_channel();
    void schedule_reconnect();
    void process_command(const frp_runtime_command_base& command, std::string payload);

    // --- provider side ---
    void register_all_services();
    void handle_prepare_flow(const frp_runtime_prepare_flow_data& request);
    void create_provider_punch_engine(const std::shared_ptr<provider_flow_runtime>& flow);
    void start_provider_backend_connect(const std::shared_ptr<provider_flow_runtime>& flow);
    void start_backend_read_loop(const std::shared_ptr<provider_flow_runtime>& flow);
    void handle_backend_write_queue(const std::shared_ptr<provider_flow_runtime>& flow);

    // --- time sync ---
    void run_time_sync();

    // --- accessor side ---
    void subscribe_all_keys();
    void reconcile_listeners(const std::vector<frp_runtime_visible_service_data>& services);
    void start_accept_loop(const std::shared_ptr<listener_runtime>& listener);
    void start_udp_receive_loop(const std::shared_ptr<listener_runtime>& listener);
    void create_accessor_punch_engine(const std::shared_ptr<accessor_session_context>& session);
    void request_flow(const std::shared_ptr<accessor_session_context>& session);
    void start_local_read_loop(const std::shared_ptr<accessor_session_context>& session);
    void handle_local_write_queue(const std::shared_ptr<accessor_session_context>& session);
    void fail_session(const std::shared_ptr<accessor_session_context>& session, const std::string& reason);

    // --- polling ---
    void start_polling();
    void do_poll();

private:
    network_data_reference reference_;
    frp_proxy_client_config config_;
    const std::string uuid_;
    asio::steady_timer reconnect_timer_;
    int reconnect_delay_seconds_ = 2;
    std::shared_ptr<frp_runtime_signal_client_channel> channel_;
    frp_runtime_nat_type probed_nat_type_ = frp_runtime_nat_type_disabled;
    std::uint32_t startup_rtt_ms_ = 100;
    std::int64_t server_clock_offset_us_ = 0;  // server_steady - local_steady (us)

    // provider state
    std::unordered_map<std::uint32_t, std::shared_ptr<provider_flow_runtime>> provider_flows_;
    std::unordered_map<std::string, frp_provider_service_config> services_by_name_;

    // accessor state
    std::unordered_map<std::string, std::shared_ptr<listener_runtime>> listeners_;
    std::unordered_map<std::uint32_t, std::shared_ptr<accessor_session_context>> accessor_sessions_;
    std::unordered_map<std::uint64_t, std::shared_ptr<accessor_session_context>> pending_sessions_;
    std::uint64_t next_session_id_ = 1;
    asio::steady_timer poll_timer_;

    // self-filter
    std::unordered_set<std::string> last_known_services_;
};

} // namespace network::proxy
