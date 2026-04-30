#include "frp_proxy_data_channel.hpp"

#include "frp_runtime_command.hpp"
#include "frp_runtime_common.hpp"
#include "frp_kcp_crypto.hpp"
#include "network/rudp/kcp_imp/ikcp.h"
#include "network/network.hpp"

#include "fundamental/basic/log.h"
#include "fundamental/basic/string_utils.hpp"
#include "fundamental/rttr_handler/serializer.h"

#include <algorithm>
#include <cstring>
#include <random>

namespace network::proxy
{

namespace
{

static constexpr std::size_t kMaxEndpointProbeAttempts = 10;
static constexpr int kPunchMaxRounds      = 32;
static constexpr int kPunchSocketCount    = 32;
static constexpr int kPunchScanRound0     = 64;
static constexpr int kPunchScanPerRound   = 128;
static constexpr int kPunchRetransmitCount = 5;
static constexpr int kPunchRetransmitMs   = 100;
static constexpr int kPunchRoundMs        = 1000;

std::optional<asio::ip::udp::endpoint> resolve_udp_ep(const asio::any_io_executor& executor,
                                                       const std::string& host,
                                                       std::uint16_t port) {
    std::error_code ec;
    auto address = asio::ip::make_address(host, ec);
    if (!ec && !address.is_v6()) {
        return asio::ip::udp::endpoint(address, port);
    }
    asio::ip::udp::resolver resolver(executor);
    auto endpoints = resolver.resolve(asio::ip::udp::v4(), host, std::to_string(port), ec);
    if (ec || endpoints.begin() == endpoints.end()) return std::nullopt;
    return endpoints.begin()->endpoint();
}

} // namespace

// ---------------------------------------------------------------------------
// kcp_releaser
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::kcp_releaser::operator()(ikcpcb* ptr) const noexcept {
    if (ptr) ikcp_release(ptr);
}

// ---------------------------------------------------------------------------
// KCP output callback (static)
// ---------------------------------------------------------------------------

std::int32_t frp_proxy_data_channel::kcp_output_callback(const char* buf, int len, ikcpcb*, void* user) {
    auto* ctx = static_cast<send_context*>(user);
    if (!ctx || !ctx->self) return 0;
    auto* self = ctx->self;

    auto payload = std::make_shared<std::string>(buf, len);

    if (self->p2p_success_ && self->p2p_socket_ && self->p2p_peer_endpoint_.port() != 0) {
        // p2p mode: send via UDP
        self->p2p_socket_->async_send_to(
            asio::buffer(*payload), self->p2p_peer_endpoint_,
            [payload](const std::error_code&, std::size_t) {});
    } else if (self->relay_transport_) {
        // relay mode: send via TCP relay
        // wrap in a shared_ptr for the write queue
        asio::post(self->executor_, [self_weak = self->weak_from_this(), payload]() mutable {
            auto self = self_weak.lock();
            if (!self || !self->reference_.is_valid() || !self->relay_transport_) return;
            self->relay_write_queue_.push_back(std::move(payload));
            if (self->relay_write_queue_.size() == 1) {
                self->relay_do_write();
            }
        });
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

frp_proxy_data_channel::frp_proxy_data_channel(const asio::any_io_executor& executor,
                                               std::string relay_host,
                                               std::string relay_service,
                                               std::uint32_t flow_id,
                                               std::string uuid,
                                               std::string traffic_secret,
                                               std::string public_server_host,
                                               std::uint16_t public_server_udp_port,
                                               std::uint8_t my_nat_type)
    : executor_(executor),
      relay_host_(std::move(relay_host)),
      relay_service_(std::move(relay_service)),
      flow_id_(flow_id),
      uuid_(std::move(uuid)),
      traffic_secret_(std::move(traffic_secret)),
      public_server_host_(std::move(public_server_host)),
      public_server_udp_port_(public_server_udp_port),
      my_nat_type_(my_nat_type),
      kcp_update_timer_(executor),
      endpoint_probe_timer_(executor),
      punch_timer_(executor),
      p2p_timer_(executor) {
    kcp_send_ctx_.self = this;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::enable_ssl(network_client_ssl_config config) {
    ssl_config_ = std::move(config);
}

void frp_proxy_data_channel::set_on_connected(connected_callback_t cb)           { on_connected_ = std::move(cb); }
void frp_proxy_data_channel::set_on_disconnected(disconnected_callback_t cb)     { on_disconnected_ = std::move(cb); }
void frp_proxy_data_channel::set_on_data(data_callback_t cb)                     { on_data_ = std::move(cb); }
void frp_proxy_data_channel::set_on_p2p_upgraded(p2p_upgraded_callback_t cb)     { on_p2p_upgraded_ = std::move(cb); }
void frp_proxy_data_channel::set_on_p2p_upgrade_failed(p2p_upgrade_failed_callback_t cb) { on_p2p_upgrade_failed_ = std::move(cb); }

void frp_proxy_data_channel::release_obj() {
    if (!reference_.release()) return;
    kcp_update_timer_.cancel();
    endpoint_probe_timer_.cancel();
    punch_timer_.cancel();
    p2p_timer_.cancel();
    kcp_.reset();
    for (auto& s : punch_sockets_) { if (s) { std::error_code ec; s->close(ec); } }
    punch_sockets_.clear();
    if (p2p_socket_) { std::error_code ec; p2p_socket_->close(ec); p2p_socket_.reset(); }
    if (relay_upstream_) { relay_upstream_->release_obj(); relay_upstream_ = nullptr; }
    relay_transport_ = nullptr;
}

std::string frp_proxy_data_channel::local_relay_endpoint() const {
    if (!relay_upstream_) return {};
    return relay_upstream_->local_endpoint_string();
}

std::string frp_proxy_data_channel::remote_relay_endpoint() const {
    if (!relay_upstream_) return {};
    return relay_upstream_->remote_endpoint_string();
}

std::string frp_proxy_data_channel::local_p2p_endpoint() const {
    if (!p2p_socket_) return {};
    std::error_code ec;
    auto ep = p2p_socket_->local_endpoint(ec);
    if (ec) return {};
    return Fundamental::StringFormat("{}:{}", ep.address().to_string(), ep.port());
}

// ---------------------------------------------------------------------------
// start() -- connect TCP relay, create KCP
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::start() {
    // Derive symmetric KCP encryption key (both sides use the same key for this flow)
    kcp_traffic_key_ = frp_derive_kcp_flow_key(traffic_secret_, flow_id_);

    // Create KCP instance (output -> relay initially via kcp_output_callback)
    kcp_ = std::unique_ptr<ikcpcb, kcp_releaser>(ikcp_create(flow_id_, &kcp_send_ctx_));
    ikcp_setoutput(kcp_.get(), kcp_output_callback);
    kcp_.get()->stream = 0;
    ikcp_wndsize(kcp_.get(), 256, 256);
    ikcp_nodelay(kcp_.get(), 1, 20, 2, 1);
    schedule_kcp_update();

    // Connect TCP relay
    relay_upstream_ = std::make_shared<frp_client_upstream>(executor_, relay_host_, relay_service_);
    relay_upstream_->enable_ssl(ssl_config_);
    relay_upstream_->notify_connect_result.Connect(
        shared_from_this(),
        [this, self = shared_from_this()](Fundamental::error_code ec,
                                          std::shared_ptr<frp_client_upstream> upstream) {
            if (!reference_.is_valid()) return;
            if (ec || !upstream) {
                notify_disconnect_once();
                return;
            }
            relay_upstream_ = std::move(upstream);
            relay_transport_ = relay_upstream_;

            // Send data open handshake
            frp_runtime_data_open_data open_req;
            open_req.command = frp_runtime_data_open_command;
            open_req.flow_id = flow_id_;
            open_req.uuid    = uuid_;
            auto pkt = packet_frp_runtime_command_data(open_req);
            relay_write_queue_.push_back(std::move(pkt));
            relay_do_write();

            relay_connected_ = true;
            if (on_connected_) on_connected_();
            start_relay_read_loop();
        });
    relay_upstream_->start_async_connect();
}

// ---------------------------------------------------------------------------
// send_bytes -- application data -> KCP
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::send_bytes(const char* data, std::size_t size) {
    if (!reference_.is_valid() || !kcp_) return;
    kcp_send_raw(data, size);
}

void frp_proxy_data_channel::kcp_send_raw(const char* data, std::size_t size) {
    if (!kcp_) return;
    auto encrypted = frp_kcp_encrypt(kcp_traffic_key_,
                                     std::vector<std::uint8_t>(data, data + size));
    if (encrypted.empty()) return;
    ikcp_send(kcp_.get(), reinterpret_cast<const char*>(encrypted.data()),
              static_cast<int>(encrypted.size()));
    ikcp_update(kcp_.get(), static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() & 0xffffffffu));
}

// ---------------------------------------------------------------------------
// Relay read loop -- feeds received bytes into KCP
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::start_relay_read_loop() {
    if (!reference_.is_valid() || !relay_transport_) return;
    relay_transport_->async_buffers_read_some(
        { network_read_buffer_t { relay_read_buf_.data(), relay_read_buf_.size() } },
        [this, self = shared_from_this()](std::error_code ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (ec || bytes_read == 0) {
                if (p2p_success_) return; // Expected after p2p switch -- relay released
                // Peer may have completed punch and released its relay.
                // If we have already sent confirmation probes, treat as p2p success.
                if (confirmation_sent_ && punch_active_ && !p2p_success_) {
                    FINFO("frp_proxy_data_channel flow_id={} relay closed during punch, treating as p2p success",
                          flow_id_);
                    punch_active_ = false;
                    punch_done_ = true;
                    p2p_success_ = true;
                    punch_timer_.cancel();
                    switch_to_p2p();
                    return;
                }
                notify_disconnect_once();
                return;
            }
            if (p2p_success_) {
                // Already switched to p2p -- discard relay data
                start_relay_read_loop();
                return;
            }
            // Feed into KCP
            ikcp_input(kcp_.get(), relay_read_buf_.data(), static_cast<long>(bytes_read));
            auto now = static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count() & 0xffffffffu);
            ikcp_update(kcp_.get(), now);
            kcp_recv_loop();
            start_relay_read_loop();
        });
}

void frp_proxy_data_channel::relay_do_write() {
    if (!relay_transport_ || relay_write_queue_.empty()) return;
    auto& current = relay_write_queue_.front();
    relay_transport_->async_buffers_write(
        { network_write_buffer_t { current->data(), current->size() } },
        [this, self = shared_from_this()](std::error_code ec, std::size_t) {
            if (!reference_.is_valid()) return;
            if (ec || !relay_transport_) {
                release_obj();
                return;
            }
            relay_write_queue_.pop_front();
            if (!relay_write_queue_.empty()) relay_do_write();
        });
}

void frp_proxy_data_channel::notify_disconnect_once() {
    if (disconnect_notified_) return;
    disconnect_notified_ = true;
    if (on_disconnected_) on_disconnected_();
}

// ---------------------------------------------------------------------------
// KCP update timer + recv loop
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::schedule_kcp_update() {
    if (!reference_.is_valid() || !kcp_) return;
    kcp_update_timer_.expires_after(std::chrono::milliseconds(20));
    kcp_update_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {
        if (ec || !reference_.is_valid() || !kcp_) return;
        auto now = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() & 0xffffffffu);
        ikcp_update(kcp_.get(), now);
        kcp_recv_loop();
        schedule_kcp_update();
    });
}

