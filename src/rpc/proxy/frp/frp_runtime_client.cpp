#include "frp_runtime_client.hpp"

#include "frp_proxy_data_channel.hpp"
#include "frp_punch_engine.hpp"
#include "frp_runtime_common.hpp"
#include "frp_kcp_crypto.hpp"
#include "fundamental/basic/base64_utils.hpp"

#include <algorithm>
#include <random>
#include <set>

namespace network::proxy
{

namespace
{

std::optional<asio::ip::udp::endpoint> resolve_udp_endpoint(const asio::any_io_executor& executor,
                                                            const std::string& host,
                                                            std::uint16_t port) {
    std::error_code ec;
    auto address = asio::ip::make_address(host, ec);
    if (!ec) {
        if (address.is_v6()) {
            FINFO("resolve_udp_endpoint: rejecting IPv6 address {}, falling back to v4 DNS for {}", host, host);
        } else {
            return asio::ip::udp::endpoint(address, port);
        }
    }

    asio::ip::udp::resolver resolver(executor);
    auto endpoints = resolver.resolve(asio::ip::udp::v4(), host, std::to_string(port), ec);
    if (ec) {
        FINFO("resolve_udp_endpoint: v4 DNS resolution failed for {}:{} err={}", host, port, ec.message());
        return std::nullopt;
    }
    auto it = endpoints.begin();
    if (it == endpoints.end()) {
        return std::nullopt;
    }
    return it->endpoint();
}

} // namespace



frp_runtime_signal_client_channel::frp_runtime_signal_client_channel(const asio::any_io_executor& executor,
                                                                     std::string host,
                                                                     std::string service) :
executor_(executor), host_(std::move(host)), service_(std::move(service)) {
#ifndef NETWORK_DISABLE_SSL
    ssl_config_.disable_ssl = true;
#endif
}

void frp_runtime_signal_client_channel::enable_ssl(network_client_ssl_config config) {
    ssl_config_ = std::move(config);
}

void frp_runtime_signal_client_channel::set_on_connected(connect_callback_t cb) {
    on_connected_ = std::move(cb);
}

void frp_runtime_signal_client_channel::set_on_disconnected(disconnect_callback_t cb) {
    on_disconnected_ = std::move(cb);
}

void frp_runtime_signal_client_channel::set_on_command(command_callback_t cb) {
    on_command_ = std::move(cb);
}

void frp_runtime_signal_client_channel::start() {
    if (!reference_.is_valid()) return;
    upstream_ = frp_client_upstream::make_shared(executor_, host_, service_);
    upstream_->enable_ssl(ssl_config_);
    upstream_->notify_connect_result.Connect(
        shared_from_this(), [this](Fundamental::error_code ec, std::shared_ptr<frp_client_upstream> upstream) {
            if (!reference_.is_valid()) return;
            if (ec || !upstream) {
                notify_disconnect_once();
                return;
            }
            upstream_ = std::move(upstream);
            transport_ = upstream_;
            frp_runtime_signal_open_data open;
            open.command = frp_runtime_signal_open_command;
            send_command(open);
            if (on_connected_) on_connected_();
            read_next_command();
        });
    upstream_->start_async_connect();
}

void frp_runtime_signal_client_channel::release_obj() {
    if (!reference_.release()) return;
    asio::post(executor_, [this, self = shared_from_this()] {
        if (upstream_) {
            upstream_->release_obj();
            upstream_ = nullptr;
        }
        transport_ = nullptr;
        notify_disconnect_once();
    });
}

void frp_runtime_signal_client_channel::read_next_command() {
    if (!transport_) return;
    transport_->async_buffers_read(
        { network_read_buffer_t { header_buf_.data(), header_buf_.size() } },
        [this, self = shared_from_this()](std::error_code ec, std::size_t) {
            if (!reference_.is_valid()) return;
            if (ec || !transport_) {
                release_obj();
                return;
            }
            std::uint32_t payload_len = 0;
            Fundamental::net_buffer_copy(header_buf_.data(), &payload_len, 4);
            if (payload_len == 0 || payload_len > frp_runtime_command_base::kMaxCommandPayloadLen) {
                release_obj();
                return;
            }
            payload_.resize(payload_len);
            transport_->async_buffers_read(
                { network_read_buffer_t { payload_.data(), payload_.size() } },
                [this, self = shared_from_this()](std::error_code ec, std::size_t) {
                    if (!reference_.is_valid()) return;
                    if (ec || !transport_) {
                        release_obj();
                        return;
                    }
                    frp_runtime_command_base base_command;
                    if (!Fundamental::io::from_json(payload_, base_command)) {
                        release_obj();
                        return;
                    }
                    if (on_command_) on_command_(base_command, payload_);
                    if (reference_.is_valid()) read_next_command();
                });
        });
}

void frp_runtime_signal_client_channel::do_write() {
    if (!transport_ || write_queue_.empty()) return;
    auto& current = write_queue_.front();
    transport_->async_buffers_write(
        { network_write_buffer_t { current->data(), current->size() } },
        [this, self = shared_from_this()](std::error_code ec, std::size_t) {
            if (!reference_.is_valid()) return;
            if (ec || !transport_) {
                release_obj();
                return;
            }
            write_queue_.pop_front();
            if (!write_queue_.empty()) do_write();
        });
}

void frp_runtime_signal_client_channel::notify_disconnect_once() {
    if (disconnect_notified_) return;
    disconnect_notified_ = true;
    if (on_disconnected_) on_disconnected_();
}

frp_runtime_data_client_channel::frp_runtime_data_client_channel(const asio::any_io_executor& executor,
                                                                 std::string host,
                                                                 std::string service,
                                                                 std::uint32_t flow_id,
                                                                 std::string uuid) :
executor_(executor),
host_(std::move(host)),
service_(std::move(service)),
flow_id_(flow_id),
uuid_(std::move(uuid)) {
#ifndef NETWORK_DISABLE_SSL
    ssl_config_.disable_ssl = true;
#endif
}

void frp_runtime_data_client_channel::enable_ssl(network_client_ssl_config config) {
    ssl_config_ = std::move(config);
}

void frp_runtime_data_client_channel::set_on_connected(connect_callback_t cb) {
    on_connected_ = std::move(cb);
}

void frp_runtime_data_client_channel::set_on_disconnected(disconnect_callback_t cb) {
    on_disconnected_ = std::move(cb);
}

void frp_runtime_data_client_channel::set_on_data(data_callback_t cb) {
    on_data_ = std::move(cb);
}

void frp_runtime_data_client_channel::start() {
    if (!reference_.is_valid()) return;
    upstream_ = frp_client_upstream::make_shared(executor_, host_, service_);
    upstream_->enable_ssl(ssl_config_);
    upstream_->notify_connect_result.Connect(
        shared_from_this(), [this](Fundamental::error_code ec, std::shared_ptr<frp_client_upstream> upstream) {
            if (!reference_.is_valid()) return;
            if (ec || !upstream) {
                notify_disconnect_once();
                return;
            }
            upstream_  = std::move(upstream);
            transport_ = upstream_;
            frp_runtime_data_open_data open_request;
            open_request.command = frp_runtime_data_open_command;
            open_request.flow_id = flow_id_;
            open_request.uuid    = uuid_;
            auto open = packet_frp_runtime_command_data(open_request);
            if (!open) {
                release_obj();
                return;
            }
            write_queue_.push_back(std::move(open));
            do_write();
            if (on_connected_) on_connected_();
            read_next_chunk();
        });
    upstream_->start_async_connect();
}

void frp_runtime_data_client_channel::release_obj() {
    if (!reference_.release()) return;
    asio::post(executor_, [this, self = shared_from_this()] {
        if (upstream_) {
            upstream_->release_obj();
            upstream_ = nullptr;
        }
        transport_ = nullptr;
        notify_disconnect_once();
    });
}

void frp_runtime_data_client_channel::send_bytes(const std::shared_ptr<std::string>& data) {
    asio::post(executor_, [this, self = shared_from_this(), data] {
        if (!reference_.is_valid() || !transport_ || !data) return;
        write_queue_.push_back(data);
        if (write_queue_.size() == 1) {
            do_write();
        }
    });
}

std::string frp_runtime_data_client_channel::local_endpoint_string() const {
    if (!upstream_) return "unknown";
    return upstream_->local_endpoint_string();
}

std::string frp_runtime_data_client_channel::remote_endpoint_string() const {
    if (!upstream_) return "unknown";
    return upstream_->remote_endpoint_string();
}

void frp_runtime_data_client_channel::read_next_chunk() {
    if (!transport_) return;
    transport_->async_buffers_read_some(
        { network_read_buffer_t { read_buf_.data(), read_buf_.size() } },
        [this, self = shared_from_this()](std::error_code ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (ec || !transport_) {
                release_obj();
                return;
            }
            if (bytes_read == 0) {
                release_obj();
                return;
            }
            if (on_data_) on_data_(std::string(read_buf_.data(), bytes_read));
            read_next_chunk();
        });
}

void frp_runtime_data_client_channel::do_write() {
    if (!transport_ || write_queue_.empty()) return;
    auto& current = write_queue_.front();
    transport_->async_buffers_write(
        { network_write_buffer_t { current->data(), current->size() } },
        [this, self = shared_from_this()](std::error_code ec, std::size_t) {
            if (!reference_.is_valid()) return;
            if (ec || !transport_) {
                release_obj();
                return;
            }
            write_queue_.pop_front();
            if (!write_queue_.empty()) do_write();
        });
}

void frp_runtime_data_client_channel::notify_disconnect_once() {
    if (disconnect_notified_) return;
    disconnect_notified_ = true;
    if (on_disconnected_) on_disconnected_();
}

static constexpr std::size_t kMaxEndpointProbeAttempts = 10;
static constexpr int kPunchMaxRounds       = 32;
static constexpr int kPunchSocketCount     = 32;   // symmetric side: local sockets
static constexpr int kPunchScanRound0      = 64;   // full side: round-0 port count
static constexpr int kPunchScanPerRound    = 128;  // full side: rounds 1+ port count
static constexpr int kPunchRetransmitCount = 5;
static constexpr int kPunchRetransmitMs    = 100;
static constexpr int kPunchRoundMs         = 1000;

void run_startup_probe(const asio::any_io_executor& executor,
                       const std::string& traffic_secret,
                       const std::string& public_server_host,
                       const std::vector<std::uint16_t>& udp_ports,
                       std::function<void(frp_runtime_nat_type, std::uint32_t rtt_ms)> on_done) {
    if (udp_ports.empty()) {
        on_done(frp_runtime_nat_type_disabled, 0);
        return;
    }

    struct probe_state {
        std::unique_ptr<asio::ip::udp::socket> socket;
        asio::steady_timer timer;
        std::vector<std::uint8_t> traffic_key;
        std::array<char, 2048> recv_buf {};
        asio::ip::udp::endpoint recv_endpoint;
        std::string result1;
        std::string result2;
        std::size_t port_index = 0;
        std::size_t attempts = 0;
        bool done = false;
        std::function<void(frp_runtime_nat_type, std::uint32_t)> on_done;
        std::uint32_t rtt_ms = 0;
        std::int64_t send_timestamp = 0;
        std::string public_server_host;
        std::vector<std::uint16_t> udp_ports;
        std::string traffic_secret;
        asio::any_io_executor executor;

        probe_state(const asio::any_io_executor& ex) : socket(std::make_unique<asio::ip::udp::socket>(ex)), timer(ex), executor(ex) {}
    };

    auto state = std::make_shared<probe_state>(executor);
    state->on_done = std::move(on_done);
    state->public_server_host = public_server_host;
    state->udp_ports = udp_ports;
    state->traffic_secret = traffic_secret;
    state->traffic_key = frp_derive_kcp_flow_key(traffic_secret, 0);

    std::error_code ec;
    state->socket->open(asio::ip::udp::v4(), ec);
    if (ec) {
        FINFO("startup_probe socket open failed err={}", ec.message());
        state->on_done(frp_runtime_nat_type_disabled, 0);
        return;
    }
    state->socket->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0), ec);
    if (ec) {
        FINFO("startup_probe socket bind failed err={}", ec.message());
        state->on_done(frp_runtime_nat_type_disabled, 0);
        return;
    }
    FINFO("startup_probe socket bound local_port={}", state->socket->local_endpoint(ec).port());

    auto do_probe = std::make_shared<std::function<void()>>();
    auto do_recv  = std::make_shared<std::function<void()>>();

    *do_recv = [state, do_probe, do_recv]() mutable {
        state->socket->async_receive_from(
            asio::buffer(state->recv_buf.data(), state->recv_buf.size()),
            state->recv_endpoint,
            [state, do_probe, do_recv](const std::error_code& ec, std::size_t bytes_read) mutable {
                if (state->done) return;
                if (!ec && bytes_read > 0) {
                    FINFO("startup_probe recv bytes={} from={}:{}", bytes_read,
                          state->recv_endpoint.address().to_string(), state->recv_endpoint.port());
                    std::vector<std::uint8_t> encrypted(state->recv_buf.data(), state->recv_buf.data() + bytes_read);
                    auto plaintext = frp_kcp_decrypt(state->traffic_key, encrypted);
                    if (plaintext) {
                        std::string payload(plaintext->begin(), plaintext->end());
                        FINFO("startup_probe decrypted payload_size={} payload={}", plaintext->size(), payload);
                        frp_runtime_udp_echo_data echo;
                        if (Fundamental::io::from_json(payload, echo) && !echo.external_ip.empty()) {
                            state->timer.cancel();
                            auto now_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            if (state->send_timestamp > 0 && now_ts > state->send_timestamp) {
                                state->rtt_ms = static_cast<std::uint32_t>(now_ts - state->send_timestamp);
                            }
                            auto result = Fundamental::StringFormat("{}:{}", echo.external_ip, echo.external_port);
                            FINFO("startup_probe port_index={} received echo external={} rtt={}ms",
                                  state->port_index, result, state->rtt_ms);
                            if (state->port_index == 0) {
                                state->result1 = result;
                            } else {
                                state->result2 = result;
                            }
                            state->port_index++;
                            state->attempts = 0;
                            if (state->port_index >= state->udp_ports.size()) {
                                state->done = true;
                                { std::error_code ignore; state->socket->close(ignore); }
                                frp_runtime_nat_type detected;
                                if (state->result1.empty()) {
                                    detected = frp_runtime_nat_type_disabled;
                                } else if (state->udp_ports.size() == 1 || state->result1 == state->result2) {
                                    detected = frp_runtime_nat_type_cone;
                                } else {
                                    detected = frp_runtime_nat_type_symmetric;
                                }
                                FINFO("startup_probe probe_result_1={} probe_result_2={} detected_nat_type={}",
                                      state->result1, state->result2, static_cast<int>(detected));
                                state->on_done(detected, state->rtt_ms);
                                return;
                            }
                            (*do_probe)();
                            return;
                        }
                        FINFO("startup_probe recv unexpected payload: flow_id not 0 or parse failed");
                    } else {
                        FINFO("startup_probe recv decrypt failed, ignoring packet from={}:{}",
                              state->recv_endpoint.address().to_string(), state->recv_endpoint.port());
                    }
                }
                if (!state->done) (*do_recv)();
            });
    };

    *do_probe = [state, do_probe, do_recv]() mutable {
        if (state->done) return;
        if (state->attempts >= kMaxEndpointProbeAttempts) {
            state->done = true;
            { std::error_code ignore; state->socket->close(ignore); }
            FINFO("startup_probe probe_result_1={} probe_result_2={} p2p_probe_result=failed (timeout)",
                  state->result1, state->result2);
            state->on_done(frp_runtime_nat_type_disabled, state->rtt_ms);
            return;
        }
        auto server_endpoint = resolve_udp_endpoint(state->executor, state->public_server_host,
                                                    state->udp_ports[state->port_index]);
        if (!server_endpoint) {
            state->done = true;
            { std::error_code ignore; state->socket->close(ignore); }
            state->on_done(frp_runtime_nat_type_disabled, state->rtt_ms);
            return;
        }
        std::error_code local_ec;
        auto local_port = state->socket->local_endpoint(local_ec).port();
        frp_runtime_p2p_probe_data probe;
        probe.command    = frp_runtime_p2p_probe_command;
        probe.local_port = local_port;
        auto payload = Fundamental::io::to_json(probe);
        auto encrypted = frp_kcp_encrypt_string(state->traffic_key, payload);
        if (encrypted.empty()) {
            state->done = true;
            { std::error_code ignore; state->socket->close(ignore); }
            state->on_done(frp_runtime_nat_type_disabled, state->rtt_ms);
            return;
        }
        state->attempts++;
        state->send_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        FINFO("startup_probe send attempt={} port_index={} local_port={} server={}:{} probe_size={}",
              state->attempts, state->port_index, local_port,
              server_endpoint->address().to_string(), server_endpoint->port(), encrypted.size());
        auto enc_ptr = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted));
        state->socket->async_send_to(asio::buffer(*enc_ptr), *server_endpoint,
                                     [enc_ptr](const std::error_code&, std::size_t) {});
        state->timer.expires_after(std::chrono::milliseconds(200));
        state->timer.async_wait([state, do_probe](const std::error_code& ec) mutable {
            if (ec || state->done) return;
            (*do_probe)();
        });
        if (state->attempts == 1) {
            (*do_recv)();
        }
    };

    (*do_probe)();
}

