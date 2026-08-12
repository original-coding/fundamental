#pragma once

#include "frp_client_command.hpp"

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
// Self-contained NAT hole-punching component. Adapted from old frp_punch_engine
// for the new architecture: connection_uuid instead of flow_id, P2P messages
// tunneled through frp_forward_command.

class frp_punch_engine : public std::enable_shared_from_this<frp_punch_engine>,
                          private asio::noncopyable {
public:
    struct config {
        asio::any_io_executor executor;
        std::string connection_uuid;
        std::string peer_uuid;
        std::string traffic_secret;
        std::string public_server_host;
        std::uint16_t public_server_udp_port = 0;
        std::uint8_t my_nat_type = 0;
        std::uint32_t my_rtt_ms = 100;
        std::uint32_t punch_seq = 0;
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
    using signal_sender = std::function<void(std::string json_payload)>;

    using success_callback = std::function<void(punch_result)>;
    using failed_callback  = std::function<void()>;

    static std::shared_ptr<frp_punch_engine> create(config cfg, signal_sender sender);

    frp_punch_engine(config cfg, signal_sender sender);
    ~frp_punch_engine();

    void start();
    void on_peer_info(peer_info peer);
    void start_punch_at(std::int64_t local_deadline_us);

    // Provider: called when probe match detected on symmetric sockets
    void send_provider_probe_match(std::uint16_t local_port, std::uint16_t peer_port,
                                   std::uint16_t target_port, std::uint16_t peer_external_port);

    // Accessor: incoming provider_probe_match → sends accessor_punch_confirm
    void on_provider_probe_match(std::uint16_t local_port, std::uint16_t peer_port,
                                 std::uint16_t external_local_port, std::uint16_t external_peer_port);

    // Provider: incoming accessor_punch_confirm → sends provider_confirm_ack
    void on_accessor_punch_confirm(std::uint16_t local_port, std::uint16_t peer_port,
                                   std::uint16_t external_local_port, std::uint16_t external_peer_port);

    // Accessor: incoming provider_confirm_ack → p2p ready, sends accessor_confirm_ok
    void on_provider_confirm_ack(std::uint16_t local_port, std::uint16_t peer_port,
                                 std::uint16_t external_local_port, std::uint16_t external_peer_port);

    // Provider: incoming accessor_confirm_ok → p2p ready
    void on_accessor_confirm_ok(std::uint16_t local_port, std::uint16_t peer_port,
                                std::uint16_t external_local_port, std::uint16_t external_peer_port);

    bool is_valid_probe_pair(std::uint16_t local_port, std::uint16_t tgt_port) const;

    using probe_match_callback = std::function<void(std::uint16_t local_port, std::uint16_t peer_port,
                                                     std::uint16_t target_port, std::uint16_t peer_external_port)>;
    void set_on_probe_match(probe_match_callback cb) { on_probe_match_ = std::move(cb); }

    using endpoint_ready_callback = std::function<void(std::string ip, std::uint16_t port)>;
    void set_on_endpoint_ready(endpoint_ready_callback cb) { on_endpoint_ready_ = std::move(cb); }
    using p2p_imminent_callback = std::function<void()>;
    void set_on_p2p_imminent(p2p_imminent_callback cb) { on_p2p_imminent_ = std::move(cb); }

    void set_on_success(success_callback cb);
    void set_on_failed(failed_callback cb);

    void release();

private:
    void start_endpoint_echo_loop();
    void do_endpoint_probe();
    void start_udp_punch();
    void do_punch_round();
    void rebuild_symmetric_sockets();
    void start_punch_read_loop();

    void on_punch_success(std::uint16_t local_port, std::uint16_t peer_port,
                          std::uint16_t external_local_port, std::uint16_t external_peer_port);

    network_data_reference reference_;
    const asio::any_io_executor executor_;

    const std::string connection_uuid_;
    std::string peer_uuid_;
    const std::string traffic_secret_;
    const std::string public_server_host_;
    const std::uint16_t public_server_udp_port_;
    const std::uint8_t my_nat_type_;
    std::uint32_t my_rtt_ms_ = 100;
    std::uint32_t punch_tag_ = 0;

    signal_sender signal_sender_;

    asio::ip::udp::endpoint p2p_peer_endpoint_;
    std::uint8_t peer_nat_type_ = 0;
    std::uint32_t peer_rtt_ms_ = 100;

    std::shared_ptr<asio::ip::udp::socket> p2p_socket_;
    asio::ip::udp::endpoint p2p_recv_endpoint_;
    std::array<char, 16 * 1024> p2p_read_buf_{};

    asio::steady_timer endpoint_probe_timer_;
    std::size_t endpoint_probe_attempts_ = 0;
    bool probing_ = false;
    std::string my_external_ip_;
    std::uint16_t my_external_port_ = 0;

    asio::steady_timer deadline_timer_;
    asio::steady_timer punch_timer_;
    std::vector<std::shared_ptr<asio::ip::udp::socket>> punch_sockets_;
    std::unordered_set<std::uint16_t> current_cone_targets_;
    int punch_round_ = 0;
    bool punch_active_ = false;
    bool punch_done_ = false;
    int punch_socket_gen_ = 0;

    bool confirm_started_ = false;
    probe_match_callback on_probe_match_;
    bool result_delivered_ = false;

    endpoint_ready_callback on_endpoint_ready_;
    p2p_imminent_callback on_p2p_imminent_;
    success_callback on_success_;
    failed_callback on_failed_;
};

} // namespace network::proxy