void frp_proxy_data_channel::kcp_recv_loop() {
    if (!kcp_) return;
    std::array<char, 16 * 1024> buf {};
    while (true) {
        auto n = ikcp_recv(kcp_.get(), buf.data(), static_cast<int>(buf.size()));
        if (n < 0) break;
        std::vector<std::uint8_t> encrypted(buf.data(), buf.data() + n);
        auto plaintext = frp_kcp_decrypt(kcp_traffic_key_, encrypted);
        if (!plaintext) {
            FINFO("frp_proxy_data_channel flow_id={} kcp_recv decrypt failed size={}", flow_id_, n);
            continue;
        }
        if (on_data_) {
            on_data_(std::string(reinterpret_cast<const char*>(plaintext->data()), plaintext->size()));
        }
    }
}


// ---------------------------------------------------------------------------
// start_p2p_upgrade() -- begin endpoint probe
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::start_p2p_upgrade() {
    if (!reference_.is_valid() || upgrade_started_ || public_server_udp_port_ == 0) {
        if (on_p2p_upgrade_failed_) on_p2p_upgrade_failed_();
        return;
    }
    upgrade_started_ = true;

    p2p_socket_ = std::make_unique<asio::ip::udp::socket>(executor_);
    protocal_helper::udp_bind_endpoint(*p2p_socket_, 0);
    start_p2p_read_loop();

    awaiting_endpoint_ready_ = true;
    endpoint_probe_attempts_ = 0;
    std::error_code ec;
    auto local_port = p2p_socket_->local_endpoint(ec).port();
    FINFO("frp_proxy_data_channel flow_id={} start_p2p_upgrade local_port={}", flow_id_, local_port);
    do_endpoint_probe();
}