// ---------------------------------------------------------------------------
// Provider punch engine helpers
// ---------------------------------------------------------------------------

static constexpr int kMaxPunchRetries       = 10000;
static constexpr int kPunchRetryStepSeconds = 10;
static constexpr int kPunchRetryMaxSeconds  = 60;

static int punch_retry_delay(int retry_count) {
    // 10s -> 20s -> 30s -> ... -> 60s capped
    int delay = retry_count * kPunchRetryStepSeconds;
    return std::min(delay, kPunchRetryMaxSeconds);
}






// ---------------------------------------------------------------------------
// frp_runtime_unified_client_agent
// ---------------------------------------------------------------------------

void frp_runtime_unified_client_agent::run_time_sync() {
    if (!config_.public_server_udp_port) {
        connect_signal_channel();
        return;
    }
    auto executor = reconnect_timer_.get_executor();
    auto traffic_key = frp_derive_kcp_flow_key(config_.traffic_secret, 0);
    auto server_ep = resolve_udp_endpoint(executor, config_.public_server_host, config_.public_server_udp_port);
    if (!server_ep) { schedule_reconnect(); return; }

    struct sync_sample { std::int64_t offset_us = 0; std::int64_t delay_us = 0; };
    struct sync_state {
        asio::any_io_executor executor;
        std::unique_ptr<asio::ip::udp::socket> socket;
        std::vector<std::uint8_t> traffic_key;
        asio::ip::udp::endpoint server_ep;
        std::vector<sync_sample> samples;
        std::uint32_t seq = 0;
        int sends_done = 0;
        int sends_at_three = 0;   // sends_done when we reached 3 samples (0 = not yet)
        std::array<char, 2048> recv_buf{};
        asio::ip::udp::endpoint recv_ep;
        std::function<void(bool ok, std::int64_t offset_us)> on_done;
        asio::steady_timer send_timer;
        sync_state(const asio::any_io_executor& ex) : executor(ex), send_timer(ex) {}
    };
    auto s = std::make_shared<sync_state>(executor);
    s->traffic_key = std::move(traffic_key);
    s->server_ep = *server_ep;
    s->on_done = [this, self = shared_from_this()](bool ok, std::int64_t offset_us) {
        if (ok) {
            server_clock_offset_us_ = offset_us;
            FINFO("time_sync done offset_us={} offset_ms={}", offset_us, offset_us / 1000);
            reconnect_delay_seconds_ = 2;
        } else {
            FWARN("time_sync failed, will retry after reconnect");
        }
        connect_signal_channel();
    };

    {
        auto sock = std::make_unique<asio::ip::udp::socket>(executor);
        std::error_code ec;
        sock->open(asio::ip::udp::v4(), ec);
        if (ec) { schedule_reconnect(); return; }
        sock->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0), ec);
        if (ec) { schedule_reconnect(); return; }
        s->socket = std::move(sock);
    }

    auto do_send = std::make_shared<std::function<void()>>();
    auto do_recv  = std::make_shared<std::function<void()>>();

    *do_recv = [s, do_recv]() {
        s->socket->async_receive_from(
            asio::buffer(s->recv_buf.data(), s->recv_buf.size()), s->recv_ep,
            [s, do_recv](const std::error_code& ec, std::size_t bytes_read) {
                if (ec) return;
                std::vector<std::uint8_t> encrypted(s->recv_buf.data(), s->recv_buf.data() + bytes_read);
                auto plaintext = frp_kcp_decrypt(s->traffic_key, encrypted);
                if (!plaintext) return;
                std::string payload(plaintext->begin(), plaintext->end());
                frp_runtime_time_sync_response_data resp;
                if (!Fundamental::io::from_json(payload, resp) || resp.command != frp_runtime_time_sync_response_command) return;
                if (resp.seq != s->seq) return;

                std::int64_t T4 = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::int64_t offset = ((resp.server_recv_ts - resp.client_send_ts) +
                                       (resp.server_send_ts - T4)) / 2;
                std::int64_t delay = (T4 - resp.client_send_ts) - (resp.server_send_ts - resp.server_recv_ts);
                s->samples.push_back({offset, delay});
                FINFO("time_sync sample seq={} offset={}us delay={}us count={}", resp.seq, offset, delay, s->samples.size());
                if (s->samples.size() == 3 && s->sends_at_three == 0)
                    s->sends_at_three = s->sends_done;
                (*do_recv)();
            });
    };

    *do_send = [s, do_send]() {
        bool stop = false;
        if (s->sends_at_three > 0 && s->sends_done >= s->sends_at_three + 7)
            stop = true;
        if (s->sends_done >= 30)
            stop = true;

        if (stop) {
            std::error_code ignore;
            s->socket->close(ignore);
            int n = static_cast<int>(s->samples.size());
            if (n < 3) {
                s->on_done(false, 0);
                return;
            }
            std::sort(s->samples.begin(), s->samples.end(),
                      [](const sync_sample& a, const sync_sample& b) { return a.delay_us < b.delay_us; });
            std::int64_t sum = 0;
            int count = 0;
            int start = (n >= 5) ? 1 : 0;
            int end   = (n >= 5) ? n - 1 : n;
            for (int i = start; i < end; i++) {
                sum += s->samples[i].offset_us;
                count++;
            }
            s->on_done(true, sum / count);
            return;
        }
        s->sends_done++;
        s->seq++;
        std::int64_t T1 = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        frp_runtime_time_sync_request_data req;
        req.command = frp_runtime_time_sync_request_command;
        req.seq = s->seq;
        req.client_send_ts = T1;
        auto payload = Fundamental::io::to_json(req);
        auto encrypted = frp_kcp_encrypt_string(s->traffic_key, payload);
        if (encrypted.empty()) { s->on_done(false, 0); return; }

        auto enc_ptr = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted));
        s->socket->async_send_to(asio::buffer(*enc_ptr), s->server_ep,
            [enc_ptr](const std::error_code&, std::size_t) {});
        FINFO("time_sync send seq={} send={}/max samples={}", s->seq, s->sends_done, s->samples.size());

        s->send_timer.expires_after(std::chrono::milliseconds(100));
        s->send_timer.async_wait([do_send](const std::error_code& ec) {
            if (!ec) (*do_send)();
        });
    };

    (*do_recv)();
    (*do_send)();
}

