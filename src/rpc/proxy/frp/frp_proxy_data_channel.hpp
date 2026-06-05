#pragma once

#include "frp_runtime_command.hpp"
#include "frp_client_upstream.hpp"

#include "network/network.hpp"
#include "network/rudp/kcp_imp/ikcp.h"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace network::proxy
{

// frp_proxy_data_channel
//
// Unified data channel for a single FRP flow. Manages:
//   - TCP relay connection (always established first)
//   - KCP instance (runs over relay initially, switches to UDP on p2p upgrade)
//   - P2P keepalive (10s idle -> probe every 2s for 30s)
//   - Idle timeout
//
// P2P upgrade is handled externally by frp_punch_engine.
// When the punch succeeds, the caller calls accept_p2p() to hand off
// the UDP socket and peer endpoint. The data channel then switches
// KCP output from relay to P2P and starts keepalive.
//
// Lifecycle:
//   1. Construct with flow parameters
//   2. start() -- connects TCP relay, creates KCP (output -> TCP relay)
//   3. [optional] accept_p2p() -- hands off UDP socket, switches to P2P
//   4. release_obj() -- cleanup
//
// Thread safety: all operations must be called from the same executor strand.

class frp_proxy_data_channel : public std::enable_shared_from_this<frp_proxy_data_channel>,
                                private asio::noncopyable {
public:
    using connected_callback_t           = std::function<void()>;
    using disconnected_callback_t        = std::function<void()>;
    using data_callback_t                = std::function<void(std::string)>;
    using p2p_upgraded_callback_t        = std::function<void()>;

    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_proxy_data_channel>(std::forward<Args>(args)...);
    }

    frp_proxy_data_channel(const asio::any_io_executor& executor,
                           std::string relay_host,
                           std::string relay_service,
                           std::uint32_t flow_id,
                           std::string uuid,
                           std::string traffic_secret,
                           std::uint32_t idle_timeout_seconds = 120);

    ~frp_proxy_data_channel() = default;

    void enable_ssl(network_client_ssl_config config);
    void set_on_connected(connected_callback_t cb);
    void set_on_disconnected(disconnected_callback_t cb);
    void set_on_data(data_callback_t cb);
    void set_on_p2p_upgraded(p2p_upgraded_callback_t cb);

    // Start TCP relay connection and KCP
    void start();

    // Release all resources
    void release_obj();

    // Send application data through KCP (works in both relay and p2p mode)
    void send_bytes(const char* data, std::size_t size);

    // Accept the result of a successful punch.
    // Takes ownership of the UDP socket, switches KCP output to P2P,
    // releases relay, starts keepalive.
    void accept_p2p(std::shared_ptr<asio::ip::udp::socket> socket,
                    const asio::ip::udp::endpoint& peer_endpoint,
                    std::uint16_t local_port,
                    std::uint16_t peer_port);

    // Signal that a p2p signal handshake is in progress.
    // Prevents relay-disconnect notification during the confirmation window.
    void expect_p2p_disconnect();
    bool p2p_active_or_expected() const { return p2p_success_ || p2p_expected_; }

    // Returns true if p2p upgrade has completed
    bool is_p2p_active() const { return p2p_success_; }

    std::string local_relay_endpoint() const;
    std::string remote_relay_endpoint() const;
    std::string local_p2p_endpoint() const;

private:
    // --- relay ---
    void start_relay_read_loop();
    void relay_do_write();
    void notify_disconnect_once();

    // --- KCP ---
    void kcp_send_raw(const char* data, std::size_t size);
    void schedule_kcp_update();
    void kcp_recv_loop();

    // --- p2p switch ---
    void switch_to_p2p();
    void schedule_p2p_idle_timer();
    void do_keepalive_probe();
    void reset_keepalive_timer();

    // --- p2p read loop (keepalive + KCP data) ---
    void start_p2p_read_loop();

    // --- idle timeout ---
    void reset_activity_timestamp();
    void schedule_idle_check();

    // --- helpers ---
    static std::int32_t kcp_output_callback(const char* buf, int len, ikcpcb*, void* user);

private:
    struct kcp_releaser {
        void operator()(ikcpcb* ptr) const noexcept;
    };

    struct send_context {
        frp_proxy_data_channel* self = nullptr;
    };

    network_data_reference reference_;
    const asio::any_io_executor executor_;

    // relay
    const std::string relay_host_;
    const std::string relay_service_;
    const std::uint32_t flow_id_;
    const std::string uuid_;
    network_client_ssl_config ssl_config_;
    std::shared_ptr<frp_client_upstream> relay_upstream_;
    std::shared_ptr<proxy_upstream_interface> relay_transport_;
    std::deque<std::shared_ptr<std::string>> relay_write_queue_;
    std::array<char, 16 * 1024> relay_read_buf_{};
    bool relay_connected_ = false;
    bool disconnect_notified_ = false;

    // KCP
    const std::string traffic_secret_;
    std::unique_ptr<ikcpcb, kcp_releaser> kcp_;
    asio::steady_timer kcp_update_timer_;
    bool kcp_active_ = false;
    send_context kcp_send_ctx_;
    std::vector<std::uint8_t> kcp_traffic_key_;

    // P2P (populated by accept_p2p)
    std::shared_ptr<asio::ip::udp::socket> p2p_socket_;
    asio::ip::udp::endpoint p2p_peer_endpoint_;
    asio::ip::udp::endpoint p2p_recv_endpoint_;
    std::array<char, 16 * 1024> p2p_read_buf_{};
    bool p2p_success_ = false;
    bool p2p_expected_ = false;  // relay close tolerated (handshake in progress)

    // keepalive
    asio::steady_timer p2p_timer_;
    int keepalive_probe_count_ = 0;
    bool keepalive_probing_ = false;

    // idle timeout
    const std::uint32_t idle_timeout_seconds_;
    std::chrono::steady_clock::time_point last_activity_time_;
    asio::steady_timer idle_check_timer_;

    // callbacks
    connected_callback_t on_connected_;
    disconnected_callback_t on_disconnected_;
    data_callback_t on_data_;
    p2p_upgraded_callback_t on_p2p_upgraded_;
};

} // namespace network::proxy
