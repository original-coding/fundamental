#pragma once

#include "frp_runtime_command.hpp"

#include "network/network.hpp"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace network::proxy
{

// frp_punch_engine
//
// Self-contained NAT hole-punching component. Lifecycle:
//   1. create(config, signal_sender) -- configure with flow params + signal callback
//   2. start() -- bind UDP socket, begin endpoint probe to public server
//   3. on_peer_info() -- caller delivers server's flow_p2p_peer response
//      -> internally starts UDP punch (symmetric or cone)
//   4. on_punch_confirm / ack / ok -- caller delivers signal-channel handshake messages
//   5a. on_success(punch_result) -- punch succeeded; caller takes socket + peer endpoint
//   5b. on_failed() -- punch exhausted all rounds; caller destroys engine, may retry
//
// On retry: destroy the old instance, create a new one. No state leaks between attempts.
//
// Thread safety: all methods must be called from the same executor strand.

class frp_punch_engine : public std::enable_shared_from_this<frp_punch_engine>,
                          private asio::noncopyable {
public:
    struct config {
        asio::any_io_executor executor;
        std::uint32_t flow_id = 0;
        std::string uuid;
        std::string peer_uuid;
        std::string traffic_secret;
        std::string public_server_host;
        std::uint16_t public_server_udp_port = 0;
        std::uint8_t my_nat_type = 0;          // frp_runtime_nat_type_*
        std::uint32_t my_rtt_ms = 100;
    };

    struct peer_info {
        std::string host;
        std::uint16_t port = 0;
        std::uint8_t nat_type = 0;
        std::uint32_t rtt_ms = 100;
    };

    struct punch_result {
        std::shared_ptr<asio::ip::udp::socket> socket;
        asio::ip::udp::endpoint peer_endpoint;
        std::uint16_t local_port = 0;
        std::uint16_t peer_port = 0;
    };

    // Signal-channel output: engine calls this with a pre-serialized JSON payload
    // for punch_confirm / punch_confirm_ack / punch_confirm_ok.
    using signal_sender = std::function<void(std::string json_payload)>;

    using success_callback = std::function<void(punch_result)>;
    using failed_callback  = std::function<void()>;

    static std::shared_ptr<frp_punch_engine> create(config cfg, signal_sender sender);

    frp_punch_engine(config cfg, signal_sender sender);
    ~frp_punch_engine();

    // Begin: bind UDP socket + send endpoint probe to public server
    void start();

    // Called when server returns flow_p2p_peer (via signal channel).
    // Cancels endpoint probe, stores peer info.
    void on_peer_info(peer_info peer);

    // Start punch at the given absolute local steady_clock deadline (us).
    // Sets a timer to fire at the deadline, then calls start_udp_punch().
    void start_punch_at(std::int64_t local_deadline_us);

    // Called by external owner to send punch_confirm via signal channel
    void send_punch_confirm(std::uint16_t local_port, std::uint16_t peer_port,
                            std::uint16_t external_local_port, std::uint16_t external_peer_port);

    // Signal-channel handshake messages (caller delivers after deserializing).
    // Port arguments are as they appear in the wire command (caller swaps if needed).
    void on_punch_confirm(std::uint16_t local_port, std::uint16_t peer_port,
                          std::uint16_t external_local_port, std::uint16_t external_peer_port);
    void on_punch_confirm_ack(std::uint16_t local_port, std::uint16_t peer_port,
                              std::uint16_t external_local_port, std::uint16_t external_peer_port);
    void on_punch_confirm_ok(std::uint16_t local_port, std::uint16_t peer_port,
                             std::uint16_t external_local_port, std::uint16_t external_peer_port);

    // Called when a probe match is detected. Caller decides whether to
    // confirm by calling send_punch_confirm() on the engine.
    // local_port: own internal port
    // peer_port: peer internal port (from probe)
    // target_port: external port being targeted (from probe)
    // peer_external_port: peer external (source port of received packet)
    using probe_match_callback = std::function<void(std::uint16_t local_port, std::uint16_t peer_port,
                                                     std::uint16_t target_port, std::uint16_t peer_external_port)>;
    void set_on_probe_match(probe_match_callback cb) { on_probe_match_ = std::move(cb); }
    bool is_valid_probe_pair(std::uint16_t local_port, std::uint16_t tgt_port) const;

    using endpoint_ready_callback = std::function<void(std::string ip, std::uint16_t port)>;
    void set_on_endpoint_ready(endpoint_ready_callback cb) { on_endpoint_ready_ = std::move(cb); }
    using p2p_imminent_callback = std::function<void()>;
    void set_on_p2p_imminent(p2p_imminent_callback cb) { on_p2p_imminent_ = std::move(cb); }

    void set_on_success(success_callback cb);
    void set_on_failed(failed_callback cb);

    // Force cleanup (cancels timers, closes sockets)
    void release();

private:
    // --- endpoint probe ---
    void start_endpoint_echo_loop();
    void do_endpoint_probe();

    // --- synchronized punch ---
    void start_udp_punch();
    void do_punch_round();
    void rebuild_symmetric_sockets();

    // p2p read loop (punch phase) -- handles 6-byte punch probes
    void start_punch_read_loop();

    // --- signal handshake ---
    void send_punch_confirm_ack(std::uint16_t local_port, std::uint16_t peer_port,
                                 std::uint16_t external_local_port, std::uint16_t external_peer_port);
    void send_punch_confirm_ok(std::uint16_t local_port, std::uint16_t peer_port,
                                std::uint16_t external_local_port, std::uint16_t external_peer_port);
    void on_punch_success(std::uint16_t local_port, std::uint16_t peer_port,
                                             std::uint16_t external_local_port, std::uint16_t external_peer_port);

private:
    network_data_reference reference_;
    const asio::any_io_executor executor_;

    // config
    const std::uint32_t flow_id_;
    const std::string uuid_;
    std::string peer_uuid_;
    const std::string traffic_secret_;
    const std::string public_server_host_;
    const std::uint16_t public_server_udp_port_;
    const std::uint8_t my_nat_type_;
    std::uint32_t my_rtt_ms_ = 100;

    // signal output
    signal_sender signal_sender_;

    // peer info
    asio::ip::udp::endpoint p2p_peer_endpoint_;
    std::uint8_t peer_nat_type_ = 0; // frp_runtime_nat_type_disabled
    std::uint32_t peer_rtt_ms_ = 100;

    // UDP socket (created in start(), handed off on success)
    std::shared_ptr<asio::ip::udp::socket> p2p_socket_;
    asio::ip::udp::endpoint p2p_recv_endpoint_;
    std::array<char, 16 * 1024> p2p_read_buf_{};

    // endpoint probe
    asio::steady_timer endpoint_probe_timer_;
    int endpoint_probe_attempts_ = 0;
    bool probing_ = false;
    std::string my_external_ip_;
    std::uint16_t my_external_port_ = 0;

    // synchronized punch
    asio::steady_timer deadline_timer_;
    // punch
    asio::steady_timer punch_timer_;
    std::vector<std::shared_ptr<asio::ip::udp::socket>> punch_sockets_;
    std::unordered_set<std::uint16_t> current_cone_targets_; // cone-side probe targets for validation
    int punch_round_ = 0;
    bool punch_active_ = false;
    bool punch_done_ = false;
    int punch_socket_gen_ = 0;

    // signal handshake
    bool confirm_started_ = false;
    probe_match_callback on_probe_match_;
    // result delivered guard
    bool result_delivered_ = false;

    // callbacks
    endpoint_ready_callback on_endpoint_ready_;
    p2p_imminent_callback on_p2p_imminent_;
    success_callback on_success_;
    failed_callback on_failed_;
};

} // namespace network::proxy