// ---------------------------------------------------------------------------
// frp_runtime_unified_client_agent structs
// ---------------------------------------------------------------------------

struct frp_runtime_unified_client_agent::provider_flow_runtime {
    std::uint32_t flow_id = 0;
    std::string service_name;
    std::string accessor_uuid;
    std::uint8_t transport = frp_runtime_transport_tcp_relay;
    bool enable_p2p = true;
    std::shared_ptr<frp_proxy_data_channel> data_channel;
    std::shared_ptr<frp_punch_engine> punch_engine;
    int punch_retry_count = 0;
    asio::steady_timer punch_retry_timer;
    asio::ip::tcp::socket backend_socket;
    asio::ip::tcp::resolver resolver;
    std::unique_ptr<asio::ip::udp::socket> backend_udp_socket;
    asio::ip::udp::endpoint backend_udp_target;
    std::array<char, 65536> udp_recv_buf{};
    std::array<char, 16 * 1024> read_buf{};
    std::deque<std::string> pending_writes;
    bool writing = false;
    bool backend_connected = false;
    bool closed = false;
    std::uint32_t expected_punch_seq = 0;
    asio::steady_timer handshake_timer;
    explicit provider_flow_runtime(const asio::any_io_executor& e)
        : punch_retry_timer(e), backend_socket(e), resolver(e), handshake_timer(e) {}
};

struct frp_runtime_unified_client_agent::accessor_session_context {
    std::uint64_t session_id = 0;
    std::uint32_t flow_id = 0;
    std::string service_name;
    std::uint8_t service_type = frp_runtime_service_tcp;
    std::uint8_t provider_nat_type = frp_runtime_nat_type_disabled;
    std::uint32_t provider_startup_rtt_ms = 100;
    bool enable_p2p = true;
    bool provider_enable_p2p = true;
    std::string register_key;
    std::string provider_uuid;
    asio::ip::tcp::socket local_socket;
    asio::ip::udp::endpoint remote_endpoint;
    asio::ip::udp::socket* udp_socket = nullptr;
    std::shared_ptr<frp_proxy_data_channel> data_channel;
    std::shared_ptr<frp_punch_engine> punch_engine;
    int punch_retry_count = 0;
    asio::steady_timer punch_retry_timer;
    std::array<char, 16 * 1024> read_buf{};
    std::deque<std::string> pending_writes;
    std::deque<std::string> pending_forwards;
    bool writing = false;
    bool ready = false;
    bool peer_closed = false;
    bool closed = false;
    bool probe_confirm_sent = false;
    std::uint32_t pending_punch_seq = 0;
    asio::steady_timer ack_timer;
    std::function<void()> on_destroy;

    ~accessor_session_context() { if (on_destroy) on_destroy(); }

    explicit accessor_session_context(std::uint64_t sid, asio::ip::tcp::socket&& s)
        : session_id(sid), local_socket(std::move(s)), punch_retry_timer(s.get_executor()), ack_timer(s.get_executor()) {}
    explicit accessor_session_context(std::uint64_t sid, const asio::ip::udp::endpoint& ep,
                                       const asio::any_io_executor& e)
        : session_id(sid), local_socket(e), remote_endpoint(ep), punch_retry_timer(e), ack_timer(e) {}
};

struct frp_runtime_unified_client_agent::listener_runtime {
    std::string service_name;
    std::string listen_host;
    std::uint16_t listen_port = 0;
    std::uint8_t service_type = frp_runtime_service_tcp;
    std::uint8_t provider_nat_type = frp_runtime_nat_type_disabled;
    std::uint32_t provider_startup_rtt_ms = 100;
    bool enable_p2p = true;
    bool provider_enable_p2p = true;
    std::string register_key;
    asio::ip::tcp::acceptor acceptor;
    std::unique_ptr<asio::ip::udp::socket> udp_socket;
    std::unordered_map<asio::ip::udp::endpoint, std::weak_ptr<accessor_session_context>> udp_sessions;
    asio::ip::udp::endpoint udp_recv_endpoint_;
    std::array<char, 65536> udp_recv_buf{};

    listener_runtime(const asio::any_io_executor& executor, std::string sn, std::string lh, std::uint16_t lp)
        : service_name(std::move(sn)), listen_host(std::move(lh)), listen_port(lp), acceptor(executor) {}
};

// ---------------------------------------------------------------------------
// frp_runtime_unified_client_agent
// ---------------------------------------------------------------------------

frp_runtime_unified_client_agent::frp_runtime_unified_client_agent(frp_proxy_client_config config) :
config_(std::move(config)),
uuid_(frp_generate_runtime_uuid()),
reconnect_timer_(io_context_pool::Instance().get_io_context()),
poll_timer_(io_context_pool::Instance().get_io_context()) {
    // Build services map for provider-side lookups
    for (const auto& group : config_.groups) {
        for (const auto& svc : group.services)
            services_by_name_[svc.service_name] = svc;
    }
}

