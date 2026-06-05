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

namespace network::proxy
{

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
        self->p2p_socket_->async_send_to(
            asio::buffer(*payload), self->p2p_peer_endpoint_,
            [payload](const std::error_code&, std::size_t) {});
    } else if (self->relay_transport_) {
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
// Constructor
// ---------------------------------------------------------------------------

frp_proxy_data_channel::frp_proxy_data_channel(const asio::any_io_executor& executor,
                                               std::string relay_host,
                                               std::string relay_service,
                                               std::uint32_t flow_id,
                                               std::string uuid,
                                               std::string traffic_secret,
                                               std::uint32_t idle_timeout_seconds)
    : executor_(executor),
      relay_host_(std::move(relay_host)),
      relay_service_(std::move(relay_service)),
      flow_id_(flow_id),
      uuid_(std::move(uuid)),
      traffic_secret_(std::move(traffic_secret)),
      kcp_update_timer_(executor),
      p2p_timer_(executor),
      idle_timeout_seconds_(idle_timeout_seconds),
      last_activity_time_(std::chrono::steady_clock::now()),
      idle_check_timer_(executor) {
    kcp_send_ctx_.self = this;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::enable_ssl(network_client_ssl_config config) {
    ssl_config_ = std::move(config);
}

void frp_proxy_data_channel::set_on_connected(connected_callback_t cb)       { on_connected_ = std::move(cb); }
void frp_proxy_data_channel::set_on_disconnected(disconnected_callback_t cb) { on_disconnected_ = std::move(cb); }
void frp_proxy_data_channel::set_on_data(data_callback_t cb)                 { on_data_ = std::move(cb); }
void frp_proxy_data_channel::set_on_p2p_upgraded(p2p_upgraded_callback_t cb) { on_p2p_upgraded_ = std::move(cb); }

// ---------------------------------------------------------------------------
// accept_p2p() -- take ownership of the punch result, switch to P2P
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::accept_p2p(std::shared_ptr<asio::ip::udp::socket> socket,
                                         const asio::ip::udp::endpoint& peer_endpoint,
                                         std::uint16_t /*local_port*/,
                                         std::uint16_t /*peer_port*/) {
    if (p2p_success_) return;
    p2p_socket_ = std::move(socket);
    p2p_peer_endpoint_ = peer_endpoint;
    p2p_success_ = true;
    FINFO("frp_proxy_data_channel flow_id={} accept_p2p peer={}:{}",
          flow_id_, peer_endpoint.address().to_string(), peer_endpoint.port());
    switch_to_p2p();
}

// ---------------------------------------------------------------------------
// expect_p2p_disconnect() -- tolerate relay close during confirmation
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::expect_p2p_disconnect() {
    p2p_expected_ = true;
}

// ---------------------------------------------------------------------------
// release_obj()
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::release_obj() {
    if (!reference_.release()) return;
    FINFO("frp_proxy_data_channel flow_id={} release_obj transport={}",
          flow_id_, p2p_success_ ? "p2p" : "tcp_relay");
    kcp_update_timer_.cancel();
    p2p_timer_.cancel();
    kcp_.reset();
    if (p2p_socket_) { std::error_code ec; p2p_socket_->close(ec); p2p_socket_.reset(); }
    if (relay_upstream_) { relay_upstream_->release_obj(); relay_upstream_ = nullptr; }
    relay_transport_ = nullptr;
}

// ---------------------------------------------------------------------------
// Endpoint queries
// ---------------------------------------------------------------------------

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
    kcp_traffic_key_ = frp_derive_kcp_flow_key(traffic_secret_, flow_id_);

    kcp_ = std::unique_ptr<ikcpcb, kcp_releaser>(ikcp_create(flow_id_, &kcp_send_ctx_));
    ikcp_setoutput(kcp_.get(), kcp_output_callback);
    kcp_.get()->stream = 0;
    ikcp_wndsize(kcp_.get(), 256, 256);
    ikcp_nodelay(kcp_.get(), 1, 20, 2, 1);

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

            frp_runtime_data_open_data open_req;
            open_req.command = frp_runtime_data_open_command;
            open_req.flow_id = flow_id_;
            open_req.uuid    = uuid_;
            auto pkt = packet_frp_runtime_command_data(open_req);
            if (!pkt) {
                notify_disconnect_once();
                return;
            }
            relay_write_queue_.push_back(std::move(pkt));
            relay_do_write();

            relay_connected_ = true;
            FINFO("frp_proxy_data_channel flow_id={} relay established local={} remote={}",
                  flow_id_, local_relay_endpoint(), remote_relay_endpoint());
            if (on_connected_) on_connected_();
            start_relay_read_loop();
            schedule_idle_check();
        });
    relay_upstream_->start_async_connect();
}

