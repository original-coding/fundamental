#include "frp_punch_engine.hpp"

#include "frp_command.hpp"
#include "frp_common.hpp"
#include "frp_kcp_crypto.hpp"

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

static constexpr std::size_t kMaxEndpointProbeAttempts = 50;
static constexpr int kEndpointProbeIntervalMs       = 100;
static constexpr int kPunchMaxRounds          = 1;
static constexpr int kPunchSocketCount        = 65;
static constexpr int kPunchRetransmitCount     = 5;
static constexpr int kPunchPortMin             = 512;
static constexpr int kPunchPortMax             = 65535;
static constexpr int kConeProbePortCount      = 3000;

std::vector<std::uint16_t> build_cone_port_pool() {
    constexpr auto count = kPunchPortMax - kPunchPortMin + 1;
    std::vector<std::uint16_t> ports;
    ports.reserve(static_cast<std::size_t>(count));
    for (std::uint32_t p = kPunchPortMin; p <= static_cast<std::uint32_t>(kPunchPortMax); ++p) {
        ports.push_back(static_cast<std::uint16_t>(p));
    }
    return ports;
}

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
// Constructor / destructor
// ---------------------------------------------------------------------------

frp_punch_engine::frp_punch_engine(config cfg, signal_sender sender)
    : executor_(std::move(cfg.executor)),
      connection_uuid_(std::move(cfg.connection_uuid)),
      peer_uuid_(std::move(cfg.peer_uuid)),
      traffic_secret_(std::move(cfg.traffic_secret)),
      public_server_host_(std::move(cfg.public_server_host)),
      public_server_udp_port_(cfg.public_server_udp_port),
      my_nat_type_(cfg.my_nat_type),
      my_rtt_ms_(cfg.my_rtt_ms),
      punch_seq_(cfg.punch_seq),
      signal_sender_(std::move(sender)),
      endpoint_probe_timer_(executor_),
      deadline_timer_(executor_),
      punch_timer_(executor_)
{
    std::hash<std::string> hasher;
    punch_tag_ = static_cast<std::uint32_t>(hasher(connection_uuid_) & 0xFFFFFFFF);
    if (punch_tag_ == 0) punch_tag_ = 1;
    io_context_pool::Instance().reg_timer(endpoint_probe_timer_);
    io_context_pool::Instance().reg_timer(deadline_timer_);
    io_context_pool::Instance().reg_timer(punch_timer_);
}

frp_punch_engine::~frp_punch_engine()
{
    release();
    io_context_pool::Instance().unreg_timer(endpoint_probe_timer_);
    io_context_pool::Instance().unreg_timer(deadline_timer_);
    io_context_pool::Instance().unreg_timer(punch_timer_);
}

std::shared_ptr<frp_punch_engine> frp_punch_engine::create(config cfg, signal_sender sender)
{
    return std::make_shared<frp_punch_engine>(std::move(cfg), std::move(sender));
}

void frp_punch_engine::set_on_success(success_callback cb) { on_success_ = std::move(cb); }
void frp_punch_engine::set_on_failed(failed_callback cb)   { on_failed_  = std::move(cb); }

bool frp_punch_engine::is_valid_probe_pair(std::uint16_t local_port, std::uint16_t tgt_port) const {
    std::error_code ec;
    bool port_ok = false;
    if (p2p_socket_ && p2p_socket_->local_endpoint(ec).port() == local_port) port_ok = true;
    if (!port_ok) {
        for (auto& s : punch_sockets_) {
            if (s && s->local_endpoint(ec).port() == local_port) { port_ok = true; break; }
        }
    }
    bool tgt_ok = current_cone_targets_.count(tgt_port) > 0;
    return port_ok && tgt_ok;
}

void frp_punch_engine::release()
{
    if (!reference_.release()) return;
    FINFO("frp_punch_engine conn={} release", connection_uuid_);
    endpoint_probe_timer_.cancel();
    deadline_timer_.cancel();
    punch_timer_.cancel();
    for (auto& s : punch_sockets_) { if (s) { std::error_code ec; s->close(ec); } }
    punch_sockets_.clear();
    if (p2p_socket_) { std::error_code ec; p2p_socket_->close(ec); p2p_socket_.reset(); }
}

