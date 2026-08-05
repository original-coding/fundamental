#include "frp_signal_client.hpp"
#include "frp_client.hpp"
#include "frp_common.hpp"
#include "frp_kcp_crypto.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/uuid_utils.hpp"
#include <openssl/sha.h>

namespace network::proxy {
namespace {
std::string sha256_hex(std::string_view input) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
  return Fundamental::Utils::BufferToHex(hash, SHA256_DIGEST_LENGTH);
}

std::optional<asio::ip::udp::endpoint> resolve_udp_endpoint(
    const asio::any_io_executor& ex, const std::string& host, std::uint16_t port) {
  std::error_code ec;
  auto addr = asio::ip::make_address(host, ec);
  if (!ec) {
    if (addr.is_v6()) return std::nullopt;
    return asio::ip::udp::endpoint(addr, port);
  }
  asio::ip::udp::resolver resolver(ex);
  auto eps = resolver.resolve(asio::ip::udp::v4(), host, std::to_string(port), ec);
  if (ec) return std::nullopt;
  auto it = eps.begin(); if (it == eps.end()) return std::nullopt;
  return it->endpoint();
}
} // namespace

frp_signal_client::frp_signal_client(frp_proxy_client_config config) :
config_(std::move(config)),
uuid_(frp_generate_uuid()),
executor_(io_context_pool::Instance().get_io_context().get_executor()),
reconnect_timer_(executor_),
poll_timer_(executor_),
probe_timer_(executor_) {
    io_context_pool::Instance().reg_timer(reconnect_timer_);
    io_context_pool::Instance().reg_timer(poll_timer_);
    io_context_pool::Instance().reg_timer(probe_timer_);
}

frp_signal_client::~frp_signal_client() {
    io_context_pool::Instance().unreg_timer(reconnect_timer_);
    io_context_pool::Instance().unreg_timer(poll_timer_);
    io_context_pool::Instance().unreg_timer(probe_timer_);
    io_context_pool::Instance().unreg_object(this);
}

void frp_signal_client::start() {
    if (!reference_.is_valid()) return;
    FINFO("frp_signal_client start uuid={}", uuid_);
    io_context_pool::Instance().reg_object(this,
        [self = shared_from_this()]() { self->release_obj(); });
    connect_signal_channel();
}

void frp_signal_client::release_obj() {
    if (!reference_.release()) return;
    FINFO("frp_signal_client release_obj uuid={}", uuid_);
    io_context_pool::Instance().unreg_object(this);
    asio::post(executor_, [this, self = shared_from_this()] {
        reconnect_timer_.cancel();
        poll_timer_.cancel();
        probe_timer_.cancel();
        if (probe_socket_) { std::error_code ec; probe_socket_->close(ec); probe_socket_.reset(); }
        if (signal_) signal_->release_obj();
    });
}

void frp_signal_client::connect_signal_channel() {
    signal_ = frp_signal_channel::make_shared(
        executor_, config_.public_server_host, std::to_string(config_.public_server_tcp_port));
    signal_->enable_ssl(to_network_config(config_.ssl));
    signal_->set_on_connected([this] {
        FINFO("signal connected uuid={}", uuid_);
        reconnect_delay_seconds_ = 2;
    });
    signal_->set_on_disconnected([this] {
        FWARN("signal disconnected uuid={}", uuid_);
        schedule_reconnect();
    });
    signal_->set_on_command(
        [this](const frp_command_base& cmd, std::string p) { process_command(cmd, std::move(p)); });
    signal_->start();
}

void frp_signal_client::schedule_reconnect() {
    if (!reference_.is_valid()) return;
    reconnect_timer_.expires_after(std::chrono::seconds(reconnect_delay_seconds_));
    reconnect_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {
        if (ec || !reference_.is_valid()) return;
        if (reconnect_delay_seconds_ < 60) reconnect_delay_seconds_ *= 2;
        connect_signal_channel();
    });
}