void frp_proxy_data_channel::do_endpoint_probe() {
    if (!reference_.is_valid() || !awaiting_endpoint_ready_ || !p2p_socket_) return;
    if (endpoint_probe_attempts_ >= kMaxEndpointProbeAttempts) {
        FINFO("frp_proxy_data_channel flow_id={} endpoint_probe timeout", flow_id_);
        awaiting_endpoint_ready_ = false;
        if (on_p2p_upgrade_failed_) on_p2p_upgrade_failed_();
        return;
    }

    std::error_code ec;
    auto local_port = p2p_socket_->local_endpoint(ec).port();

    frp_runtime_p2p_probe_data probe;
    probe.command    = frp_runtime_p2p_probe_command;
    probe.flow_id    = flow_id_;
    probe.uuid       = uuid_;
    probe.local_ip   = "";
    probe.local_port = local_port;
    auto payload   = Fundamental::io::to_json(probe);
    auto encrypted = frp_kcp_encrypt_string(frp_derive_kcp_flow_key(traffic_secret_, 0), payload);
    if (encrypted.empty()) {
        if (on_p2p_upgrade_failed_) on_p2p_upgrade_failed_();
        return;
    }
    auto server_ep = resolve_udp_ep(executor_, public_server_host_, public_server_udp_port_);
    if (!server_ep) {
        if (on_p2p_upgrade_failed_) on_p2p_upgrade_failed_();
        return;
    }

    endpoint_probe_attempts_++;
    FINFO("frp_proxy_data_channel flow_id={} endpoint_probe attempt={} local_port={} server={}:{}",
          flow_id_, endpoint_probe_attempts_, local_port,
          public_server_host_, public_server_udp_port_);

    auto enc_ptr = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted));
    p2p_socket_->async_send_to(asio::buffer(*enc_ptr), *server_ep,
                               [enc_ptr](const std::error_code&, std::size_t) {});

    endpoint_probe_timer_.expires_after(std::chrono::milliseconds(200));
    endpoint_probe_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {
        if (ec || !reference_.is_valid()) return;
        do_endpoint_probe();
    });
}

