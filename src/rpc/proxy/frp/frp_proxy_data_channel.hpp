#pragma once

#include "frp_runtime_command.hpp"
#include "frp_client_upstream.hpp"

#include "network/network.hpp"
#include "network/rudp/kcp_imp/ikcp.h"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace network::proxy
{

// frp_proxy_data_channel
//
// Unified data channel for a single FRP flow. Manages:
//   - TCP relay connection (always established first)
//   - KCP instance (runs over relay initially, switches to UDP on p2p upgrade)
//   - UDP socket + punch logic (created on start_p2p_upgrade())
//
// Lifecycle:
//   1. Construct with flow parameters
//   2. start() -- connects TCP relay, creates KCP (output -> TCP relay)
//   3. [optional] start_p2p_upgrade() -- begins UDP endpoint probe
//   4. [optional] set_p2p_peer() -- triggers UDP punch after server sends peer info
//   5. On punch success: KCP output switches to UDP, TCP relay released
//
// Thread safety: all operations must be called from the same executor strand.

class frp_proxy_data_channel : public std::enable_shared_from_this<frp_proxy_data_channel>,
                                private asio::noncopyable {
public:
    using connected_callback_t           = std::function<void()>;
    using disconnected_callback_t        = std::function<void()>;
    using data_callback_t                = std::function<void(std::string)>;
    using p2p_upgraded_callback_t        = std::function<void()>;
    using p2p_upgrade_failed_callback_t  = std::function<void()>;

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
                           std::string public_server_host,
                           std::uint16_t public_server_udp_port,
                           std::uint8_t my_nat_type);

    ~frp_proxy_data_channel() = default;

    void enable_ssl(network_client_ssl_config config);
    void set_on_connected(connected_callback_t cb);
    void set_on_disconnected(disconnected_callback_t cb);
    void set_on_data(data_callback_t cb);
    void set_on_p2p_upgraded(p2p_upgraded_callback_t cb);
    void set_on_p2p_upgrade_failed(p2p_upgrade_failed_callback_t cb);

    // Start TCP relay connection and KCP
    void start();

    // Release all resources
    void release_obj();

    // Send application data through KCP (works in both relay and p2p mode)
    void send_bytes(const char* data, std::size_t size);

    // Begin UDP endpoint probe for p2p upgrade (call after relay is ready)
    void start_p2p_upgrade();

    // Provide peer endpoint info from server's flow_p2p_peer command
    void set_p2p_peer(const std::string& peer_host,
                      std::uint16_t peer_port,
                      std::uint8_t peer_nat_type,
                      std::uint32_t peer_rtt_ms);
    void set_my_rtt_ms(std::uint32_t rtt_ms) { my_rtt_ms_ = rtt_ms; }

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

    // --- endpoint probe ---
    void do_endpoint_probe();

    // --- udp punch ---
    void start_udp_punch();
    void start_p2p_read_loop();
    void do_punch_round();

    // --- p2p switch ---
    void switch_to_p2p();
    void schedule_p2p_idle_timer();
    void do_keepalive_probe();
    void reset_keepalive_timer();

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
    std::array<char, 16 * 1024> relay_read_buf_ {};
    bool relay_connected_ = false;
    bool disconnect_notified_ = false;

    // p2p upgrade parameters
    const std::string traffic_secret_;
    const std::string public_server_host_;
    const std::uint16_t public_server_udp_port_;
    const std::uint8_t my_nat_type_;

    // KCP
    std::unique_ptr<ikcpcb, kcp_releaser> kcp_;
    asio::steady_timer kcp_update_timer_;
    send_context kcp_send_ctx_;
    std::vector<std::uint8_t> kcp_traffic_key_;

    // UDP / punch
    std::unique_ptr<asio::ip::udp::socket> p2p_socket_;
    asio::ip::udp::endpoint p2p_peer_endpoint_;
    asio::ip::udp::endpoint p2p_recv_endpoint_;
    std::array<char, 16 * 1024> p2p_read_buf_ {};
    asio::steady_timer endpoint_probe_timer_;
    asio::steady_timer punch_timer_;
    asio::steady_timer p2p_timer_;
    std::vector<std::unique_ptr<asio::ip::udp::socket>> punch_sockets_;
    std::set<std::uint16_t> punch_scanned_ports_;
    std::uint8_t peer_nat_type_ = frp_runtime_nat_type_disabled;
    int punch_round_ = 0;
    std::size_t endpoint_probe_attempts_ = 0;
    bool awaiting_endpoint_ready_ = false;
    bool punch_active_ = false;
    bool punch_done_ = false;   // local punch matched (2+ reflected probes)
    bool p2p_success_ = false;  // switch_to_p2p() called
    bool upgrade_started_ = false;
    int punch_match_count_ = 0;  // count of probes with reflected_port == my_port
    bool confirmation_sent_ = false;  // we have replied with correct peer_port
    int keepalive_probe_count_ = 0;   // consecutive keepalive probes without response
    bool keepalive_probing_ = false;  // in 2s probing state (after 10s idle)
    std::uint64_t endpoint_probe_send_ts_ = 0; // timestamp sent in endpoint probe
    std::uint32_t my_rtt_ms_ = 100;    // RTT to server, default 100ms
    std::uint32_t peer_rtt_ms_ = 100;  // peer's RTT to server, default 100ms

    // callbacks
    connected_callback_t on_connected_;
    disconnected_callback_t on_disconnected_;
    data_callback_t on_data_;
    p2p_upgraded_callback_t on_p2p_upgraded_;
    p2p_upgrade_failed_callback_t on_p2p_upgrade_failed_;
};

} // namespace network::proxy