void frp_punch_engine::start()
{
    if (!reference_.is_valid() || public_server_udp_port_ == 0) {
        if (on_failed_) on_failed_();
        return;
    }

    {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(25000, 65535);
        int attempts = 0;
        while (attempts < 100) {
            auto probe_port = static_cast<std::uint16_t>(dist(rng));
            auto sock = std::make_shared<asio::ip::udp::socket>(executor_);
            std::error_code ec;
            sock->open(asio::ip::udp::v4(), ec);
            if (ec) continue;
            sock->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), probe_port), ec);
            if (!ec) {
                p2p_socket_ = std::move(sock);
                break;
            }
            attempts++;
        }
    }
    if (!p2p_socket_) {
        if (on_failed_) on_failed_();
        return;
    }
    probing_ = true;
    start_endpoint_echo_loop();

    std::error_code ec;
    auto local_port = p2p_socket_->local_endpoint(ec).port();
    FINFO("frp_punch_engine conn={} start local_port={}", connection_uuid_, local_port);
    do_endpoint_probe();
}

void frp_punch_engine::on_peer_info(peer_info peer)
{
    if (!reference_.is_valid()) return;

    std::error_code ec;
    auto addr = asio::ip::make_address(peer.host, ec);
    if (ec) {
        FERR("frp_punch_engine conn={} on_peer_info bad address {}", connection_uuid_, peer.host);
        if (on_failed_) on_failed_();
        return;
    }
    p2p_peer_endpoint_ = asio::ip::udp::endpoint(addr, peer.port);
    peer_nat_type_      = peer.nat_type;
    peer_rtt_ms_        = peer.rtt_ms;

    FINFO("frp_punch_engine conn={} on_peer_info peer={}:{} peer_nat={} peer_rtt={}ms my_rtt={}ms",
          connection_uuid_, peer.host, peer.port, static_cast<int>(peer.nat_type), peer_rtt_ms_, my_rtt_ms_);

    if (peer.nat_type == frp_nat_type_disabled || my_nat_type_ == frp_nat_type_disabled) {
        FINFO("frp_punch_engine conn={} p2p not viable", connection_uuid_);
        if (on_failed_) on_failed_();
        return;
    }
}

// ---------------------------------------------------------------------------
// Signal-send helpers (using new command names)
// ---------------------------------------------------------------------------

void frp_punch_engine::send_provider_probe_match(std::uint16_t local_port, std::uint16_t peer_port,
                                                  std::uint16_t target_port, std::uint16_t peer_external_port)
{
    if (!signal_sender_) return;
    frp_client_punch_confirm_data pm;
    pm.command             = frp_client_provider_probe_match;
    pm.connection_uuid     = connection_uuid_;
    pm.local_port          = local_port;
    pm.peer_port           = peer_port;
    pm.external_local_port = target_port;
    pm.external_peer_port  = peer_external_port;
    FINFO("frp_punch_engine conn={} send provider_probe_match local={} peer={} ext_local={} ext_peer={}",
          connection_uuid_, local_port, peer_port, target_port, peer_external_port);
    signal_sender_(Fundamental::io::to_json(pm));
}

// ---------------------------------------------------------------------------
// Signal handshake -- incoming from peer via signal channel (new protocol)
// ---------------------------------------------------------------------------

void frp_punch_engine::on_provider_probe_match(std::uint16_t local_port, std::uint16_t peer_port,
                                                std::uint16_t external_local_port, std::uint16_t external_peer_port)
{
    // Accessor: received probe_match hint, send formal punch_confirm
    if (!is_valid_probe_pair(local_port, external_peer_port)) return;
    if (result_delivered_) return;
    confirm_started_ = true;

    if (!punch_sockets_.empty()) {
        for (auto& s : punch_sockets_) {
            if (!s) continue;
            std::error_code ec;
            if (s->local_endpoint(ec).port() == local_port) {
                p2p_socket_ = std::move(s);
                break;
            }
        }
        for (auto& s : punch_sockets_) {
            if (s) { std::error_code ec; s->close(ec); }
        }
        punch_sockets_.clear();
    }

    punch_active_ = false;
    punch_done_ = true;
    punch_timer_.cancel();
    FINFO("frp_punch_engine conn={} probe_match received, sending punch_confirm", connection_uuid_);
    if (on_p2p_imminent_) on_p2p_imminent_();

    frp_client_punch_confirm_data confirm;
    confirm.command             = frp_client_accessor_punch_confirm;
    confirm.connection_uuid     = connection_uuid_;
    confirm.local_port          = local_port;
    confirm.peer_port           = peer_port;
    confirm.external_local_port = external_local_port;
    confirm.external_peer_port  = external_peer_port;
    signal_sender_(Fundamental::io::to_json(confirm));
}