// ---------------------------------------------------------------------------
// set_p2p_peer() -- called when server sends flow_p2p_peer
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::set_p2p_peer(const std::string& peer_host,
                                           std::uint16_t peer_port,
                                           std::uint8_t peer_nat_type) {
    if (!reference_.is_valid() || !p2p_socket_) return;
    awaiting_endpoint_ready_ = false;
    endpoint_probe_timer_.cancel();

    std::error_code ec;
    auto addr = asio::ip::make_address(peer_host, ec);
    if (ec) {
        FERR("frp_proxy_data_channel flow_id={} set_p2p_peer bad address {}", flow_id_, peer_host);
        if (on_p2p_upgrade_failed_) on_p2p_upgrade_failed_();
        return;
    }
    p2p_peer_endpoint_ = asio::ip::udp::endpoint(addr, peer_port);
    peer_nat_type_      = peer_nat_type;

    FINFO("frp_proxy_data_channel flow_id={} set_p2p_peer peer={}:{} peer_nat_type={}",
          flow_id_, peer_host, peer_port, static_cast<int>(peer_nat_type));

    if (peer_nat_type == frp_runtime_nat_type_disabled || my_nat_type_ == frp_runtime_nat_type_disabled) {
        FINFO("frp_proxy_data_channel flow_id={} set_p2p_peer p2p not viable peer_nat={} my_nat={}",
              flow_id_, static_cast<int>(peer_nat_type), static_cast<int>(my_nat_type_));
        if (on_p2p_upgrade_failed_) on_p2p_upgrade_failed_();
        return;
    }
    start_udp_punch();
}