void frp_signal_client::process_command(const frp_command_base& cmd, std::string payload) {
    if (cmd.command >= 100) process_client_command(cmd.command, std::move(payload));
    else process_server_command(cmd, std::move(payload));
}

void frp_signal_client::process_server_command(const frp_command_base& cmd, std::string payload) {
    // 任何 server 命令都是存活信号：重置信令假死计数
    ping_wait_count_ = 0;
    switch (cmd.command) {
    case frp_server_hello_command: {
        frp_server_hello_data hello;
        if (!Fundamental::io::from_json(payload, hello)) { signal_->release_obj(); return; }
        FINFO("server_hello nonce={}", hello.server_nonce);
        frp_auth_request_data req;
        req.command = frp_auth_request_command;
        req.digest = frp_hmac_sha256_hex(config_.traffic_secret, hello.server_nonce);
        signal_->send_command(req);
        signal_->read_next_command();
        return;
    }
    case frp_auth_response_command: {
        frp_auth_response_data resp;
        if (!Fundamental::io::from_json(payload, resp) || !resp.ok) {
            FERR("auth failed uuid={}", uuid_); signal_->release_obj(); return;
        }
        FINFO("auth ok uuid={}", uuid_);
        if (config_.public_server_udp_port != 0) {
            run_nat_probe();
        } else {
            if (on_server_cmd_) { frp_command_base d; d.command = frp_auth_response_command; on_server_cmd_(d, std::string{}); }
        }
        // 认证后无条件启动信令保活轮询（ping）：provider 无 listener 时
        // 没有 subscribe_resp 可触发轮询，否则保活永不启动
        start_polling();
        signal_->read_next_command();
        return;
    }
    case frp_register_services_resp_command: {
        frp_register_services_resp_data resp;
        if (!Fundamental::io::from_json(payload, resp)) { signal_->read_next_command(); return; }
        if (!resp.ok) { FERR("register failed msg={}", resp.message); signal_->read_next_command(); return; }
        FINFO("register ok uuid={}", uuid_);
        signal_->read_next_command();
        return;
    }
    case frp_subscribe_services_resp_command: {
        frp_subscribe_services_resp_data resp;
        if (!Fundamental::io::from_json(payload, resp) || !resp.ok) { signal_->read_next_command(); return; }
        {
            std::unordered_set<std::string> cur;
            for (const auto& svc : resp.services)
                cur.insert(Fundamental::StringFormat("{}@{}", svc.service_name, svc.provider_uuid));
            if (cur != last_known_services_) {
                FINFO("subscribe services={} (changed)", resp.services.size());
                last_known_services_ = std::move(cur);
                if (on_subscribe_) on_subscribe_(resp.services);
            }
        }
        start_polling();
        signal_->read_next_command();
        return;
    }
    case frp_signal_pong_command: {
        // 信令 ping 响应：存活确认（计数已在入口重置）
        signal_->read_next_command();
        return;
    }
    default:
        signal_->read_next_command();
        return;
    }
}

void frp_signal_client::process_client_command(std::uint8_t cmd, std::string payload) {
    if (on_client_cmd_) on_client_cmd_(cmd, std::move(payload));
    signal_->read_next_command();
}

void frp_signal_client::send_p2p_command(const std::string& peer_uuid, std::string json_payload) {
    if (!reference_.is_valid() || !signal_) return;
    frp_forward_command_data fwd;
    fwd.command = frp_forward_command;
    fwd.dst_uuid = peer_uuid;
    fwd.payload = std::move(json_payload);
    signal_->send_command(fwd);
}