void frp_punch_engine::on_accessor_punch_confirm(std::uint16_t local_port, std::uint16_t peer_port,
                                                  std::uint16_t external_local_port, std::uint16_t external_peer_port)
{
    // Provider: received formal punch_confirm, send confirm_ack
    if (!is_valid_probe_pair(local_port, external_peer_port)) return;
    if (result_delivered_) return;
    confirm_started_ = true;

    if (!punch_sockets_.empty()) {
        for (auto& s : punch_sockets_) {
            if (!s) continue;
            std::error_code ec;
            if (s->local_endpoint(ec).port() == local_port) {
                p2p_socket_ = std::move(s);
                break;
            }
        }
        for (auto& s : punch_sockets_) {
            if (s) { std::error_code ec; s->close(ec); }
        }
        punch_sockets_.clear();
    }

    punch_active_ = false;
    punch_done_ = true;
    punch_timer_.cancel();
    FINFO("frp_punch_engine conn={} punch_confirm received, sending confirm_ack", connection_uuid_);
    if (on_p2p_imminent_) on_p2p_imminent_();

    frp_client_punch_confirm_data ack;
    ack.command             = frp_client_provider_confirm_ack;
    ack.connection_uuid     = connection_uuid_;
    ack.local_port          = local_port;
    ack.peer_port           = peer_port;
    ack.external_local_port = external_local_port;
    ack.external_peer_port  = external_peer_port;
    signal_sender_(Fundamental::io::to_json(ack));
}

void frp_punch_engine::on_provider_confirm_ack(std::uint16_t local_port, std::uint16_t peer_port,
                                                std::uint16_t external_local_port, std::uint16_t external_peer_port)
{
    // Accessor: received confirm_ack → p2p ready, send confirm_ok
    if (!is_valid_probe_pair(local_port, external_peer_port)) return;
    if (result_delivered_) return;

    punch_active_ = false;
    punch_done_ = true;
    punch_timer_.cancel();
    if (!punch_sockets_.empty()) {
        for (auto& s : punch_sockets_) {
            if (!s) continue;
            std::error_code ec;
            if (s->local_endpoint(ec).port() == local_port) {
                p2p_socket_ = std::move(s);
                break;
            }
        }
        for (auto& s : punch_sockets_) {
            if (s) { std::error_code ec; s->close(ec); }
        }
        punch_sockets_.clear();
    }
    FINFO("frp_punch_engine conn={} confirm_ack received, sending confirm_ok", connection_uuid_);
    if (on_p2p_imminent_) on_p2p_imminent_();

    // Send confirm_ok
    frp_client_punch_confirm_data ok;
    ok.command             = frp_client_accessor_confirm_ok;
    ok.connection_uuid     = connection_uuid_;
    ok.local_port          = local_port;
    ok.peer_port           = peer_port;
    ok.external_local_port = external_local_port;
    ok.external_peer_port  = external_peer_port;
    signal_sender_(Fundamental::io::to_json(ok));

    // Accessor: p2p ready after sending confirm_ok
    on_punch_success(local_port, peer_port, external_local_port, external_peer_port);
}

void frp_punch_engine::on_accessor_confirm_ok(std::uint16_t local_port, std::uint16_t peer_port,
                                               std::uint16_t external_local_port, std::uint16_t external_peer_port)
{
    // Provider: received confirm_ok → p2p ready
    if (!is_valid_probe_pair(local_port, external_peer_port)) return;
    if (result_delivered_) return;

    punch_active_ = false;
    punch_done_ = true;
    punch_timer_.cancel();
    for (auto& s : punch_sockets_) {
        if (s) { std::error_code ec; s->close(ec); }
    }
    punch_sockets_.clear();
    FINFO("frp_punch_engine conn={} confirm_ok received, p2p ready", connection_uuid_);
    on_punch_success(local_port, peer_port, external_local_port, external_peer_port);
}