void frp_runtime_unified_client_agent::start() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    FINFO("client start local_steady_clock={}us", now);
    if (config_.nat_type != frp_runtime_nat_type_disabled && config_.public_server_udp_port != 0) {
        std::vector<std::uint16_t> ports = { config_.public_server_udp_port,
                                              static_cast<std::uint16_t>(config_.public_server_udp_port + 1) };
        run_startup_probe(reconnect_timer_.get_executor(), config_.traffic_secret,
                          config_.public_server_host, ports,
                          [this, self = shared_from_this()](frp_runtime_nat_type nat, std::uint32_t rtt) {
                              if (!reference_.is_valid()) return;
                              probed_nat_type_ = nat;
                              startup_rtt_ms_ = rtt;
                              run_time_sync();
                          });
    } else {
        run_time_sync();
    }
}

void frp_runtime_unified_client_agent::release_obj() {
    if (!reference_.release()) return;
    reconnect_timer_.cancel();
    poll_timer_.cancel();
    if (channel_) { channel_->release_obj(); channel_ = nullptr; }
    for (auto& [_, s] : accessor_sessions_) { if (s->data_channel) s->data_channel->release_obj(); }
    for (auto& [_, s] : pending_sessions_) { if (s->data_channel) s->data_channel->release_obj(); }
    for (auto& [_, f] : provider_flows_) { if (f->data_channel) f->data_channel->release_obj(); }
    for (auto& [_, l] : listeners_) {
        std::error_code ec; l->acceptor.close(ec);
        if (l->udp_socket) { l->udp_socket->close(ec); l->udp_socket.reset(); }
    }
    listeners_.clear();
    accessor_sessions_.clear();
    pending_sessions_.clear();
    provider_flows_.clear();
}

// --- punch engine helpers (shared pattern) ---

void frp_runtime_unified_client_agent::create_provider_punch_engine(
    const std::shared_ptr<provider_flow_runtime>& flow)
{
    if (!channel_ || !config_.public_server_udp_port) return;
    frp_punch_engine::config cfg;
    cfg.executor              = reconnect_timer_.get_executor();
    cfg.flow_id               = flow->flow_id;
    cfg.uuid                  = uuid_;
    cfg.peer_uuid             = flow->accessor_uuid;
    cfg.traffic_secret         = config_.traffic_secret;
    cfg.public_server_host     = config_.public_server_host;
    cfg.public_server_udp_port = config_.public_server_udp_port;
    cfg.my_nat_type            = static_cast<std::uint8_t>(probed_nat_type_);
    cfg.my_rtt_ms              = startup_rtt_ms_;

    auto engine = frp_punch_engine::create(std::move(cfg),
        [this, weak_ch = std::weak_ptr<frp_runtime_signal_client_channel>(channel_)]
        (std::string json) { if (auto ch = weak_ch.lock()) ch->send_raw_json(json); });

    auto schedule_retry = [this, self = shared_from_this(), flow] {
        if (flow->punch_retry_count >= kMaxPunchRetries) return;
        flow->punch_retry_count++;
        auto delay = std::chrono::seconds(punch_retry_delay(flow->punch_retry_count));
        FINFO("client flow {} punch retry in {}s (attempt {})",
              flow->flow_id, delay.count(), flow->punch_retry_count);
        flow->punch_retry_timer.expires_after(delay);
        flow->punch_retry_timer.async_wait(
            [this, self, flow](const std::error_code& ec) {
                if (ec || !reference_.is_valid() || flow->closed) return;
                create_provider_punch_engine(flow);
                flow->punch_engine->start();
            });
    };

    engine->set_on_success([this, self = shared_from_this(), flow, schedule_retry](frp_punch_engine::punch_result result) {
        if (!reference_.is_valid()) return;
        FINFO("client flow {} punch success local_port={} peer_port={}",
              flow->flow_id, result.local_port, result.peer_port);
        flow->data_channel->accept_p2p(std::move(result.socket), result.peer_endpoint,
                                       result.local_port, result.peer_port);
        flow->punch_engine->release(); flow->punch_engine.reset();
    });
    engine->set_on_p2p_imminent([this, self = shared_from_this(), flow] {
        if (flow->data_channel) flow->data_channel->expect_p2p_disconnect();
    });
    engine->set_on_probe_match([this, self = shared_from_this(), flow](std::uint16_t local_port, std::uint16_t peer_port,
                                                                          std::uint16_t external_local_port, std::uint16_t external_peer_port) {
        if (!reference_.is_valid() || !flow->punch_engine) return;
        if(!flow->punch_engine->is_valid_probe_pair(local_port,external_peer_port))return;
        flow->punch_engine->send_punch_confirm(local_port, peer_port, external_local_port, external_peer_port);
    });
    engine->set_on_failed([this, self = shared_from_this(), flow, schedule_retry] {
        if (!reference_.is_valid()) return;
        flow->punch_engine->release(); flow->punch_engine.reset(); schedule_retry();
    });
    flow->punch_engine = std::move(engine);
}

void frp_runtime_unified_client_agent::create_accessor_punch_engine(
    const std::shared_ptr<accessor_session_context>& session)
{
    if (!channel_ || !config_.public_server_udp_port) return;
    frp_punch_engine::config cfg;
    cfg.executor              = session->local_socket.get_executor();
    cfg.flow_id               = session->flow_id;
    cfg.uuid                  = uuid_;
    cfg.peer_uuid             = session->provider_uuid;
    cfg.traffic_secret         = config_.traffic_secret;
    cfg.public_server_host     = config_.public_server_host;
    cfg.public_server_udp_port = config_.public_server_udp_port;
    cfg.my_nat_type            = static_cast<std::uint8_t>(probed_nat_type_);
    cfg.my_rtt_ms              = startup_rtt_ms_;

    auto engine = frp_punch_engine::create(std::move(cfg),
        [this, weak_ch = std::weak_ptr<frp_runtime_signal_client_channel>(channel_)]
        (std::string json) { if (auto ch = weak_ch.lock()) ch->send_raw_json(json); });

    auto schedule_retry = [this, self = shared_from_this(), session] {
        if (session->punch_retry_count >= kMaxPunchRetries) return;
        session->punch_retry_count++;
        auto delay = std::chrono::seconds(punch_retry_delay(session->punch_retry_count));
        FINFO("client session {} flow {} punch retry in {}s (attempt {})",
              session->session_id, session->flow_id, delay.count(),
              session->punch_retry_count);
        session->punch_retry_timer.expires_after(delay);
        session->punch_retry_timer.async_wait(
            [this, self = shared_from_this(), session](const std::error_code& ec) {
                if (ec || !reference_.is_valid() || session->closed) return;
                create_accessor_punch_engine(session);
                session->punch_engine->start();
            });
    };

    engine->set_on_endpoint_ready(
        [this, self = shared_from_this(), session](std::string ip, std::uint16_t port) {
            if (!reference_.is_valid() || !channel_) return;
            session->pending_punch_seq++;
            frp_runtime_p2p_handshake_data hs;
            hs.command = frp_runtime_p2p_handshake_command;
            hs.flow_id = session->flow_id;
            hs.uuid = session->provider_uuid;
            hs.internal_ip = ""; hs.internal_port = 0;
            hs.external_ip = ip; hs.external_port = port;
            hs.rtt_ms = startup_rtt_ms_;
            hs.nat_type = static_cast<std::uint8_t>(probed_nat_type_);
            hs.punch_seq = session->pending_punch_seq;
            FINFO("client session {} flow {} p2p_handshake external={}:{} seq={}",
                  session->session_id, session->flow_id, ip, port, session->pending_punch_seq);
            channel_->send_command(hs);
            session->ack_timer.expires_after(std::chrono::seconds(30));
            session->ack_timer.async_wait(
                [this, self, session](const std::error_code& ec) {
                    if (ec || !reference_.is_valid() || session->closed) return;
                    FWARN("client session {} flow {} handshake ack timeout",
                          session->session_id, session->flow_id);
                    fail_session(session, "p2p handshake ack timeout");
                });
    });
    engine->set_on_success([this, self = shared_from_this(), session, schedule_retry](frp_punch_engine::punch_result result) {
        if (!reference_.is_valid()) return;
        FINFO("client session {} flow {} punch success local_port={} peer_port={}",
              session->session_id, session->flow_id, result.local_port, result.peer_port);
        session->data_channel->accept_p2p(std::move(result.socket), result.peer_endpoint,
                                          result.local_port, result.peer_port);
        session->punch_engine->release(); session->punch_engine.reset();
    });
    engine->set_on_p2p_imminent([this, self = shared_from_this(), session] {
        if (session->data_channel) session->data_channel->expect_p2p_disconnect();
    });
    engine->set_on_probe_match([this, self = shared_from_this(), session](std::uint16_t local_port, std::uint16_t peer_port,
                                                                            std::uint16_t external_local_port, std::uint16_t external_peer_port) {
        if (!reference_.is_valid() || !session->punch_engine || session->probe_confirm_sent) return;
        if(!session->punch_engine->is_valid_probe_pair(local_port,external_peer_port))return;
        session->probe_confirm_sent = true;
        if (session->data_channel) session->data_channel->expect_p2p_disconnect();
        session->punch_engine->send_punch_confirm(local_port, peer_port, external_local_port, external_peer_port);
    });
    engine->set_on_failed([this, self = shared_from_this(), session, schedule_retry] {
        if (!reference_.is_valid()) return;
        session->punch_engine->release(); session->punch_engine.reset(); schedule_retry();
    });
    session->punch_engine = std::move(engine);
}