// ---------------------------------------------------------------------------
// on_peer_p2p_connected()
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// switch_to_p2p()
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::switch_to_p2p() {
    relay_write_queue_.clear();

    // Release relay TCP connection now that P2P is active.
    // The server has p2p_signaled=true on this flow (set when flow_p2p_peer
    // is sent), so the relay disconnect will be handled gracefully:
    // clear the weak_ptr only, no flow_closed sent to the peer.
    if (relay_upstream_) {
        relay_upstream_->release_obj();
        relay_upstream_ = nullptr;
    }
    relay_transport_ = nullptr;

    std::error_code ep_ec;
    std::string local_str = p2p_socket_ ?
        Fundamental::StringFormat("{}:{}", p2p_socket_->local_endpoint(ep_ec).address().to_string(),
                                   p2p_socket_->local_endpoint(ep_ec).port()) : "?";
    FINFO("frp_proxy_data_channel flow_id={} switched to p2p local={} peer={}:{}",
          flow_id_, local_str,
          p2p_peer_endpoint_.address().to_string(), p2p_peer_endpoint_.port());

    schedule_p2p_keepalive();
    start_p2p_read_loop();  // ensure UDP read loop is active after switch
    if (on_p2p_upgraded_) on_p2p_upgraded_();
}

void frp_proxy_data_channel::schedule_p2p_keepalive() {
    if (!reference_.is_valid() || !p2p_success_ || !p2p_socket_) return;
    p2p_timer_.expires_after(std::chrono::seconds(10));
    p2p_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {
        if (ec || !reference_.is_valid() || !p2p_success_ || !p2p_socket_) return;
        std::uint8_t keepalive_byte = 0;
        p2p_socket_->async_send_to(asio::buffer(&keepalive_byte, 1), p2p_peer_endpoint_,
                                    [](const std::error_code&, std::size_t) {});
        schedule_p2p_keepalive();
    });
}