void frp_punch_engine::on_punch_success(std::uint16_t local_port, std::uint16_t peer_port,
                                         std::uint16_t external_local_port, std::uint16_t external_peer_port)
{
    if (result_delivered_) return;
    result_delivered_ = true;

    if (p2p_socket_) {
        std::error_code ignore;
        p2p_socket_->cancel(ignore);
    }

    punch_result result;
    result.socket        = std::move(p2p_socket_);
    result.peer_endpoint = asio::ip::udp::endpoint(p2p_peer_endpoint_.address(), external_peer_port);
    result.local_port    = local_port;
    result.peer_port     = peer_port;
    FINFO("frp_punch_engine conn={} success local_port={} peer_port={} peer_endpoint={}:{}",
          connection_uuid_, local_port, peer_port,
          result.peer_endpoint.address().to_string(), result.peer_endpoint.port());

    if (on_success_) on_success_(std::move(result));
}

// ---------------------------------------------------------------------------
// Endpoint echo loop
// ---------------------------------------------------------------------------

void frp_punch_engine::start_endpoint_echo_loop()
{
    if (!reference_.is_valid() || !probing_ || !p2p_socket_) return;
    p2p_socket_->async_receive_from(
        asio::buffer(p2p_read_buf_.data(), p2p_read_buf_.size()), p2p_recv_endpoint_,
        [this, self = shared_from_this()](const std::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid() || !probing_) return;
            if (ec || bytes_read == 0) { start_endpoint_echo_loop(); return; }

            std::vector<std::uint8_t> encrypted(p2p_read_buf_.data(), p2p_read_buf_.data() + bytes_read);
            auto key = frp_derive_kcp_key(traffic_secret_);
            auto plaintext = frp_kcp_decrypt(key, encrypted);
            if (!plaintext) { start_endpoint_echo_loop(); return; }
            std::string payload(plaintext->begin(), plaintext->end());
            frp_udp_echo_data echo;
            if (!Fundamental::io::from_json(payload, echo) || echo.command != frp_udp_echo_command) {
                start_endpoint_echo_loop();
                return;
            }

            probing_ = false;
            endpoint_probe_timer_.cancel();
            my_external_ip_ = echo.external_ip;
            my_external_port_ = echo.external_port;
            FINFO("frp_punch_engine conn={} echo external={}:{}",
                  connection_uuid_, my_external_ip_, my_external_port_);
            if (on_endpoint_ready_) on_endpoint_ready_(my_external_ip_, my_external_port_);
        });
}

void frp_punch_engine::do_endpoint_probe()
{
    if (!reference_.is_valid() || !probing_ || !p2p_socket_) return;
    if (endpoint_probe_attempts_ >= kMaxEndpointProbeAttempts) {
        FINFO("frp_punch_engine conn={} endpoint_probe timeout", connection_uuid_);
        probing_ = false;
        if (on_failed_) on_failed_();
        return;
    }

    std::error_code ec;
    auto local_port = p2p_socket_->local_endpoint(ec).port();

    frp_p2p_probe_data probe;
    probe.command    = frp_p2p_probe_command;
    probe.local_port = local_port;
    auto payload   = Fundamental::io::to_json(probe);
    auto encrypted = frp_kcp_encrypt_string(frp_derive_kcp_key(traffic_secret_), payload);
    if (encrypted.empty()) {
        if (on_failed_) on_failed_();
        return;
    }
    auto server_ep = resolve_udp_ep(executor_, public_server_host_, public_server_udp_port_);
    if (!server_ep) {
        if (on_failed_) on_failed_();
        return;
    }

    endpoint_probe_attempts_++;
    FINFO("frp_punch_engine conn={} endpoint_probe attempt={} local_port={} server={}:{}",
          connection_uuid_, endpoint_probe_attempts_, local_port,
          public_server_host_, public_server_udp_port_);

    auto enc_ptr = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted));
    p2p_socket_->async_send_to(asio::buffer(*enc_ptr), *server_ep,
                               [enc_ptr](const std::error_code&, std::size_t) {});

    endpoint_probe_timer_.expires_after(std::chrono::milliseconds(kEndpointProbeIntervalMs));
    endpoint_probe_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec2) {
        if (ec2 || !reference_.is_valid()) return;
        do_endpoint_probe();
    });
}