// --- signal channel ---

void frp_runtime_unified_client_agent::connect_signal_channel() {
    if (!reference_.is_valid()) return;
    channel_ = frp_runtime_signal_client_channel::make_shared(
        reconnect_timer_.get_executor(), config_.public_server_host,
        std::to_string(config_.public_server_tcp_port));
    channel_->enable_ssl(to_network_config(config_.ssl));
    channel_->set_on_connected([this, self = shared_from_this()] {
        FINFO("client signal connected server={}:{} uuid={}",
              config_.public_server_host, config_.public_server_tcp_port, uuid_);
    });
    channel_->set_on_disconnected([this, self = shared_from_this()] {
        if (!reference_.is_valid()) return;
        FERR("client signal disconnected uuid={}", uuid_);
        for (auto& [_, s] : accessor_sessions_) { if (s->data_channel) s->data_channel->release_obj(); }
        for (auto& [_, s] : pending_sessions_) { if (s->data_channel) s->data_channel->release_obj(); }
        for (auto& [_, f] : provider_flows_) { if (f->data_channel) f->data_channel->release_obj(); }
        for (auto& [_, l] : listeners_) {
            std::error_code ec;
            l->acceptor.close(ec);
            if (l->udp_socket) { l->udp_socket->close(ec); l->udp_socket.reset(); }
        }
        listeners_.clear();
        accessor_sessions_.clear();
        pending_sessions_.clear();
        provider_flows_.clear();
        last_known_services_.clear();
        channel_ = nullptr;
        schedule_reconnect();
    });
    channel_->set_on_command([this, self = shared_from_this()](
        const frp_runtime_command_base& cmd, std::string payload) {
        process_command(cmd, std::move(payload));
    });
    channel_->start();
}

void frp_runtime_unified_client_agent::schedule_reconnect() {
    reconnect_timer_.expires_after(std::chrono::seconds(reconnect_delay_seconds_));
    FINFO("client schedule_reconnect delay={}s", reconnect_delay_seconds_);
    reconnect_timer_.async_wait([this, self = shared_from_this()](const asio::error_code& ec) {
        if (!reference_.is_valid() || ec) return;
        if (reconnect_delay_seconds_ < 32) reconnect_delay_seconds_ *= 2;
        run_time_sync();
    });
}

// --- process_command ---

void frp_runtime_unified_client_agent::process_command(
    const frp_runtime_command_base& command, std::string payload) {
    if (!channel_) return;
    switch (command.command) {
    case frp_runtime_server_hello_command: {
        frp_runtime_server_hello_data hello;
        if (!Fundamental::io::from_json(payload, hello)) { channel_->release_obj(); return; }
        FINFO("client recv server_hello nonce={}", hello.server_nonce);
        frp_runtime_auth_request_data req;
        req.command = frp_runtime_auth_request_command;
        req.digest = frp_hmac_sha256_hex(config_.traffic_secret, hello.server_nonce);
        channel_->send_command(req);
        return;
    }
    case frp_runtime_auth_response_command: {
        frp_runtime_auth_response_data resp;
        if (!Fundamental::io::from_json(payload, resp) || !resp.ok) {
            FERR("client auth failed uuid={}", uuid_);
            channel_->release_obj();
            return;
        }
        FINFO("client recv auth_response ok uuid={}", uuid_);
        register_all_services();
        return;
    }
    case frp_runtime_register_services_resp_command: {
        frp_runtime_register_services_resp_data resp;
        if (!Fundamental::io::from_json(payload, resp)) return;
        if (!resp.ok) { FERR("client register_services failed msg={}", resp.message); return; }
        FINFO("client register_services ok");
        subscribe_all_keys();
        return;
    }
    case frp_runtime_subscribe_services_resp_command: {
        frp_runtime_subscribe_services_resp_data resp;
        if (!Fundamental::io::from_json(payload, resp) || !resp.ok) return;
        {
            std::unordered_set<std::string> current;
            for (const auto& svc : resp.services)
                current.insert(Fundamental::StringFormat("{}@{}", svc.service_name, svc.provider_uuid));
            if (current != last_known_services_) {
                FINFO("client recv subscribe_services_resp services={} (changed)", resp.services.size());
                last_known_services_ = std::move(current);
                reconcile_listeners(resp.services);
            }
        }
        start_polling();
        return;
    }
    case frp_runtime_prepare_flow_command: {
        frp_runtime_prepare_flow_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        handle_prepare_flow(req);
        return;
    }
    case frp_runtime_flow_transport_ready_command: {
        frp_runtime_flow_transport_ready_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        auto it = provider_flows_.find(req.flow_id);
        if (it == provider_flows_.end()) return;
        start_provider_backend_connect(it->second);
        return;
    }
    case frp_runtime_create_flow_response_command: {
        frp_runtime_create_flow_response_data resp;
        if (!Fundamental::io::from_json(payload, resp)) { channel_->release_obj(); return; }
        if (resp.result == frp_runtime_flow_result_rejected) { return; }
        if (pending_sessions_.empty()) return;
        auto it = pending_sessions_.begin(); // FIFO match
        auto session = it->second;
        pending_sessions_.erase(it);
        session->flow_id = resp.flow_id;
        session->provider_uuid = resp.provider_uuid;
        accessor_sessions_[resp.flow_id] = session;
        session->data_channel = frp_proxy_data_channel::make_shared(
            reconnect_timer_.get_executor(), config_.public_server_host,
            std::to_string(config_.public_server_tcp_port), resp.flow_id, uuid_,
            config_.traffic_secret, config_.data_channel_idle_timeout_seconds);
        session->data_channel->enable_ssl(to_network_config(config_.ssl));
        session->data_channel->set_on_disconnected([this, self = shared_from_this(), session] {
            if (!reference_.is_valid() || session->closed) return;
            fail_session(session, "data channel disconnected");
        });
        session->data_channel->set_on_data([this, self = shared_from_this(), session](std::string data) {
            if (!reference_.is_valid() || session->closed) return;
            session->pending_writes.push_back(std::move(data));
            handle_local_write_queue(session);
        });
        session->data_channel->set_on_p2p_upgraded([this, self = shared_from_this(), session] {
            FINFO("client flow {} p2p upgrade complete", session->flow_id);
        });
        session->data_channel->start();

        // Create punch engine if P2P is viable
        if (session->enable_p2p && session->provider_enable_p2p &&
            config_.public_server_udp_port != 0 &&
            probed_nat_type_ != frp_runtime_nat_type_disabled &&
            session->provider_nat_type != frp_runtime_nat_type_disabled) {
            create_accessor_punch_engine(session);
        }
        return;
    }
    case frp_runtime_flow_ready_command: {
        frp_runtime_flow_ready_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        auto it = accessor_sessions_.find(req.flow_id);
        if (it == accessor_sessions_.end()) return;
        auto session = it->second;
        if (session->closed) return;
        session->ready = true;
        if (probed_nat_type_ != frp_runtime_nat_type_disabled &&
            session->provider_nat_type != frp_runtime_nat_type_disabled &&
            config_.public_server_udp_port != 0 &&
            session->enable_p2p && session->provider_enable_p2p &&
            session->punch_engine) {
            session->punch_engine->start();
        }
        start_local_read_loop(session);
        if (session->service_type == frp_runtime_service_udp) {
            while (!session->pending_forwards.empty()) {
                auto& d = session->pending_forwards.front();
                if (session->data_channel) session->data_channel->send_bytes(d.data(), d.size());
                session->pending_forwards.pop_front();
            }
        }
        return;
    }
    case frp_runtime_flow_failed_command: {
        frp_runtime_flow_failed_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        auto it = accessor_sessions_.find(req.flow_id);
        if (it != accessor_sessions_.end()) fail_session(it->second, req.message);
        auto pit = provider_flows_.find(req.flow_id);
        if (pit != provider_flows_.end()) {
            pit->second->closed = true;
            if (pit->second->data_channel) { pit->second->data_channel->release_obj(); pit->second->data_channel = nullptr; }
            provider_flows_.erase(pit);
        }
        return;
    }
    case frp_runtime_flow_closed_command: {
        frp_runtime_flow_closed_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        auto it = accessor_sessions_.find(req.flow_id);
        if (it != accessor_sessions_.end()) {
            if (!it->second->data_channel || !it->second->data_channel->p2p_active_or_expected())
                fail_session(it->second, "peer closed");
            return;
        }
        auto pit = provider_flows_.find(req.flow_id);
        if (pit != provider_flows_.end()) {
            auto& flow = pit->second;
            if (flow->data_channel && flow->data_channel->p2p_active_or_expected()) return;
            FWARN("client flow {} peer closed, cleaning up provider backend", req.flow_id);
            flow->closed = true;
            flow->handshake_timer.cancel();
            if (flow->punch_engine) { flow->punch_engine->release(); flow->punch_engine.reset(); }
            if (flow->data_channel) { flow->data_channel->release_obj(); flow->data_channel = nullptr; }
            std::error_code ec;
            flow->backend_socket.close(ec);
            if (flow->backend_udp_socket) { flow->backend_udp_socket->close(ec); flow->backend_udp_socket.reset(); }
            provider_flows_.erase(pit);
        }
        return;
    }
    case frp_runtime_punch_confirm_command: {
        frp_runtime_punch_confirm_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        // Accessor: provider sends confirm as port-pair hint. Send confirm (once).
        auto it = accessor_sessions_.find(req.flow_id);
        if (it != accessor_sessions_.end() && it->second->punch_engine && !it->second->probe_confirm_sent) {
            if(!it->second->punch_engine->is_valid_probe_pair(req.peer_port,req.external_local_port))return;
            it->second->probe_confirm_sent = true;
            if (it->second->data_channel) it->second->data_channel->expect_p2p_disconnect();
            it->second->punch_engine->send_punch_confirm(
                req.peer_port, req.local_port, req.external_peer_port, req.external_local_port);
            return;
        }
        // Provider: accessor sends confirm to start handshake.
        auto pit = provider_flows_.find(req.flow_id);
        if (pit != provider_flows_.end() && pit->second->punch_engine)
            pit->second->punch_engine->on_punch_confirm(req.peer_port, req.local_port,
                                                         req.external_peer_port, req.external_local_port);
        return;
    }
    case frp_runtime_punch_confirm_ack_command: {
        frp_runtime_punch_confirm_ack_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        auto it = accessor_sessions_.find(req.flow_id);
        if (it != accessor_sessions_.end() && it->second->punch_engine)
            it->second->punch_engine->on_punch_confirm_ack(req.peer_port, req.local_port,
                                                           req.external_peer_port, req.external_local_port);
        return;
    }
    case frp_runtime_punch_confirm_ok_command: {
        frp_runtime_punch_confirm_ok_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        auto pit = provider_flows_.find(req.flow_id);
        if (pit != provider_flows_.end() && pit->second->punch_engine)
            pit->second->punch_engine->on_punch_confirm_ok(req.peer_port, req.local_port,
                                                           req.external_peer_port, req.external_local_port);
        return;
    }
    case frp_runtime_p2p_handshake_command: {
        frp_runtime_p2p_handshake_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        // Provider: accessor's endpoint info arrives
        auto pit = provider_flows_.find(req.flow_id);
        if (pit == provider_flows_.end() || !pit->second->enable_p2p) {
            // Reply with port=0 if P2P not supported
            frp_runtime_p2p_handshake_data ack;
            ack.command = frp_runtime_p2p_handshake_ack_command;
            ack.flow_id = req.flow_id; ack.punch_seq = req.punch_seq;
            if (channel_) channel_->send_command(ack);
            return;
        }
        auto& flow = pit->second;
        if (flow->closed) return;
        if (flow->punch_engine) return; // already handling a handshake
        flow->expected_punch_seq = req.punch_seq;
        create_provider_punch_engine(flow);
        // Store accessor's info as peer info
        flow->punch_engine->on_peer_info(
            frp_punch_engine::peer_info{req.external_ip, req.external_port,
                req.nat_type, req.rtt_ms});
        flow->punch_engine->set_on_endpoint_ready(
            [this, self = shared_from_this(), flow](std::string ip, std::uint16_t port) {
                if (!reference_.is_valid() || !channel_) return;
                frp_runtime_p2p_handshake_data ack;
                ack.command = frp_runtime_p2p_handshake_ack_command;
                ack.flow_id = flow->flow_id;
                ack.uuid = flow->accessor_uuid;
                ack.internal_ip = ""; ack.internal_port = 0;
                ack.external_ip = ip; ack.external_port = port;
                ack.rtt_ms = startup_rtt_ms_;
                ack.nat_type = static_cast<std::uint8_t>(probed_nat_type_);
                ack.punch_seq = flow->expected_punch_seq;
                FINFO("client flow {} p2p_handshake ack external={}:{} seq={}",
                      flow->flow_id, ip, port, flow->expected_punch_seq);
                channel_->send_command(ack);
            });
        flow->punch_engine->set_on_failed([this, self = shared_from_this(), flow] {
            // Probe failed: reply port=0
            frp_runtime_p2p_handshake_data ack;
            ack.command = frp_runtime_p2p_handshake_ack_command;
            ack.flow_id = flow->flow_id; ack.punch_seq = flow->expected_punch_seq;
            if (channel_) channel_->send_command(ack);
        });
        flow->punch_engine->start();
        return;
    }
    case frp_runtime_p2p_handshake_ack_command: {
        frp_runtime_p2p_handshake_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        // Accessor: provider's endpoint info arrives
        auto it = accessor_sessions_.find(req.flow_id);
        if (it == accessor_sessions_.end()) return;
        auto& session = it->second;
        if (req.punch_seq != session->pending_punch_seq) return; // stale
        session->ack_timer.cancel();
        if (req.external_port == 0) return; // provider probe failed, wait retry
        session->provider_startup_rtt_ms = req.rtt_ms;
        session->provider_nat_type = req.nat_type;
        if (session->punch_engine) {
            session->punch_engine->on_peer_info(
                frp_punch_engine::peer_info{req.external_ip, req.external_port,
                    req.nat_type, req.rtt_ms});
        }
        // Calculate deadline and trigger punch
        std::int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::int64_t deadline_us = now_us + server_clock_offset_us_ +
            (startup_rtt_ms_ + req.rtt_ms) * 3 * 1000;
        frp_runtime_punch_start_data ps;
        ps.command = frp_runtime_punch_start_command;
        ps.flow_id = req.flow_id; ps.uuid = session->provider_uuid; ps.deadline_us = deadline_us;
        if (channel_) channel_->send_command(ps);
        if (session->punch_engine)
            session->punch_engine->start_punch_at(deadline_us - server_clock_offset_us_);
        return;
    }
    case frp_runtime_punch_start_command: {
        frp_runtime_punch_start_data req;
        if (!Fundamental::io::from_json(payload, req)) return;
        std::int64_t local_deadline_us = req.deadline_us - server_clock_offset_us_;
        FINFO("client punch_start flow_id={} server_deadline={}us local_deadline={}us",
              req.flow_id, req.deadline_us, local_deadline_us);
        auto pit = provider_flows_.find(req.flow_id);
        if (pit != provider_flows_.end() && pit->second->punch_engine)
            pit->second->punch_engine->start_punch_at(local_deadline_us);
        return;
    }
    default: break;
    }
}