// ---------------------------------------------------------------------------
// start_udp_punch()
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::start_udp_punch() {
    if (!reference_.is_valid() || p2p_success_ || !p2p_socket_) return;
    if (my_nat_type_ == frp_runtime_nat_type_disabled || peer_nat_type_ == frp_runtime_nat_type_disabled) return;

    const bool i_am_symmetric = (my_nat_type_ == frp_runtime_nat_type_symmetric);
    std::mt19937 rng(std::random_device{}());

    if (i_am_symmetric) {
        std::error_code ec;
        std::uint16_t xxx = p2p_socket_->local_endpoint(ec).port();
        if (ec || xxx == 0) xxx = 40000;
        punch_sockets_.clear();
        std::set<std::uint16_t> used;
        {
            auto sock = std::make_unique<asio::ip::udp::socket>(executor_);
            if (!protocal_helper::udp_bind_endpoint(*sock, xxx)) {
                punch_sockets_.push_back(std::move(sock));
                used.insert(xxx);
            }
        }
        std::uniform_int_distribution<int> dist(
            std::max(1024, static_cast<int>(xxx) - 128),
            std::min(65535, static_cast<int>(xxx) + 128));
        int attempts = 0;
        while (static_cast<int>(punch_sockets_.size()) < kPunchSocketCount && attempts < 2000) {
            attempts++;
            auto port = static_cast<std::uint16_t>(dist(rng));
            if (used.count(port)) continue;
            auto sock = std::make_unique<asio::ip::udp::socket>(executor_);
            if (protocal_helper::udp_bind_endpoint(*sock, port)) continue;
            used.insert(port);
            punch_sockets_.push_back(std::move(sock));
        }
        FINFO("frp_proxy_data_channel flow_id={} udp_punch symmetric sockets={} target={}:{}",
              flow_id_, punch_sockets_.size(),
              p2p_peer_endpoint_.address().to_string(), p2p_peer_endpoint_.port());
    } else {
        punch_sockets_.clear();
        FINFO("frp_proxy_data_channel flow_id={} udp_punch full target={}:{}",
              flow_id_, p2p_peer_endpoint_.address().to_string(), p2p_peer_endpoint_.port());
    }

    punch_active_ = true;
    punch_round_  = 0;

    // For symmetric side, start read loops on punch_sockets
    if (i_am_symmetric) {
        for (auto& sock : punch_sockets_) {
            if (!sock) continue;
            auto punch_sock_ptr = sock.get();
            auto recv_buf = std::make_shared<std::array<char, 64>>();
            auto recv_ep  = std::make_shared<asio::ip::udp::endpoint>();
            auto do_recv = std::make_shared<std::function<void()>>();
            *do_recv = [this, self = shared_from_this(), punch_sock_ptr, recv_buf, recv_ep,
                        do_recv]() mutable {
                if (!reference_.is_valid() || !punch_active_ || p2p_success_ || punch_done_) return;
                punch_sock_ptr->async_receive_from(
                    asio::buffer(recv_buf->data(), recv_buf->size()), *recv_ep,
                    [this, self, punch_sock_ptr, recv_buf, recv_ep, do_recv]
                    (const std::error_code& ec, std::size_t bytes) mutable {
                        if (!reference_.is_valid()) return;
                        if (!ec && bytes == 6 && punch_active_ && !p2p_success_ && !punch_done_) {
                            std::uint16_t pkt_fid = 0;
                            std::memcpy(&pkt_fid, recv_buf->data(), 2);
                            std::uint16_t src_port = 0;
                            std::memcpy(&src_port, recv_buf->data() + 2, 2);
                            std::uint16_t reflected = 0;
                            std::memcpy(&reflected, recv_buf->data() + 4, 2);
                            if (pkt_fid == (flow_id_ & 0xFFFF)) {
                                // Immediately reply with correct peer_port
                                std::error_code lec;
                                std::uint16_t my_port = punch_sock_ptr->local_endpoint(lec).port();
                                if (!lec) {
                                    auto reply = std::make_shared<std::array<std::uint8_t, 6>>();
                                    std::memcpy(reply->data(), &pkt_fid, 2);
                                    std::memcpy(reply->data() + 2, &my_port, 2);
                                    std::memcpy(reply->data() + 4, &src_port, 2);
                                    punch_sock_ptr->async_send_to(asio::buffer(*reply), *recv_ep,
                                        [reply](const std::error_code&, std::size_t) {});
                                    confirmation_sent_ = true;
                                }
                                if (reflected == my_port) {
                                    punch_match_count_++;
                                    FINFO("frp_proxy_data_channel flow_id={} punch match {}/2 src={} reflected={}",
                                          flow_id_, punch_match_count_, src_port, reflected);
                                    if (punch_match_count_ >= 2) {
                                        for (auto& s : punch_sockets_) {
                                            if (s.get() == punch_sock_ptr) {
                                                p2p_socket_ = std::move(s);
                                                break;
                                            }
                                        }
                                        for (auto& s : punch_sockets_) {
                                            if (s) { std::error_code ce; s->close(ce); }
                                        }
                                        punch_sockets_.clear();
                                        punch_active_ = false;
                                        punch_done_ = true;
                                        p2p_success_ = true;
                                        p2p_peer_endpoint_ = *recv_ep;
                                        punch_timer_.cancel();
                                        FINFO("frp_proxy_data_channel flow_id={} udp_punch succeeded (symmetric) matches={}",
                                              flow_id_, punch_match_count_);
                                        switch_to_p2p();
                                        return;
                                    }
                                }
                            }
                        }
                        if (punch_active_ && !p2p_success_ && !punch_done_) (*do_recv)();
                    });
            };
            (*do_recv)();
        }
    }

    do_punch_round();
}