void frp_punch_engine::start_punch_at(std::int64_t local_deadline_us)
{
    if (!reference_.is_valid() || !p2p_socket_) return;
    probing_ = false;
    // 取消 endpoint echo 循环的 pending receive，避免同 socket 双接收者（规范 §3.5）
    std::error_code cancel_ec;
    p2p_socket_->cancel(cancel_ec);
    start_punch_read_loop();
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::int64_t delay_us = local_deadline_us - now_us;
    if (delay_us < 0) delay_us = 0;
    FINFO("frp_punch_engine conn={} start_punch_at deadline={}us delay={}us",
          connection_uuid_, local_deadline_us, delay_us);
    deadline_timer_.expires_after(std::chrono::microseconds(delay_us));
    deadline_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {
        if (ec || !reference_.is_valid()) return;
        start_udp_punch();
    });
}

void frp_punch_engine::start_udp_punch()
{
    if (!reference_.is_valid() || !p2p_socket_) return;
    if (my_nat_type_ == frp_nat_type_disabled || peer_nat_type_ == frp_nat_type_disabled) return;

    const bool i_am_symmetric = (my_nat_type_ == frp_nat_type_symmetric);

    if (!i_am_symmetric) {
        punch_sockets_.clear();
        FINFO("frp_punch_engine conn={} udp_punch cone target={}:{}",
              connection_uuid_, p2p_peer_endpoint_.address().to_string(), p2p_peer_endpoint_.port());
    }

    punch_active_ = true;
    punch_round_  = 0;

    if (i_am_symmetric) {
        rebuild_symmetric_sockets();
    }

    do_punch_round();
}

void frp_punch_engine::rebuild_symmetric_sockets()
{
    punch_socket_gen_++;
    for (auto& s : punch_sockets_) {
        if (s) { std::error_code ec; s->close(ec); }
    }
    punch_sockets_.clear();

    std::mt19937 rng(std::random_device{}());
    std::set<std::uint16_t> scanned_ports;

    std::error_code ec;
    std::uint16_t xxx = p2p_socket_->local_endpoint(ec).port();
    if (ec || xxx == 0) xxx = 40000;
    FINFO("frp_punch_engine conn={} udp_punch sym base_port={}", connection_uuid_, xxx);

    // 1) Base socket
    {
        auto sock = std::make_shared<asio::ip::udp::socket>(executor_);
        sock->open(asio::ip::udp::v4(), ec);
        if (!ec) sock->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), xxx), ec);
        if (!ec) {
            punch_sockets_.push_back(std::move(sock));
            scanned_ports.insert(xxx);
        }
    }

    // 2) 64 spread-random ports
    {
        constexpr int kSpreadPorts = kPunchSocketCount - 1;
        const std::uint32_t total_range =
            static_cast<std::uint32_t>(kPunchPortMax - kPunchPortMin + 1);
        const int seg_size = static_cast<int>(total_range / static_cast<std::uint32_t>(kSpreadPorts));

        for (int i = 0; i < kSpreadPorts; i++) {
            int seg_start = static_cast<int>(kPunchPortMin + i * seg_size);
            int seg_end = (i == kSpreadPorts - 1) ? kPunchPortMax : seg_start + seg_size - 1;
            if (seg_start < static_cast<int>(xxx) && static_cast<int>(xxx) <= seg_end) {
                continue;
            }
            std::uniform_int_distribution<int> port_dist(seg_start, seg_end);
            for (int attempt = 0; attempt < 30; attempt++) {
                auto port = static_cast<std::uint16_t>(port_dist(rng));
                if (scanned_ports.count(port)) continue;
                auto sock = std::make_shared<asio::ip::udp::socket>(executor_);
                sock->open(asio::ip::udp::v4(), ec);
                if (ec) continue;
                sock->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), port), ec);
                if (!ec) {
                    punch_sockets_.push_back(std::move(sock));
                    scanned_ports.insert(port);
                    break;
                }
            }
        }
    }

    std::shuffle(punch_sockets_.begin(), punch_sockets_.end(), rng);

    int my_gen = punch_socket_gen_;
    for (auto& sock : punch_sockets_) {
        if (!sock) continue;
        auto recv_buf = std::make_shared<std::array<char, 64>>();
        auto recv_ep  = std::make_shared<asio::ip::udp::endpoint>();
        auto do_recv = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> w_recv = do_recv;
        *do_recv = [this, self = shared_from_this(), sock, recv_buf, recv_ep,
                    w_recv, my_gen]() mutable {
            if (!reference_.is_valid() || !punch_active_ || punch_done_ || result_delivered_) return;
            if (punch_socket_gen_ != my_gen) return;
            auto inner_recv = w_recv.lock();
            if (!inner_recv) return;
            sock->async_receive_from(
                asio::buffer(recv_buf->data(), recv_buf->size()), *recv_ep,
                [this, self, sock, recv_buf, recv_ep, inner_recv]
                (const std::error_code& ec2, std::size_t bytes) mutable {
                    if (!reference_.is_valid()) return;
                    if (!ec2 && bytes == 8 && punch_active_ && !punch_done_ && !result_delivered_) {
                        std::uint32_t pkt_tag = 0;
                        std::memcpy(&pkt_tag, recv_buf->data(), 4);
                        std::uint16_t src_port = 0;
                        std::memcpy(&src_port, recv_buf->data() + 4, 2);
                        if (pkt_tag == punch_tag_) {
                            std::error_code lec;
                            std::uint16_t my_port = sock->local_endpoint(lec).port();
                            if (!lec) {
                                std::uint16_t tgt_port = 0;
                                std::memcpy(&tgt_port, recv_buf->data() + 6, 2);
                                FINFO("frp_punch_engine conn={} sym probe match local={} peer={} tgt={} ext_peer={}",
                                      connection_uuid_, my_port, src_port, tgt_port, recv_ep->port());
                                p2p_peer_endpoint_ = *recv_ep;
                                if (on_probe_match_) on_probe_match_(my_port, src_port, tgt_port, recv_ep->port());
                            }
                        }
                    }
                    if (punch_active_ && !punch_done_ && !result_delivered_) {
                        (*inner_recv)();
                    }
                });
        };
        (*do_recv)();
    }
    FINFO("frp_punch_engine conn={} udp_punch symmetric round={} sockets={}",
          connection_uuid_, punch_round_, punch_sockets_.size());
}