void frp_signal_client::run_nat_probe() {
    if (config_.public_server_udp_port == 0) return;
    auto self = shared_from_this();
    auto executor = reconnect_timer_.get_executor();
    FINFO("run_nat_probe to {}:{} / {}", config_.public_server_host,
          config_.public_server_udp_port, config_.public_server_udp_port + 1);

    static constexpr std::size_t kMaxAttempts = 10;
    std::vector<std::uint16_t> udp_ports = {
        config_.public_server_udp_port,
        static_cast<std::uint16_t>(config_.public_server_udp_port + 1)
    };

    struct probe_state {
        std::vector<std::uint8_t> traffic_key;
        std::array<char, 2048> recv_buf{};
        asio::ip::udp::endpoint recv_endpoint;
        std::string result1;
        std::string result2;
        std::size_t port_index = 0;
        std::size_t attempts = 0;
        bool done = false;
        std::int64_t send_timestamp = 0;
        std::uint32_t rtt_ms = 0;
        std::string public_server_host;
        std::vector<std::uint16_t> udp_ports;
    };

    auto state = std::make_shared<probe_state>();
    state->public_server_host = config_.public_server_host;
    state->udp_ports = std::move(udp_ports);
    state->traffic_key = frp_derive_kcp_key(config_.traffic_secret);

    self->probe_socket_ = std::make_shared<asio::ip::udp::socket>(reconnect_timer_.get_executor());
    std::error_code ec;
    self->probe_socket_->open(asio::ip::udp::v4(), ec);
    if (ec) { run_time_sync(); return; }
    self->probe_socket_->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0), ec);
    if (ec) { run_time_sync(); return; }

    auto do_probe = std::make_shared<std::function<void()>>();
    auto do_recv  = std::make_shared<std::function<void()>>();

    *do_recv = [state, do_probe, do_recv, self]() mutable {
        self->probe_socket_->async_receive_from(
            asio::buffer(state->recv_buf.data(), state->recv_buf.size()), state->recv_endpoint,
            [state, do_probe, do_recv, self](const std::error_code& ec2, std::size_t bytes_read) mutable {
                if (state->done) return;
                if (!ec2 && bytes_read > 0) {
                    std::vector<std::uint8_t> encrypted(state->recv_buf.data(), state->recv_buf.data() + bytes_read);
                    auto plaintext = frp_kcp_decrypt(state->traffic_key, encrypted);
                    if (plaintext) {
                        std::string payload(plaintext->begin(), plaintext->end());
                        frp_udp_echo_data echo;
                        if (Fundamental::io::from_json(payload, echo) && !echo.external_ip.empty()) {
                            self->probe_timer_.cancel();
                            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            if (state->send_timestamp > 0 && now_ms > state->send_timestamp)
                                state->rtt_ms = static_cast<std::uint32_t>(now_ms - state->send_timestamp);
                            auto result = Fundamental::StringFormat("{}:{}", echo.external_ip, echo.external_port);
                            if (state->port_index == 0) state->result1 = result;
                            else state->result2 = result;
                            FINFO("nat_probe port={} result={} rtt={}ms", state->port_index, result, state->rtt_ms);
                            state->port_index++;
                            state->attempts = 0;
                            if (state->port_index >= state->udp_ports.size()) {
                                state->done = true;
                                { self->probe_socket_.reset(); }
                                if (state->result1.empty()) {
                                    self->probed_nat_type_ = frp_nat_type_disabled;
                                } else if (state->udp_ports.size() == 1 || state->result1 == state->result2) {
                                    self->probed_nat_type_ = frp_nat_type_cone;
                                } else {
                                    self->probed_nat_type_ = frp_nat_type_symmetric;
                                }
                                self->startup_rtt_ms_ = state->rtt_ms;
                                FINFO("nat_probe done nat_type={} rtt={}ms", static_cast<int>(self->probed_nat_type_), self->startup_rtt_ms_);
                                self->run_time_sync();
                                return;
                            }
                            (*do_probe)();
                            return;
                        }
                    }
                }
                if (!state->done) (*do_recv)();
            });
    };

    *do_probe = [state, do_probe, do_recv, self, executor]() mutable {
        if (state->done) return;
        if (state->attempts >= kMaxAttempts) {
            state->done = true;
            { self->probe_socket_.reset(); }
            FWARN("nat_probe port={} timeout after {} attempts", state->port_index, kMaxAttempts);
            self->probed_nat_type_ = frp_nat_type_disabled;
            self->run_time_sync();
            return;
        }
        auto server_ep = resolve_udp_endpoint(executor, state->public_server_host,
                                              state->udp_ports[state->port_index]);
        if (!server_ep) {
            state->done = true;
            { self->probe_socket_.reset(); }
            self->run_time_sync();
            return;
        }
        std::error_code local_ec;
        auto local_port = self->probe_socket_->local_endpoint(local_ec).port();
        frp_p2p_probe_data probe;
        probe.command = frp_p2p_probe_command;
        probe.local_port = local_port;
        auto encrypted = frp_kcp_encrypt_string(state->traffic_key, Fundamental::io::to_json(probe));
        if (encrypted.empty()) { self->run_time_sync(); return; }
        state->attempts++;
        state->send_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto enc_ptr = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted));
        self->probe_socket_->async_send_to(asio::buffer(*enc_ptr), *server_ep,
                                     [enc_ptr](const std::error_code&, std::size_t) {});
        self->probe_timer_.expires_after(std::chrono::milliseconds(200));
        self->probe_timer_.async_wait([state, do_probe](const std::error_code& ec3) mutable {
            if (ec3 || state->done) return;
            (*do_probe)();
        });
        if (state->attempts == 1) (*do_recv)();
    };

    (*do_probe)();
}