void frp_proxy_data_channel::do_punch_round() {
    if (!reference_.is_valid() || !punch_active_ || p2p_success_ || punch_done_) return;

    const bool i_am_symmetric = (my_nat_type_ == frp_runtime_nat_type_symmetric);
    const int max_rounds = kPunchMaxRounds + (i_am_symmetric ? 0 : 1);

    if (punch_round_ >= max_rounds) {
        FINFO("frp_proxy_data_channel flow_id={} udp_punch all rounds exhausted", flow_id_);
        punch_active_ = false;
        if (on_p2p_upgrade_failed_) on_p2p_upgrade_failed_();
        return;
    }

    std::mt19937 rng(std::random_device{}());
    std::vector<asio::ip::udp::endpoint> targets;

    if (i_am_symmetric) {
        targets.push_back(p2p_peer_endpoint_);
    } else {
        const std::uint16_t peer_base = p2p_peer_endpoint_.port();
        const auto peer_addr = p2p_peer_endpoint_.address();
        if (punch_round_ == 0) {
            std::vector<std::uint16_t> ports;
            for (int i = 0; i < kPunchScanRound0; ++i) {
                auto p = static_cast<std::uint16_t>(peer_base + i);
                if (p < 23) continue;
                ports.push_back(p);
                punch_scanned_ports_.insert(p);
            }
            std::shuffle(ports.begin(), ports.end(), rng);
            for (auto p : ports) targets.emplace_back(peer_addr, p);
        } else {
            std::uniform_int_distribution<int> dist2(23, 65535);
            int picked = 0, tries = 0;
            while (picked < kPunchScanPerRound && tries < 100000) {
                tries++;
                auto p = static_cast<std::uint16_t>(dist2(rng));
                if (punch_scanned_ports_.count(p)) continue;
                punch_scanned_ports_.insert(p);
                targets.emplace_back(peer_addr, p);
                picked++;
            }
            std::shuffle(targets.begin(), targets.end(), rng);
        }
    }

    auto retransmit_index = std::make_shared<int>(0);
    auto do_retransmit = std::make_shared<std::function<void()>>();
    *do_retransmit = [this, self = shared_from_this(), do_retransmit, retransmit_index,
                      targets = std::move(targets), i_am_symmetric]() mutable {
        if (!reference_.is_valid() || !punch_active_ || p2p_success_ || punch_done_) return;
        if (*retransmit_index >= kPunchRetransmitCount) {
            punch_round_++;
            punch_timer_.expires_after(std::chrono::milliseconds(kPunchRoundMs));
            punch_timer_.async_wait([this, self](const std::error_code& ec) mutable {
                if (ec) return;
                do_punch_round();
            });
            return;
        }
        (*retransmit_index)++;

        std::uint16_t fid = static_cast<std::uint16_t>(flow_id_ & 0xFFFF);
        std::uint16_t zero_port = 0;
        if (i_am_symmetric) {
            std::vector<asio::ip::udp::socket*> socks;
            for (auto& sock : punch_sockets_) {
                if (sock) socks.push_back(sock.get());
            }
            std::shuffle(socks.begin(), socks.end(), std::mt19937(std::random_device{}()));
            for (auto* sock : socks) {
                std::error_code ec;
                std::uint16_t lp = sock->local_endpoint(ec).port();
                if (ec) continue;
                auto pkt = std::make_shared<std::array<std::uint8_t, 6>>();
                std::memcpy(pkt->data(), &fid, 2);
                std::memcpy(pkt->data() + 2, &lp, 2);
                std::memcpy(pkt->data() + 4, &zero_port, 2);
                sock->async_send_to(asio::buffer(*pkt), targets[0],
                                    [pkt](const std::error_code&, std::size_t) {});
            }
        } else {
            std::uint16_t lp = 0;
            { std::error_code ec; lp = p2p_socket_->local_endpoint(ec).port(); }
            for (const auto& tgt : targets) {
                auto pkt = std::make_shared<std::array<std::uint8_t, 6>>();
                std::memcpy(pkt->data(), &fid, 2);
                std::memcpy(pkt->data() + 2, &lp, 2);
                std::memcpy(pkt->data() + 4, &zero_port, 2);
                p2p_socket_->async_send_to(asio::buffer(*pkt), tgt,
                                           [pkt](const std::error_code&, std::size_t) {});
            }
        }

        punch_timer_.expires_after(std::chrono::milliseconds(kPunchRetransmitMs));
        punch_timer_.async_wait([do_retransmit](const std::error_code& ec) mutable {
            if (ec) return;
            (*do_retransmit)();
        });
    };
    (*do_retransmit)();
}