void frp_punch_engine::do_punch_round()
{
    if (!reference_.is_valid() || !punch_active_ || punch_done_ || result_delivered_) return;

    const bool i_am_symmetric = (my_nat_type_ == frp_nat_type_symmetric);
    const bool peer_is_symmetric = (peer_nat_type_ == frp_nat_type_symmetric);
    const bool both_cone = !i_am_symmetric && !peer_is_symmetric;
    const int max_rounds = kPunchMaxRounds;

    if (punch_round_ >= max_rounds) {
        FINFO("frp_punch_engine conn={} udp_punch all rounds exhausted", connection_uuid_);
        punch_timer_.cancel();
        punch_active_ = false;
        punch_done_ = true;
        for (auto& s : punch_sockets_) {
            if (s) { std::error_code ec; s->close(ec); }
        }
        punch_sockets_.clear();
        if (on_failed_) on_failed_();
        return;
    }

    std::mt19937 rng(std::random_device{}());
    std::vector<asio::ip::udp::endpoint> targets;
    current_cone_targets_.clear();
    current_cone_targets_.insert(p2p_peer_endpoint_.port());
    if (i_am_symmetric || both_cone) {
        if (punch_round_ == 0) {
            targets.push_back(p2p_peer_endpoint_);
        }
    } else {
        const auto peer_addr = p2p_peer_endpoint_.address();
        auto cone_ports = build_cone_port_pool();
        std::shuffle(cone_ports.begin(), cone_ports.end(), rng);
        if (cone_ports.size() > static_cast<size_t>(kConeProbePortCount)) {
            cone_ports.resize(kConeProbePortCount);
        }
        for (auto port : cone_ports) {
            targets.emplace_back(peer_addr, port);
            current_cone_targets_.insert(port);
        }
    }

    {
        std::error_code lec;
        std::uint16_t my_port = 0;
        if (!i_am_symmetric) my_port = p2p_socket_->local_endpoint(lec).port();
        if (i_am_symmetric) {
            FINFO("frp_punch_engine conn={} udp_punch round={} sym: {} sockets -> {}:{}",
                  connection_uuid_, punch_round_, punch_sockets_.size(),
                  targets.empty() ? std::string("(idle)") : targets[0].address().to_string(),
                  targets.empty() ? 0 : targets[0].port());
        } else {
            FINFO("frp_punch_engine conn={} udp_punch round={} cone: local={} -> {} targets",
                  connection_uuid_, punch_round_, my_port, targets.size());
        }
    }

    auto retransmit_index = std::make_shared<int>(0);
    auto do_retransmit = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> w_retransmit = do_retransmit;
    *do_retransmit = [this, self = shared_from_this(), w_retransmit, retransmit_index,
                      targets = std::move(targets), i_am_symmetric]() mutable {
        if (!reference_.is_valid() || !punch_active_ || punch_done_ || result_delivered_) return;
        if (*retransmit_index >= kPunchRetransmitCount) {
            punch_round_++;
            do_punch_round();
            return;
        }
        (*retransmit_index)++;

        auto tag = punch_tag_;
        if (!targets.empty() && i_am_symmetric) {
            std::vector<std::shared_ptr<asio::ip::udp::socket>> socks;
            for (auto& sock : punch_sockets_) {
                if (sock) socks.push_back(sock);
            }
            std::shuffle(socks.begin(), socks.end(), std::mt19937(std::random_device{}()));
            for (auto& sock : socks) {
                std::error_code ec;
                std::uint16_t lp = sock->local_endpoint(ec).port();
                if (ec) continue;
                std::uint16_t tgt_port = targets[0].port();
                auto pkt = std::make_shared<std::array<std::uint8_t, 8>>();
                std::memcpy(pkt->data(), &tag, 4);
                std::memcpy(pkt->data() + 4, &lp, 2);
                std::memcpy(pkt->data() + 6, &tgt_port, 2);
                sock->async_send_to(asio::buffer(*pkt), targets[0],
                                    [pkt](const std::error_code&, std::size_t) {});
            }
        } else if (!targets.empty()) {
            std::uint16_t lp = 0;
            { std::error_code ec; lp = p2p_socket_->local_endpoint(ec).port(); }
            for (const auto& tgt : targets) {
                std::uint16_t tgt_port = tgt.port();
                auto pkt = std::make_shared<std::array<std::uint8_t, 8>>();
                std::memcpy(pkt->data(), &tag, 4);
                std::memcpy(pkt->data() + 4, &lp, 2);
                std::memcpy(pkt->data() + 6, &tgt_port, 2);
                p2p_socket_->async_send_to(asio::buffer(*pkt), tgt,
                                           [pkt](const std::error_code&, std::size_t) {});
            }
        }

        auto interval_ms = static_cast<int>((my_rtt_ms_ + peer_rtt_ms_) * 3);
        if (interval_ms < 1000) interval_ms = 1000;
        if (interval_ms > 10000) interval_ms = 10000;
        punch_timer_.expires_after(std::chrono::milliseconds(interval_ms));
        auto retransmit_ref = w_retransmit.lock();
        if (!retransmit_ref) return;
        punch_timer_.async_wait([retransmit_ref](const std::error_code& ec) {
            if (ec) return;
            (*retransmit_ref)();
        });
    };
    (*do_retransmit)();
}