// --- provider side ---

void frp_runtime_unified_client_agent::register_all_services() {
    frp_runtime_register_services_data req;
    req.command = frp_runtime_register_services_command;
    req.uuid = uuid_;
    req.nat_type = (config_.nat_type != frp_runtime_nat_type_disabled &&
                    probed_nat_type_ != frp_runtime_nat_type_disabled)
                       ? probed_nat_type_ : frp_runtime_nat_type_disabled;
    req.startup_rtt_ms = startup_rtt_ms_;
    for (const auto& group : config_.groups) {
        if (group.services.empty() && group.listeners.empty()) continue;
        frp_runtime_service_group g;
        g.register_key = group.register_key;
        for (const auto& svc : group.services) {
            frp_runtime_service_registration_data d;
            d.service_name = svc.service_name; d.service_type = svc.service_type; d.enable_p2p = svc.enable_p2p;
            g.services.push_back(std::move(d));
        }
        req.groups.push_back(std::move(g));
    }
    channel_->send_command(req);
}

void frp_runtime_unified_client_agent::handle_prepare_flow(const frp_runtime_prepare_flow_data& request) {
    auto flow = std::make_shared<provider_flow_runtime>(reconnect_timer_.get_executor());
    flow->flow_id = request.flow_id;
    flow->service_name = request.service_name;
    flow->accessor_uuid = request.accessor_uuid;
    flow->transport = request.transport;
    auto it = services_by_name_.find(request.service_name);
    if (it == services_by_name_.end()) {
        frp_runtime_flow_failed_data failed;
        failed.command = frp_runtime_flow_failed_command;
        failed.flow_id = request.flow_id;
        failed.reason = frp_runtime_flow_failed_relay_channel_open_failed;
        failed.message = "service not found";
        channel_->send_command(failed);
        return;
    }
    const auto& svc = it->second;
    flow->enable_p2p = svc.enable_p2p;
    flow->data_channel = frp_proxy_data_channel::make_shared(
        reconnect_timer_.get_executor(), config_.public_server_host,
        std::to_string(config_.public_server_tcp_port), flow->flow_id, uuid_,
        config_.traffic_secret, config_.data_channel_idle_timeout_seconds);
    flow->data_channel->enable_ssl(to_network_config(config_.ssl));
    flow->data_channel->set_on_disconnected([this, self = shared_from_this(), flow] {
        if (!reference_.is_valid() || flow->closed) return;
        flow->closed = true;
        flow->handshake_timer.cancel();
        frp_runtime_flow_failed_data f;
        f.command = frp_runtime_flow_failed_command;
        f.flow_id = flow->flow_id;
        f.reason = frp_runtime_flow_failed_relay_channel_open_failed;
        f.message = "provider data channel disconnected";
        if (channel_) channel_->send_command(f);
        if (flow->data_channel) { flow->data_channel->release_obj(); flow->data_channel = nullptr; }
        std::error_code ec;
        flow->backend_socket.close(ec);
        if (flow->backend_udp_socket) { flow->backend_udp_socket->close(ec); flow->backend_udp_socket.reset(); }
        provider_flows_.erase(flow->flow_id);
    });
    flow->data_channel->set_on_data([this, self = shared_from_this(), flow](std::string data) {
        if (!reference_.is_valid() || flow->closed) return;
        flow->pending_writes.push_back(std::move(data));
        handle_backend_write_queue(flow);
    });
    flow->data_channel->set_on_p2p_upgraded([this, self = shared_from_this(), flow] {
        FINFO("client flow {} p2p upgrade complete", flow->flow_id);
    });
    provider_flows_[flow->flow_id] = flow;
    flow->data_channel->start();

    // Punch engine created later when p2p_handshake arrives from accessor
}