// ---------------------------------------------------------------------------
// send_bytes -- application data -> KCP
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::send_bytes(const char* data, std::size_t size) {
    if (!reference_.is_valid() || !kcp_) return;
    if (!kcp_active_) {
        kcp_active_ = true;
        schedule_kcp_update();
    }
    reset_activity_timestamp();
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
// Relay read loop
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::start_relay_read_loop() {
    if (!reference_.is_valid() || !relay_transport_) return;
    relay_transport_->async_buffers_read_some(
        { network_read_buffer_t { relay_read_buf_.data(), relay_read_buf_.size() } },
        [this, self = shared_from_this()](std::error_code ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (ec || bytes_read == 0) {
                if (p2p_success_) return;
                if (p2p_expected_) return;  // punch handshake in progress, relay close expected
                notify_disconnect_once();
                return;
            }
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
            if (ec || !relay_transport_) { if (p2p_success_) return; release_obj(); return; }
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
    std::array<char, 16 * 1024> buf{};
    std::vector<char> big_buf;
    while (true) {
        int peek_size = ikcp_peeksize(kcp_.get());
        if (peek_size < 0) break;
        char* recv_buf = buf.data();
        int recv_len = static_cast<int>(buf.size());
        if (peek_size > recv_len) {
            big_buf.resize(peek_size);
            recv_buf = big_buf.data();
            recv_len = peek_size;
        }
        auto n = ikcp_recv(kcp_.get(), recv_buf, recv_len);
        if (n < 0) break;
        std::vector<std::uint8_t> encrypted(recv_buf, recv_buf + n);
        auto plaintext = frp_kcp_decrypt(kcp_traffic_key_, encrypted);
        if (!plaintext || plaintext->empty()) continue;
        reset_activity_timestamp();
        if (on_data_) {
            on_data_(std::string(reinterpret_cast<const char*>(plaintext->data()), plaintext->size()));
        }
    }
}

// ---------------------------------------------------------------------------
// switch_to_p2p()
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::switch_to_p2p() {
    relay_write_queue_.clear();

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

    schedule_p2p_idle_timer();
    start_p2p_read_loop();
    if (on_p2p_upgraded_) on_p2p_upgraded_();
}

// ---------------------------------------------------------------------------
// keepalive
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::schedule_p2p_idle_timer() {
    if (!reference_.is_valid() || !p2p_success_ || !p2p_socket_) return;
    keepalive_probing_ = false;
    keepalive_probe_count_ = 0;
    p2p_timer_.cancel();
    p2p_timer_.expires_after(std::chrono::seconds(2));
    p2p_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {
        if (ec || !reference_.is_valid() || !p2p_success_ || !p2p_socket_) return;
        keepalive_probing_ = true;
        do_keepalive_probe();
    });
}

void frp_proxy_data_channel::do_keepalive_probe() {
    if (!reference_.is_valid() || !p2p_success_ || !p2p_socket_) return;
    if (!keepalive_probing_) return;
    if (keepalive_probe_count_ >= 10) {
        FWARN("frp_proxy_data_channel flow_id={} p2p keepalive exhausted, disconnecting", flow_id_);
        notify_disconnect_once();
        return;
    }
    keepalive_probe_count_++;
    std::uint8_t keepalive_byte = static_cast<std::uint8_t>(keepalive_probe_count_ & 0x7F);
    p2p_socket_->async_send_to(asio::buffer(&keepalive_byte, 1), p2p_peer_endpoint_,
                                [](const std::error_code&, std::size_t) {});
    p2p_timer_.expires_after(std::chrono::milliseconds(200));
    p2p_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {
        if (ec || !reference_.is_valid() || !p2p_success_ || !p2p_socket_) return;
        do_keepalive_probe();
    });
}

void frp_proxy_data_channel::reset_keepalive_timer() {
    if (!p2p_success_) return;
    p2p_timer_.cancel();
    schedule_p2p_idle_timer();
}

// ---------------------------------------------------------------------------
// start_p2p_read_loop() -- handles keepalive + KCP data on p2p_socket_
// Only active after accept_p2p() (p2p_success_ == true).
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

            // 1-byte keepalive: probes are 0..127, replies are 128..255.
            if (bytes_read == 1) {
                std::uint8_t val = static_cast<std::uint8_t>(p2p_read_buf_[0]);
                if (val < 128) {
                    std::uint8_t reply_byte = static_cast<std::uint8_t>(val + 128);
                    p2p_socket_->async_send_to(asio::buffer(&reply_byte, 1), p2p_recv_endpoint_,
                                                [](const std::error_code&, std::size_t) {});
                }
                reset_keepalive_timer();
                start_p2p_read_loop();
                return;
            }

            // Skip anything too small for KCP (punch probes are handled by frp_punch_engine)
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
            reset_keepalive_timer();
            start_p2p_read_loop();
        });
}

// ---------------------------------------------------------------------------
// idle timeout
// ---------------------------------------------------------------------------

void frp_proxy_data_channel::reset_activity_timestamp() {
    last_activity_time_ = std::chrono::steady_clock::now();
}

void frp_proxy_data_channel::schedule_idle_check() {
    if (!reference_.is_valid()) return;
    if (idle_timeout_seconds_ == 0) return;
    idle_check_timer_.expires_after(std::chrono::seconds(10));
    idle_check_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {
        if (ec || !reference_.is_valid()) return;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - last_activity_time_).count();
        if (elapsed >= static_cast<long long>(idle_timeout_seconds_)) {
            FWARN("frp_proxy_data_channel flow_id={} idle_timeout elapsed={}s limit={}s disconnecting",
                  flow_id_, elapsed, idle_timeout_seconds_);
            notify_disconnect_once();
            return;
        }
        schedule_idle_check();
    });
}

} // namespace network::proxy