void frp_punch_engine::start_punch_read_loop()
{
    if (!reference_.is_valid() || !p2p_socket_) return;
    p2p_socket_->async_receive_from(
        asio::buffer(p2p_read_buf_.data(), p2p_read_buf_.size()),
        p2p_recv_endpoint_,
        [this, self = shared_from_this()](const std::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid() || result_delivered_) return;
            if (ec) {
                if (!result_delivered_) start_punch_read_loop();
                return;
            }

            // 8-byte punch probe: [uint32 punch_tag][uint16 src_port][uint16 target_port]
            if (bytes_read == 8) {
                std::uint32_t pkt_tag = 0;
                std::memcpy(&pkt_tag, p2p_read_buf_.data(), 4);
                std::uint16_t src_port = 0;
                std::memcpy(&src_port, p2p_read_buf_.data() + 4, 2);
                std::uint16_t tgt_port = 0;
                std::memcpy(&tgt_port, p2p_read_buf_.data() + 6, 2);
                if (pkt_tag == punch_tag_) {
                    if (punch_active_ && !punch_done_ && !result_delivered_) {
                        std::error_code lec;
                        std::uint16_t my_port = p2p_socket_->local_endpoint(lec).port();
                        if (!lec) {
                            FINFO("frp_punch_engine conn={} cone probe match local={} peer={} tgt={} ext_peer={}",
                                  connection_uuid_, my_port, src_port, tgt_port, p2p_recv_endpoint_.port());
                            p2p_peer_endpoint_ = p2p_recv_endpoint_;
                            if (on_probe_match_) on_probe_match_(my_port, src_port, tgt_port, p2p_recv_endpoint_.port());
                        }
                    }
                }
                if (!result_delivered_) start_punch_read_loop();
                return;
            }

            if (!result_delivered_) start_punch_read_loop();
        });
}

} // namespace network::proxy