// --- accessor side ---

void frp_runtime_unified_client_agent::subscribe_all_keys() {
    frp_runtime_subscribe_services_data req;
    req.command = frp_runtime_subscribe_services_command;
    for (const auto& group : config_.groups) {
        if (group.listeners.empty()) continue;
        req.register_keys.push_back(group.register_key);
    }
    if (!req.register_keys.empty()) channel_->send_command(req);
}

void frp_runtime_unified_client_agent::reconcile_listeners(
    const std::vector<frp_runtime_visible_service_data>& services) {
    std::unordered_map<std::string, frp_runtime_visible_service_data> services_by_name;
    for (const auto& service : services) {
        if (uuid_==service.provider_uuid) continue;
        auto key = Fundamental::StringFormat("{}:{}", service.service_name, static_cast<int>(service.service_type));
        services_by_name[key] = service;
    }
    std::unordered_set<std::string> desired_keys;
    for (const auto& group : config_.groups) {
        for (const auto& listener_config : group.listeners) {
            auto lookup_key = Fundamental::StringFormat("{}:{}", listener_config.service_name,
                                                         static_cast<int>(listener_config.service_type));
            auto service_it = services_by_name.find(lookup_key);
            if (service_it == services_by_name.end()) {
                FWARN("client listener service_name={} service_type={} not found in service directory",
                      listener_config.service_name, static_cast<int>(listener_config.service_type));
                continue;
            }
            const auto key = Fundamental::StringFormat("{}:{}:{}", listener_config.service_name,
                                                        listener_config.listen_host, listener_config.listen_port);
            desired_keys.insert(key);
            if (listeners_.count(key) > 0) continue;

            const auto& svc = service_it->second;
            auto listener = std::make_shared<listener_runtime>(
                reconnect_timer_.get_executor(), listener_config.service_name,
                listener_config.listen_host, listener_config.listen_port);
            listener->service_type = listener_config.service_type;
            listener->enable_p2p = listener_config.enable_p2p;
            listener->provider_enable_p2p = svc.enable_p2p;
            listener->provider_nat_type = svc.provider_nat_type;
            listener->provider_startup_rtt_ms = svc.provider_startup_rtt_ms;
            listener->register_key = group.register_key;

            std::error_code ec;
            auto address = asio::ip::make_address(listener_config.listen_host, ec);
            if (ec) continue;

            if (listener_config.service_type == frp_runtime_service_udp) {
                listener->udp_socket = std::make_unique<asio::ip::udp::socket>(reconnect_timer_.get_executor());
                listener->udp_socket->open(address.is_v6() ? asio::ip::udp::v6() : asio::ip::udp::v4(), ec);
                if (ec) continue;
                listener->udp_socket->set_option(asio::ip::udp::socket::reuse_address(true), ec);
                listener->udp_socket->bind(asio::ip::udp::endpoint(address, listener_config.listen_port), ec);
                if (ec) { FERR("client bind udp listener failed service={} err={}", listener_config.service_name, ec.message()); continue; }
                FWARN("client udp listener active service_name={} endpoint={}:{}",
                      listener->service_name, listener->listen_host, listener->listen_port);
                start_udp_receive_loop(listener);
            } else {
                listener->acceptor.open(address.is_v6() ? asio::ip::tcp::v6() : asio::ip::tcp::v4(), ec);
                if (ec) continue;
                listener->acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
                listener->acceptor.bind(asio::ip::tcp::endpoint(address, listener_config.listen_port), ec);
                if (ec) { FERR("client bind tcp listener failed service={} err={}", listener_config.service_name, ec.message()); continue; }
                listener->acceptor.listen(asio::socket_base::max_listen_connections, ec);
                if (ec) continue;
                FWARN("client tcp listener active service_name={} endpoint={}:{}",
                      listener->service_name, listener->listen_host, listener->listen_port);
                start_accept_loop(listener);
            }
            listeners_[key] = std::move(listener);
        }
    }
    // Remove stale listeners
    for (auto it = listeners_.begin(); it != listeners_.end(); ) {
        if (desired_keys.count(it->first) == 0) {
            std::error_code ec; it->second->acceptor.close(ec);
            if (it->second->udp_socket) { it->second->udp_socket->close(ec); it->second->udp_socket.reset(); }
            it = listeners_.erase(it);
        } else { ++it; }
    }
}

void frp_runtime_unified_client_agent::start_accept_loop(const std::shared_ptr<listener_runtime>& listener) {
    listener->acceptor.async_accept([this, self = shared_from_this(), listener](
        std::error_code ec, asio::ip::tcp::socket socket) {
        if (!reference_.is_valid()) return;
        if (!ec) {
            auto session = std::make_shared<accessor_session_context>(next_session_id_++, std::move(socket));
            session->service_name = listener->service_name;
            session->service_type = listener->service_type;
            session->enable_p2p = listener->enable_p2p;
            session->provider_enable_p2p = listener->provider_enable_p2p;
            session->provider_nat_type = listener->provider_nat_type;
            session->provider_startup_rtt_ms = listener->provider_startup_rtt_ms;
            session->register_key = listener->register_key;
            pending_sessions_[session->session_id] = session;
            request_flow(session);
        }
        start_accept_loop(listener);
    });
}

void frp_runtime_unified_client_agent::start_udp_receive_loop(const std::shared_ptr<listener_runtime>& listener) {
    if (!listener->udp_socket) return;
    listener->udp_socket->async_receive_from(
        asio::buffer(listener->udp_recv_buf), listener->udp_recv_endpoint_,
        [this, self = shared_from_this(), listener](std::error_code ec, std::size_t bytes_read) {
            if (!reference_.is_valid() || !listener->udp_socket) return;
            if (ec) { start_udp_receive_loop(listener); return; }
            auto& ep = listener->udp_recv_endpoint_;
            auto session_it = listener->udp_sessions.find(ep);
            std::shared_ptr<accessor_session_context> session;
            if (session_it != listener->udp_sessions.end()) {
                session = session_it->second.lock();
                if (!session) listener->udp_sessions.erase(session_it);
            }
            if (!session) {
                session = std::make_shared<accessor_session_context>(next_session_id_++, ep, reconnect_timer_.get_executor());
                session->service_name = listener->service_name;
                session->service_type = frp_runtime_service_udp;
                session->enable_p2p = listener->enable_p2p;
                session->provider_enable_p2p = listener->provider_enable_p2p;
                session->provider_nat_type = listener->provider_nat_type;
                session->provider_startup_rtt_ms = listener->provider_startup_rtt_ms;
                session->register_key = listener->register_key;
                session->udp_socket = listener->udp_socket.get();
                session->on_destroy = [wl = std::weak_ptr<listener_runtime>(listener), ep]() {
                    if (auto l = wl.lock()) l->udp_sessions.erase(ep);
                };
                listener->udp_sessions[ep] = session;
                pending_sessions_[session->session_id] = session;
                request_flow(session);
            }
            if (session && bytes_read > 0) {
                if (session->data_channel && session->ready)
                    session->data_channel->send_bytes(listener->udp_recv_buf.data(), bytes_read);
                else
                    session->pending_forwards.emplace_back(listener->udp_recv_buf.data(), bytes_read);
            }
            start_udp_receive_loop(listener);
        });
}

void frp_runtime_unified_client_agent::request_flow(const std::shared_ptr<accessor_session_context>& session) {
    if (!channel_) return;
    frp_runtime_create_flow_request_data req;
    req.command = frp_runtime_create_flow_request_command;
    req.service_name = session->service_name;
    req.register_key = session->register_key;
    req.transport = session->service_type == frp_runtime_service_udp
                        ? frp_runtime_transport_udp_relay : frp_runtime_transport_tcp_relay;
    channel_->send_command(req);
}

void frp_runtime_unified_client_agent::start_local_read_loop(const std::shared_ptr<accessor_session_context>& session) {
    if (!session || !session->ready || !channel_) return;
    if (session->service_type == frp_runtime_service_udp) return;
    session->local_socket.async_read_some(
        asio::buffer(session->read_buf.data(), session->read_buf.size()),
        [this, self = shared_from_this(), session](const asio::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (ec) {
                if (session->closed) return;
                if (channel_) {
                    frp_runtime_flow_closed_data c; c.command = frp_runtime_flow_closed_command;
                    c.flow_id = session->flow_id; channel_->send_command(c);
                }
                if (session->data_channel && session->data_channel->p2p_active_or_expected()) {
                    session->peer_closed = true;
                    std::error_code ign;
                    session->local_socket.close(ign);
                    return;
                }
                fail_session(session, ec.message());
                return;
            }
            if (session->data_channel)
                session->data_channel->send_bytes(session->read_buf.data(), bytes_read);
            if (!session->peer_closed) start_local_read_loop(session);
        });
}

void frp_runtime_unified_client_agent::handle_local_write_queue(const std::shared_ptr<accessor_session_context>& session) {
    if (!session || session->writing || session->pending_writes.empty()) return;
    session->writing = true;
    auto& current = session->pending_writes.front();
    if (session->service_type == frp_runtime_service_udp && session->udp_socket) {
        auto data = std::make_shared<std::string>(std::move(current));
        session->udp_socket->async_send_to(asio::buffer(*data), session->remote_endpoint,
            [this, self = shared_from_this(), session, data](const asio::error_code&, std::size_t) {
                session->writing = false;
                if (!reference_.is_valid()) return;
                session->pending_writes.pop_front();
                handle_local_write_queue(session);
            });
        return;
    }
    asio::async_write(session->local_socket, asio::buffer(current.data(), current.size()),
        [this, self = shared_from_this(), session](const asio::error_code& ec, std::size_t) {
            session->writing = false;
            if (!reference_.is_valid()) return;
            if (ec) { fail_session(session, ec.message()); return; }
            session->pending_writes.pop_front();
            handle_local_write_queue(session);
        });
}