void frp_signal_client::run_time_sync() {
    if (config_.public_server_udp_port == 0) {
        if (on_server_cmd_) { frp_command_base d; d.command = frp_auth_response_command; on_server_cmd_(d, std::string{}); }
        return;
    }
    auto self = shared_from_this();
    auto executor = reconnect_timer_.get_executor();

    auto server_ep = resolve_udp_endpoint(executor, config_.public_server_host, config_.public_server_udp_port);
    if (!server_ep) {
        if (on_server_cmd_) { frp_command_base d; d.command = frp_auth_response_command; on_server_cmd_(d, std::string{}); }
        return;
    }

    struct sync_sample { std::int64_t offset_us = 0; std::int64_t delay_us = 0; };
    struct sync_state {
        std::vector<std::uint8_t> traffic_key;
        asio::ip::udp::endpoint server_ep;
        std::vector<sync_sample> samples;
        std::uint32_t seq = 0;
        int sends_done = 0;
        int sends_at_three = 0;
        std::array<char, 2048> recv_buf{};
        asio::ip::udp::endpoint recv_ep;
    };

    auto s = std::make_shared<sync_state>();
    s->traffic_key = frp_derive_kcp_key(config_.traffic_secret);
    s->server_ep = *server_ep;

    {
        self->probe_socket_ = std::make_shared<asio::ip::udp::socket>(executor);
        std::error_code ec;
        self->probe_socket_->open(asio::ip::udp::v4(), ec);
        if (ec) { if (on_server_cmd_) { frp_command_base d; d.command = frp_auth_response_command; on_server_cmd_(d, std::string{}); } return; }
        self->probe_socket_->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0), ec);
        if (ec) { if (on_server_cmd_) { frp_command_base d; d.command = frp_auth_response_command; on_server_cmd_(d, std::string{}); } return; }
    }

    auto do_send = std::make_shared<std::function<void()>>();
    auto do_recv  = std::make_shared<std::function<void()>>();

    *do_recv = [s, do_recv, self]() {
        self->probe_socket_->async_receive_from(
            asio::buffer(s->recv_buf.data(), s->recv_buf.size()), s->recv_ep,
            [s, do_recv, self](const std::error_code& ec, std::size_t bytes_read) {
                if (ec) return;
                std::vector<std::uint8_t> encrypted(s->recv_buf.data(), s->recv_buf.data() + bytes_read);
                auto plaintext = frp_kcp_decrypt(s->traffic_key, encrypted);
                if (!plaintext) return;
                std::string payload(plaintext->begin(), plaintext->end());
                frp_time_sync_response_data resp;
                if (!Fundamental::io::from_json(payload, resp)) return;
                if (resp.command != frp_time_sync_response_command || resp.seq != s->seq) return;
                std::int64_t T4 = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::int64_t offset = ((resp.server_recv_ts - resp.client_send_ts) +
                                       (resp.server_send_ts - T4)) / 2;
                std::int64_t delay = (T4 - resp.client_send_ts) - (resp.server_send_ts - resp.server_recv_ts);
                s->samples.push_back({offset, delay});
                if (s->samples.size() == 3 && s->sends_at_three == 0)
                    s->sends_at_three = s->sends_done;
                (*do_recv)();
            });
    };

    *do_send = [s, do_send, self]() {
        bool stop = false;
        if (s->sends_at_three > 0 && s->sends_done >= s->sends_at_three + 7) stop = true;
        if (s->sends_done >= 30) stop = true;
        if (stop) {
            std::error_code ignore;
            self->probe_socket_->close(ignore);
            self->probe_socket_.reset();
            int n = static_cast<int>(s->samples.size());
            if (n < 3) {
                FWARN("time_sync failed: only {} valid samples", n);
                if (self->on_server_cmd_) { frp_command_base d; d.command = frp_auth_response_command; self->on_server_cmd_(d, std::string{}); }
                return;
            }
            std::sort(s->samples.begin(), s->samples.end(),
                      [](const sync_sample& a, const sync_sample& b) { return a.delay_us < b.delay_us; });
            std::int64_t sum = 0;
            int count = 0;
            int start = (n >= 5) ? 1 : 0;
            int end   = (n >= 5) ? n - 1 : n;
            for (int i = start; i < end; i++) { sum += s->samples[i].offset_us; count++; }
            self->server_clock_offset_us_ = sum / count;
            self->startup_rtt_ms_ = static_cast<std::uint32_t>(s->samples[0].delay_us / 1000);
            FINFO("time_sync done offset={}us samples={} avg_rtt={}ms", self->server_clock_offset_us_, count, self->startup_rtt_ms_);
            self->reconnect_delay_seconds_ = 2;
            if (self->on_server_cmd_) { frp_command_base d; d.command = frp_auth_response_command; self->on_server_cmd_(d, std::string{}); }
            return;
        }
        s->sends_done++;
        s->seq++;
        std::int64_t T1 = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        frp_time_sync_request_data req;
        req.command = frp_time_sync_request_command;
        req.seq = s->seq;
        req.client_send_ts = T1;
        auto encrypted = frp_kcp_encrypt_string(s->traffic_key, Fundamental::io::to_json(req));
        if (encrypted.empty()) { if (self->on_server_cmd_) { frp_command_base d; d.command = frp_auth_response_command; self->on_server_cmd_(d, std::string{}); } return; }
        auto enc_ptr = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted));
        self->probe_socket_->async_send_to(asio::buffer(*enc_ptr), s->server_ep,
            [enc_ptr](const std::error_code&, std::size_t) {});
        self->probe_timer_.expires_after(std::chrono::milliseconds(100));
        self->probe_timer_.async_wait([do_send](const std::error_code& ec) {
            if (!ec) (*do_send)();
        });
    };

    (*do_recv)();
    (*do_send)();
}

// =============================================================================
// Polling
// =============================================================================


void frp_signal_client::start_polling() {
    if (!reference_.is_valid()) return;
    poll_timer_.expires_after(std::chrono::seconds(30));
    poll_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {
        if (ec || !reference_.is_valid()) return;
        do_poll();
        start_polling(); // 循环：每 30s 一次信令 ping（保活 + 假死检测）
    });
}

void frp_signal_client::do_poll() {
    // 信令保活：30s ping 一次；server 回 pong 证明存活并重置其空闲超时。
    // 连续 3 次无任何响应 -> 判定对端假死 -> 断开（触发重连）
    if (++ping_wait_count_ > 3) {
        FWARN("signal ping timeout uuid={}, peer considered dead", uuid_);
        signal_->release_obj();
        return;
    }
    frp_command_base ping;
    ping.command = frp_signal_ping_command;
    if (signal_) signal_->send_command(ping);
}

} // namespace network::proxy