// ---------------------------------------------------------------------------
// start_p2p_read_loop() -- handles UDP packets on p2p_socket_
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::start_p2p_read_loop() {
    if (!reference_.is_valid() || !p2p_socket_) return;
    p2p_socket_->async_receive_from(
        asio::buffer(p2p_read_buf_.data(), p2p_read_buf_.size()),
        p2p_recv_endpoint_,
        [this, self = shared_from_this()](const std::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (ec) {
                start_p2p_read_loop();
                return;
            }

            // 1-byte keepalive
            if (bytes_read == 1) {
                start_p2p_read_loop();
                return;
            }

            // 6-byte punch probe: [uint16 flow_id][uint16 src_port][uint16 reflected_port]
            if (bytes_read == 6) {
                std::uint16_t pkt_fid = 0;
                std::memcpy(&pkt_fid, p2p_read_buf_.data(), 2);
                std::uint16_t src_port = 0;
                std::memcpy(&src_port, p2p_read_buf_.data() + 2, 2);
                std::uint16_t reflected = 0;
                std::memcpy(&reflected, p2p_read_buf_.data() + 4, 2);
                if (pkt_fid == (flow_id_ & 0xFFFF) && punch_active_ && !p2p_success_ && !punch_done_) {
                    // Immediately reply with correct peer_port
                    std::error_code lec;
                    std::uint16_t my_port = p2p_socket_->local_endpoint(lec).port();
                    if (!lec) {
                        auto reply = std::make_shared<std::array<std::uint8_t, 6>>();
                        std::memcpy(reply->data(), &pkt_fid, 2);
                        std::memcpy(reply->data() + 2, &my_port, 2);
                        std::memcpy(reply->data() + 4, &src_port, 2);
                        p2p_socket_->async_send_to(asio::buffer(*reply), p2p_recv_endpoint_,
                            [reply](const std::error_code&, std::size_t) {});
                        confirmation_sent_ = true;
                    }
                    if (reflected == my_port) {
                        punch_match_count_++;
                        FINFO("frp_proxy_data_channel flow_id={} punch match {}/2 src={} reflected={}",
                              flow_id_, punch_match_count_, src_port, reflected);
                        if (punch_match_count_ >= 2) {
                            for (auto& s : punch_sockets_) {
                                if (s) { std::error_code ce; s->close(ce); }
                            }
                            punch_sockets_.clear();
                            punch_active_ = false;
                            punch_done_ = true;
                            p2p_success_ = true;
                            p2p_peer_endpoint_ = p2p_recv_endpoint_;
                            punch_timer_.cancel();
                            FINFO("frp_proxy_data_channel flow_id={} udp_punch succeeded (full) matches={}",
                                  flow_id_, punch_match_count_);
                            switch_to_p2p();
                            return;
                        }
                    }
                }
                start_p2p_read_loop();  // continue reading for KCP data
                return;
            }

            // Too small for KCP
            if (bytes_read < 24) {
                start_p2p_read_loop();
                return;
            }

            // KCP input
            if (kcp_) {
                ikcp_input(kcp_.get(), p2p_read_buf_.data(), static_cast<long>(bytes_read));
                auto now = static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count() & 0xffffffffu);
                ikcp_update(kcp_.get(), now);
                kcp_recv_loop();
            }
            start_p2p_read_loop();
        });
}

} // namespace network::proxy