void frp_runtime_unified_client_agent::fail_session(const std::shared_ptr<accessor_session_context>& session,
                                                     const std::string& reason) {
    if (!session || session->closed) return;
    session->closed = true;
    session->ack_timer.cancel();
    FWARN("client session flow_id={} service={} closed reason={}", session->flow_id, session->service_name, reason);
    if (session->data_channel) {
        session->data_channel->release_obj();
        session->data_channel = nullptr;
    }
    std::error_code ec;
    session->local_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    session->local_socket.close(ec);
    accessor_sessions_.erase(session->flow_id);
    for (auto it = pending_sessions_.begin(); it != pending_sessions_.end(); ++it) {
        if (it->second == session) { pending_sessions_.erase(it); break; }
    }
}

// --- provider backend ---

void frp_runtime_unified_client_agent::start_provider_backend_connect(
    const std::shared_ptr<provider_flow_runtime>& flow) {
    auto it = services_by_name_.find(flow->service_name);
    if (it == services_by_name_.end()) {
        frp_runtime_flow_failed_data f; f.command = frp_runtime_flow_failed_command;
        f.flow_id = flow->flow_id; f.reason = frp_runtime_flow_failed_backend_connect_failed;
        f.message = "service not found"; channel_->send_command(f);
        provider_flows_.erase(flow->flow_id); return;
    }
    const auto& svc = it->second;
    if (flow->transport == frp_runtime_transport_udp_relay) {
        auto resolver = std::make_shared<asio::ip::udp::resolver>(reconnect_timer_.get_executor());
        auto self = shared_from_this();
        resolver->async_resolve(svc.target_host, std::to_string(svc.target_port),
            [this, self, flow, resolver](const std::error_code& ec, const asio::ip::udp::resolver::results_type& eps) {
                if (!reference_.is_valid() || flow->closed) return;
                if (ec || eps.empty()) {
                    flow->closed = true;
                    frp_runtime_flow_failed_data f; f.command = frp_runtime_flow_failed_command;
                    f.flow_id = flow->flow_id; f.reason = frp_runtime_flow_failed_backend_connect_failed;
                    f.message = ec.message(); channel_->send_command(f); provider_flows_.erase(flow->flow_id); return;
                }
                flow->backend_udp_socket = std::make_unique<asio::ip::udp::socket>(reconnect_timer_.get_executor());
                std::error_code oec;
                flow->backend_udp_socket->open(asio::ip::udp::v4(), oec);
                if (!oec) flow->backend_udp_socket->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0), oec);
                if (oec) {
                    flow->closed = true;
                    frp_runtime_flow_failed_data f; f.command = frp_runtime_flow_failed_command;
                    f.flow_id = flow->flow_id; f.reason = frp_runtime_flow_failed_backend_connect_failed;
                    f.message = oec.message(); channel_->send_command(f); provider_flows_.erase(flow->flow_id); return;
                }
                flow->backend_udp_target = *eps.begin();
                flow->backend_connected = true;
                frp_runtime_flow_ready_data r; r.command = frp_runtime_flow_ready_command;
                r.flow_id = flow->flow_id; channel_->send_command(r);
                handle_backend_write_queue(flow);
                start_backend_read_loop(flow);
                if (probed_nat_type_ != frp_runtime_nat_type_disabled && config_.public_server_udp_port != 0 &&
                    flow->data_channel && flow->enable_p2p) {
                    if (flow->enable_p2p && flow->punch_engine) {
                        flow->punch_engine->start();
                    }
                }
            });
        return;
    }
    // TCP backend
    flow->resolver.async_resolve(svc.target_host, std::to_string(svc.target_port),
        [this, self = shared_from_this(), flow](const std::error_code& ec, const asio::ip::tcp::resolver::results_type& eps) {
            if (!reference_.is_valid() || flow->closed) return;
            if (ec || eps.empty()) {
                flow->closed = true;
                frp_runtime_flow_failed_data f; f.command = frp_runtime_flow_failed_command;
                f.flow_id = flow->flow_id; f.reason = frp_runtime_flow_failed_backend_connect_failed;
                f.message = ec.message(); channel_->send_command(f); provider_flows_.erase(flow->flow_id); return;
            }
            asio::async_connect(flow->backend_socket, eps,
                [this, self, flow](const std::error_code& ec2, const asio::ip::tcp::endpoint&) {
                    if (!reference_.is_valid() || flow->closed) return;
                    if (ec2) {
                        flow->closed = true;
                        frp_runtime_flow_failed_data f; f.command = frp_runtime_flow_failed_command;
                        f.flow_id = flow->flow_id; f.reason = frp_runtime_flow_failed_backend_connect_failed;
                        f.message = ec2.message(); channel_->send_command(f); provider_flows_.erase(flow->flow_id); return;
                    }
                    flow->backend_connected = true;
                    frp_runtime_flow_ready_data r; r.command = frp_runtime_flow_ready_command;
                    r.flow_id = flow->flow_id; channel_->send_command(r);
                    handle_backend_write_queue(flow);
                    start_backend_read_loop(flow);
                    if (probed_nat_type_ != frp_runtime_nat_type_disabled && config_.public_server_udp_port != 0 &&
                        flow->data_channel && flow->enable_p2p) {
                        if (flow->enable_p2p && flow->punch_engine) {
                            flow->punch_engine->start();
                        }
                    }
                });
        });
}

void frp_runtime_unified_client_agent::start_backend_read_loop(const std::shared_ptr<provider_flow_runtime>& flow) {
    if (!flow || !flow->backend_connected || !flow->data_channel) return;
    if (flow->transport == frp_runtime_transport_udp_relay && flow->backend_udp_socket) {
        flow->backend_udp_socket->async_receive_from(
            asio::buffer(flow->udp_recv_buf), flow->backend_udp_target,
            [this, self = shared_from_this(), flow](const std::error_code& ec, std::size_t bytes_read) {
                if (!reference_.is_valid()) return;
                if (ec) { if (!flow->closed) start_backend_read_loop(flow); return; }
                if (bytes_read > 0 && flow->data_channel)
                    flow->data_channel->send_bytes(flow->udp_recv_buf.data(), bytes_read);
                if (!flow->closed) start_backend_read_loop(flow);
            });
        return;
    }
    flow->backend_socket.async_read_some(
        asio::buffer(flow->read_buf.data(), flow->read_buf.size()),
        [this, self = shared_from_this(), flow](const std::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (ec) { if (!flow->closed) start_backend_read_loop(flow); return; }
            if (bytes_read > 0 && flow->data_channel)
                flow->data_channel->send_bytes(flow->read_buf.data(), bytes_read);
            if (!flow->closed) start_backend_read_loop(flow);
        });
}

void frp_runtime_unified_client_agent::handle_backend_write_queue(const std::shared_ptr<provider_flow_runtime>& flow) {
    if (!flow || !flow->backend_connected || flow->writing || flow->pending_writes.empty()) return;
    flow->writing = true;
    auto& current = flow->pending_writes.front();
    if (flow->transport == frp_runtime_transport_udp_relay && flow->backend_udp_socket) {
        auto data = std::make_shared<std::string>(std::move(current));
        flow->backend_udp_socket->async_send_to(asio::buffer(*data), flow->backend_udp_target,
            [this, self = shared_from_this(), flow, data](const asio::error_code& ec, std::size_t) {
                flow->writing = false;
                if (!reference_.is_valid()) return;
                if (ec) {
                    flow->closed = true;
                    frp_runtime_flow_failed_data f; f.command = frp_runtime_flow_failed_command;
                    f.flow_id = flow->flow_id; f.reason = frp_runtime_flow_failed_backend_connect_failed;
                    f.message = ec.message();
                    if (flow->data_channel) { flow->data_channel->release_obj(); flow->data_channel = nullptr; }
                    channel_->send_command(f); provider_flows_.erase(flow->flow_id);
                    return;
                }
                flow->pending_writes.pop_front();
                handle_backend_write_queue(flow);
            });
        return;
    }
    asio::async_write(flow->backend_socket, asio::buffer(current.data(), current.size()),
        [this, self = shared_from_this(), flow](const asio::error_code& ec, std::size_t) {
            flow->writing = false;
            if (!reference_.is_valid()) return;
            if (ec) {
                flow->closed = true;
                frp_runtime_flow_failed_data f; f.command = frp_runtime_flow_failed_command;
                f.flow_id = flow->flow_id; f.reason = frp_runtime_flow_failed_backend_connect_failed;
                f.message = ec.message();
                if (flow->data_channel) { flow->data_channel->release_obj(); flow->data_channel = nullptr; }
                channel_->send_command(f); provider_flows_.erase(flow->flow_id);
                return;
            }
            flow->pending_writes.pop_front();
            handle_backend_write_queue(flow);
        });
}

// --- polling ---

void frp_runtime_unified_client_agent::start_polling() {
    if (!reference_.is_valid()) return;
    poll_timer_.expires_after(std::chrono::seconds(30));
    poll_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {
        if (ec || !reference_.is_valid()) return;
        do_poll();
    });
}

void frp_runtime_unified_client_agent::do_poll() {
    if (!channel_) return;
    subscribe_all_keys();
}

} // namespace network::proxy
