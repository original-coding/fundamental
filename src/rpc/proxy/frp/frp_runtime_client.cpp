#include "frp_runtime_client.hpp"

#include "frp_runtime_common.hpp"
#include "frp_udp_crypto.hpp"
#include "fundamental/basic/base64_utils.hpp"
#include "network/rudp/kcp_imp/ikcp.h"

#include <algorithm>
#include <random>
#include <set>

namespace network::proxy
{

namespace
{
struct kcp_releaser {
    void operator()(ikcpcb* ptr) const {
        if (ptr) ikcp_release(ptr);
    }
};

inline std::uint32_t frp_runtime_kcp_clock() {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count() &
        0xffffffffu);
}

struct frp_runtime_udp_send_context {
    std::unique_ptr<asio::ip::udp::socket>* socket = nullptr;
    asio::ip::udp::endpoint* peer_endpoint         = nullptr;
};

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

template <typename EndpointT>
std::string format_runtime_endpoint(const EndpointT& endpoint) {
    return Fundamental::StringFormat("{}:{}", endpoint.address().to_string(), endpoint.port());
}

inline void log_provider_channel_established(std::uint32_t flow_id,
                                             std::string_view service_name,
                                             std::string_view transport,
                                             std::string_view local_endpoint,
                                             std::string_view remote_endpoint) {
    FINFO("provider channel established transport={} flow_id={} service_name={} local={} remote={}",
          transport, flow_id, service_name, local_endpoint, remote_endpoint);
}

inline void log_accessor_channel_established(std::uint64_t session_id,
                                             std::uint32_t flow_id,
                                             std::string_view service_name,
                                             std::string_view transport,
                                             std::string_view local_endpoint,
                                             std::string_view remote_endpoint) {
    FINFO("accessor channel established transport={} flow_id={} service_name={} local={} remote={}",
          transport, flow_id, service_name, local_endpoint, remote_endpoint);
    (void)session_id;
}

static std::int32_t frp_runtime_kcp_output(const char* buf, int len, ikcpcb*, void* user) {
    auto* context = static_cast<frp_runtime_udp_send_context*>(user);
    if (!context || !context->socket || !context->peer_endpoint || !(*context->socket) ||
        context->peer_endpoint->port() == 0) {
        return 0;
    }

    // KCP header must be plaintext - send raw KCP packet directly
    auto payload = std::make_shared<std::string>(buf, len);
    FINFO("frp_runtime_kcp_output len={} to={}:{}", len,
          context->peer_endpoint->address().to_string(), context->peer_endpoint->port());
    (*context->socket)
        ->async_send_to(asio::buffer(*payload), *context->peer_endpoint, [payload](const std::error_code&, std::size_t) {});
    return 0;
}

template <typename FlowT>
void reset_runtime_p2p_state(FlowT& flow) {
    flow.p2p_timer.cancel();
    flow.kcp_update_timer.cancel();
    flow.endpoint_probe_timer.cancel();
    flow.punch_timer.cancel();
    flow.kcp.reset();
    for (auto& sock : flow.punch_sockets) {
        if (sock) {
            std::error_code ec;
            sock->close(ec);
        }
    }
    flow.punch_sockets.clear();
    flow.punch_active = false;
    if (flow.p2p_socket) {
        std::error_code ec;
        flow.p2p_socket->close(ec);
        flow.p2p_socket.reset();
    }
    flow.p2p_peer_endpoint = {};
    flow.p2p_recv_endpoint = {};
}
} // namespace

struct frp_runtime_provider_agent::provider_flow_runtime {
    std::uint32_t flow_id = 0;
    std::string service_name;
    std::string accessor_uuid;
    std::shared_ptr<frp_runtime_data_client_channel> data_channel;
    std::unique_ptr<asio::ip::udp::socket> p2p_socket;
    asio::ip::udp::endpoint p2p_recv_endpoint;
    asio::ip::udp::endpoint p2p_peer_endpoint;
    asio::steady_timer kcp_update_timer;
    std::unique_ptr<ikcpcb, kcp_releaser> kcp;
    frp_runtime_udp_send_context p2p_send_context;
    asio::steady_timer p2p_timer;
    asio::steady_timer endpoint_probe_timer;
    asio::steady_timer punch_timer;
    asio::ip::tcp::socket backend_socket;
    asio::ip::tcp::resolver resolver;
    std::array<char, 16 * 1024> read_buf {};
    std::array<char, 16 * 1024> p2p_read_buf {};
    std::deque<std::string> pending_writes;
    std::vector<std::uint8_t> udp_send_key;
    std::vector<std::uint8_t> udp_recv_key;
    // punch state
    std::vector<std::unique_ptr<asio::ip::udp::socket>> punch_sockets; // symmetric: 32 sockets; full: 1 socket
    std::uint8_t peer_nat_type = frp_runtime_nat_type_disabled;
    int punch_round = 0;
    std::set<std::uint16_t> punch_scanned_ports; // full side: already-scanned ports
    std::size_t endpoint_probe_attempts = 0;
    bool awaiting_endpoint_ready = false;
    bool punch_active = false;
    bool p2p_success = false;
    bool writing = false;
    bool transport_ready = false;
    bool backend_connected = false;
    bool backend_disconnected = false;
    bool closed = false;

    explicit provider_flow_runtime(const asio::any_io_executor& executor) :
    kcp_update_timer(executor), p2p_timer(executor), endpoint_probe_timer(executor),
    punch_timer(executor), backend_socket(executor), resolver(executor) {
    }
};

struct frp_runtime_accessor_agent::accessor_session_context {
    std::uint64_t session_id = 0;
    std::uint32_t flow_id = 0;
    std::string service_name;
    std::uint8_t nat_type = frp_runtime_nat_type_full;
    bool awaiting_p2p = false;
    bool relay_retry_used = false;
    asio::ip::tcp::socket local_socket;
    std::shared_ptr<frp_runtime_data_client_channel> data_channel;
    std::unique_ptr<asio::ip::udp::socket> p2p_socket;
    asio::ip::udp::endpoint p2p_recv_endpoint;
    asio::ip::udp::endpoint p2p_peer_endpoint;
    asio::steady_timer kcp_update_timer;
    std::unique_ptr<ikcpcb, kcp_releaser> kcp;
    frp_runtime_udp_send_context p2p_send_context;
    asio::steady_timer p2p_timer;
    asio::steady_timer endpoint_probe_timer;
    asio::steady_timer punch_timer;
    std::array<char, 16 * 1024> read_buf {};
    std::array<char, 16 * 1024> p2p_read_buf {};
    std::deque<std::string> pending_writes;
    std::vector<std::uint8_t> udp_send_key;
    std::vector<std::uint8_t> udp_recv_key;
    // punch state
    std::vector<std::unique_ptr<asio::ip::udp::socket>> punch_sockets;
    std::uint8_t peer_nat_type = frp_runtime_nat_type_disabled;
    int punch_round = 0;
    std::set<std::uint16_t> punch_scanned_ports;
    std::size_t endpoint_probe_attempts = 0;
    bool awaiting_endpoint_ready = false;
    bool punch_active = false;
    bool p2p_success = false;
    bool writing = false;
    bool ready = false;
    bool peer_closed = false;
    bool closed = false;

    explicit accessor_session_context(std::uint64_t session_id, asio::ip::tcp::socket&& socket) :
    session_id(session_id),
    local_socket(std::move(socket)),
    kcp_update_timer(local_socket.get_executor()),
    p2p_timer(local_socket.get_executor()),
    endpoint_probe_timer(local_socket.get_executor()),
    punch_timer(local_socket.get_executor()) {
    }
};

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
            auto open            = packet_frp_runtime_command_data(open_request);
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
                       std::function<void(bool)> on_done) {
    if (udp_ports.empty()) {
        on_done(false);
        return;
    }

    struct probe_state {
        std::unique_ptr<asio::ip::udp::socket> socket;
        asio::steady_timer timer;
        std::vector<std::uint8_t> send_key;
        std::vector<std::uint8_t> recv_key;
        std::array<char, 2048> recv_buf {};
        asio::ip::udp::endpoint recv_endpoint;
        std::string result1;
        std::string result2;
        std::size_t port_index = 0;
        std::size_t attempts = 0;
        bool done = false;
        std::function<void(bool)> on_done;
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
    state->send_key = frp_derive_traffic_key(traffic_secret);
    state->recv_key = frp_derive_traffic_key(traffic_secret);

    std::error_code ec;
    state->socket->open(asio::ip::udp::v4(), ec);
    if (ec) {
        FINFO("startup_probe socket open failed err={}", ec.message());
        state->on_done(false);
        return;
    }
    state->socket->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0), ec);
    if (ec) {
        FINFO("startup_probe socket bind failed err={}", ec.message());
        state->on_done(false);
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
                    auto plaintext = frp_udp_decrypt(state->recv_key, encrypted);
                    if (plaintext) {
                        std::string payload(plaintext->begin(), plaintext->end());
                        FINFO("startup_probe decrypted payload_size={} payload={}", plaintext->size(), payload);
                        frp_runtime_flow_endpoint_ready_data echo;
                        if (Fundamental::io::from_json(payload, echo) && echo.flow_id == 0 && !echo.external_ip.empty()) {
                            state->timer.cancel();
                            auto result = Fundamental::StringFormat("{}:{}", echo.external_ip, echo.external_port);
                            FINFO("startup_probe port_index={} received echo external={}", state->port_index, result);
                            if (state->port_index == 0) {
                                state->result1 = result;
                            } else {
                                state->result2 = result;
                            }
                            state->port_index++;
                            state->attempts = 0;
                            if (state->port_index >= state->udp_ports.size()) {
                                state->done = true;
                                bool ok = !state->result1.empty() &&
                                          (state->udp_ports.size() == 1 || state->result1 == state->result2);
                                FINFO("startup_probe probe_result_1={} probe_result_2={} p2p_probe_result={}",
                                      state->result1, state->result2, ok ? "succeeded" : "failed");
                                state->on_done(ok);
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
            FINFO("startup_probe probe_result_1={} probe_result_2={} p2p_probe_result=failed (timeout)",
                  state->result1, state->result2);
            state->on_done(false);
            return;
        }
        auto server_endpoint = resolve_udp_endpoint(state->executor, state->public_server_host,
                                                    state->udp_ports[state->port_index]);
        if (!server_endpoint) {
            state->done = true;
            state->on_done(false);
            return;
        }
        std::error_code local_ec;
        auto local_port = state->socket->local_endpoint(local_ec).port();
        frp_runtime_p2p_probe_data probe;
        probe.command    = frp_runtime_p2p_probe_command;
        probe.flow_id    = 0;
        probe.uuid       = "";
        probe.local_ip   = "";
        probe.local_port = local_port;
        auto payload = Fundamental::io::to_json(probe);
        auto encrypted = frp_udp_encrypt_string(state->send_key, payload);
        if (encrypted.empty()) {
            state->done = true;
            state->on_done(false);
            return;
        }
        state->attempts++;
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

frp_runtime_provider_agent::frp_runtime_provider_agent(frp_provider_config config) :
config_(std::move(config)),
uuid_(frp_generate_runtime_uuid()),
reconnect_timer_(io_context_pool::Instance().get_io_context()) {
}

void frp_runtime_provider_agent::start() {
    if (config_.nat_type != frp_runtime_nat_type_disabled && config_.public_server_udp_port != 0) {
        run_startup_probe(reconnect_timer_.get_executor(), config_.traffic_secret, config_.public_server_host,
                          { config_.public_server_udp_port,
                            static_cast<std::uint16_t>(config_.public_server_udp_port + 1) },
                          [this, self = shared_from_this()](bool ok) {
                              if (!reference_.is_valid()) return;
                              startup_probe_succeeded_ = ok;
                              connect_signal_channel();
                          });
    } else {
        connect_signal_channel();
    }
}

void frp_runtime_provider_agent::release_obj() {
    if (!reference_.release()) return;
    reconnect_timer_.cancel();
    for (auto& [_, flow] : flows_) {
        reset_runtime_p2p_state(*flow);
        if (flow->data_channel) {
            flow->data_channel->release_obj();
            flow->data_channel = nullptr;
        }
        std::error_code ec;
        flow->backend_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        flow->backend_socket.close(ec);
    }
    flows_.clear();
    if (channel_) {
        channel_->release_obj();
        channel_ = nullptr;
    }
}

void frp_runtime_provider_agent::connect_signal_channel() {
    if (!reference_.is_valid()) return;
    channel_ = frp_runtime_signal_client_channel::make_shared(reconnect_timer_.get_executor(), config_.public_server_host,
                                                              std::to_string(config_.public_server_tcp_port));
    channel_->enable_ssl(to_network_config(config_.ssl));
    channel_->set_on_connected([this, self = shared_from_this()] {
        FINFO("provider signal connected server={}:{} uuid={}", config_.public_server_host,
              config_.public_server_tcp_port, uuid_);
    });
    channel_->set_on_disconnected([this, self = shared_from_this()] {
        if (!reference_.is_valid()) return;
        FERR("provider signal disconnected uuid={}", uuid_);
        channel_ = nullptr;
        schedule_reconnect();
    });
    channel_->set_on_command([this, self = shared_from_this()](const frp_runtime_command_base& command, std::string payload) {
        process_command(command, std::move(payload));
    });
    channel_->start();
}

void frp_runtime_provider_agent::schedule_reconnect() {
    reconnect_timer_.expires_after(std::chrono::seconds(2));
    reconnect_timer_.async_wait([this, self = shared_from_this()](const asio::error_code& ec) {
        if (!reference_.is_valid() || ec) return;
        connect_signal_channel();
    });
}

void frp_runtime_provider_agent::start_udp_punch(const std::shared_ptr<provider_flow_runtime>& flow) {
    if (!flow || !channel_ || flow->closed || flow->p2p_success) return;

    const bool i_am_symmetric = (config_.nat_type == frp_runtime_nat_type_symmetric);

    std::mt19937 rng(std::random_device{}());

    if (i_am_symmetric) {
        // --- symmetric side: bind 32 sockets near local port xxx ---
        std::error_code ec;
        std::uint16_t xxx = flow->p2p_socket ? flow->p2p_socket->local_endpoint(ec).port() : 0;
        if (ec || xxx == 0) xxx = 40000;
        flow->punch_sockets.clear();
        std::set<std::uint16_t> used;
        {
            auto sock = std::make_unique<asio::ip::udp::socket>(reconnect_timer_.get_executor());
            if (!protocal_helper::udp_bind_endpoint(*sock, xxx)) {
                flow->punch_sockets.push_back(std::move(sock));
                used.insert(xxx);
            }
        }
        std::uniform_int_distribution<int> dist(
            std::max(1024, static_cast<int>(xxx) - 128),
            std::min(65535, static_cast<int>(xxx) + 128));
        int attempts = 0;
        while (static_cast<int>(flow->punch_sockets.size()) < kPunchSocketCount && attempts < 2000) {
            attempts++;
            auto port = static_cast<std::uint16_t>(dist(rng));
            if (used.count(port)) continue;
            auto sock = std::make_unique<asio::ip::udp::socket>(reconnect_timer_.get_executor());
            if (protocal_helper::udp_bind_endpoint(*sock, port)) continue;
            used.insert(port);
            flow->punch_sockets.push_back(std::move(sock));
        }
        FINFO("udp_punch symmetric provider flow_id={} sockets={} target={}:{}",
              flow->flow_id, flow->punch_sockets.size(),
              flow->p2p_peer_endpoint.address().to_string(), flow->p2p_peer_endpoint.port());
    } else {
        // --- full side: 1 socket (p2p_socket), scan peer's port range ---
        if (!flow->p2p_socket) {
            FERR("udp_punch full provider flow_id={} no p2p_socket", flow->flow_id);
            return;
        }
        flow->punch_sockets.clear();
    }

    flow->punch_active = true;
    flow->punch_round  = 0;

    auto do_round = std::make_shared<std::function<void()>>();
    *do_round = [this, self = shared_from_this(), flow, do_round, i_am_symmetric]() mutable {
        if (!reference_.is_valid() || flow->closed || !flow->punch_active || flow->p2p_success) return;

        const int max_rounds = kPunchMaxRounds + (i_am_symmetric ? 0 : 1);
        if (flow->punch_round >= max_rounds) {
            frp_runtime_flow_failed_data failed;
            failed.command = frp_runtime_flow_failed_command;
            failed.flow_id = flow->flow_id;
            failed.reason  = frp_runtime_flow_failed_punch_timeout;
            failed.message = "udp_punch all rounds exhausted";
            flow->closed   = true;
            reset_runtime_p2p_state(*flow);
            flows_.erase(flow->flow_id);
            if (channel_) channel_->send_command(failed);
            return;
        }

        std::vector<asio::ip::udp::endpoint> targets;
        if (i_am_symmetric) {
            targets.push_back(flow->p2p_peer_endpoint);
        } else {
            std::mt19937 rng2(std::random_device{}());
            const std::uint16_t peer_base = flow->p2p_peer_endpoint.port();
            const auto peer_addr = flow->p2p_peer_endpoint.address();
            if (flow->punch_round == 0) {
                std::vector<std::uint16_t> ports;
                for (int i = 0; i < kPunchScanRound0; ++i) {
                    std::uint16_t p = static_cast<std::uint16_t>(peer_base + i);
                    if (p < 23) continue;
                    ports.push_back(p);
                    flow->punch_scanned_ports.insert(p);
                }
                std::shuffle(ports.begin(), ports.end(), rng2);
                for (auto p : ports) targets.emplace_back(peer_addr, p);
            } else {
                std::uniform_int_distribution<int> dist2(23, 65535);
                int picked = 0, tries = 0;
                while (picked < kPunchScanPerRound && tries < 100000) {
                    tries++;
                    auto p = static_cast<std::uint16_t>(dist2(rng2));
                    if (flow->punch_scanned_ports.count(p)) continue;
                    flow->punch_scanned_ports.insert(p);
                    targets.emplace_back(peer_addr, p);
                    picked++;
                }
            }
        }

        auto retransmit_index = std::make_shared<int>(0);
        auto do_retransmit = std::make_shared<std::function<void()>>();
        *do_retransmit = [this, self, flow, do_round, do_retransmit, retransmit_index,
                          targets = std::move(targets), i_am_symmetric]() mutable {
            if (!reference_.is_valid() || flow->closed || !flow->punch_active || flow->p2p_success) return;
            if (*retransmit_index >= kPunchRetransmitCount) {
                flow->punch_round++;
                flow->punch_timer.expires_after(std::chrono::milliseconds(kPunchRoundMs));
                flow->punch_timer.async_wait([do_round](const std::error_code& ec) mutable {
                    if (ec) return;
                    (*do_round)();
                });
                return;
            }
            (*retransmit_index)++;

            std::uint32_t fid = flow->flow_id;
            if (i_am_symmetric) {
                for (auto& sock : flow->punch_sockets) {
                    if (!sock) continue;
                    std::error_code ec;
                    std::uint16_t local_port = sock->local_endpoint(ec).port();
                    if (ec) continue;
                    auto pkt = std::make_shared<std::array<std::uint8_t, 6>>();
                    std::memcpy(pkt->data(), &fid, 4);
                    std::memcpy(pkt->data() + 4, &local_port, 2);
                    sock->async_send_to(asio::buffer(*pkt), targets[0],
                                        [pkt](const std::error_code&, std::size_t) {});
                }
            } else {
                std::uint16_t local_port = 0;
                {
                    std::error_code ec;
                    local_port = flow->p2p_socket->local_endpoint(ec).port();
                }
                for (const auto& tgt : targets) {
                    auto pkt = std::make_shared<std::array<std::uint8_t, 6>>();
                    std::memcpy(pkt->data(), &fid, 4);
                    std::memcpy(pkt->data() + 4, &local_port, 2);
                    flow->p2p_socket->async_send_to(asio::buffer(*pkt), tgt,
                                                    [pkt](const std::error_code&, std::size_t) {});
                }
            }

            flow->punch_timer.expires_after(std::chrono::milliseconds(kPunchRetransmitMs));
            flow->punch_timer.async_wait([do_retransmit](const std::error_code& ec) mutable {
                if (ec) return;
                (*do_retransmit)();
            });
        };
        (*do_retransmit)();
    };

    // For symmetric side, start read loops on punch_sockets
    if (i_am_symmetric) {
        for (auto& sock : flow->punch_sockets) {
            if (!sock) continue;
            auto punch_sock_ptr = sock.get();
            auto recv_buf = std::make_shared<std::array<char, 64>>();
            auto recv_ep  = std::make_shared<asio::ip::udp::endpoint>();
            std::function<void()> do_recv;
            do_recv = [this, self = shared_from_this(), flow, punch_sock_ptr, recv_buf, recv_ep, do_recv]() mutable {
                if (!reference_.is_valid() || flow->closed || !flow->punch_active || flow->p2p_success) return;
                punch_sock_ptr->async_receive_from(
                    asio::buffer(recv_buf->data(), recv_buf->size()), *recv_ep,
                    [this, self, flow, punch_sock_ptr, recv_buf, recv_ep, do_recv]
                    (const std::error_code& ec, std::size_t bytes) mutable {
                        if (!reference_.is_valid() || flow->closed) return;
                        if (!ec && bytes == 6 && flow->punch_active && !flow->p2p_success) {
                            std::uint32_t pkt_fid = 0;
                            std::memcpy(&pkt_fid, recv_buf->data(), 4);
                            std::uint16_t peer_lp = 0;
                            std::memcpy(&peer_lp, recv_buf->data() + 4, 2);
                            if (pkt_fid == flow->flow_id) {
                                for (auto& s : flow->punch_sockets) {
                                    if (s.get() == punch_sock_ptr) {
                                        flow->p2p_socket = std::move(s);
                                        break;
                                    }
                                }
                                for (auto& s : flow->punch_sockets) {
                                    if (s) { std::error_code ce; s->close(ce); }
                                }
                                flow->punch_sockets.clear();
                                flow->punch_active = false;
                                flow->punch_timer.cancel();
                                flow->p2p_success = true;
                                flow->transport_ready = true;
                                flow->p2p_send_context.socket        = &flow->p2p_socket;
                                flow->p2p_send_context.peer_endpoint = &flow->p2p_peer_endpoint;
                                flow->p2p_peer_endpoint = *recv_ep;
                                std::error_code ep_ec;
                                log_provider_channel_established(
                                    flow->flow_id, flow->service_name, "p2p",
                                    format_runtime_endpoint(flow->p2p_socket->local_endpoint(ep_ec)),
                                    format_runtime_endpoint(flow->p2p_peer_endpoint));
                                FINFO("udp_punch succeeded (symmetric recv) flow_id={} peer_local_port={}", flow->flow_id, peer_lp);
                                start_provider_p2p_read_loop(flow);
                                frp_runtime_p2p_connected_data connected;
                                connected.command = frp_runtime_p2p_connected_command;
                                connected.flow_id = flow->flow_id;
                                connected.matched_peer_local_port = peer_lp;
                                if (channel_) channel_->send_command(connected);
                                return;
                            }
                        }
                        if (!flow->closed && flow->punch_active && !flow->p2p_success) do_recv();
                    });
            };
            do_recv();
        }
    }

    (*do_round)();
}

void frp_runtime_provider_agent::provider_kcp_send(const std::shared_ptr<provider_flow_runtime>& flow,
                                                   const char* data,
                                                   std::size_t size) {
    if (!flow || !flow->kcp || flow->closed) return;
    // Encrypt application data before sending through KCP
    std::vector<std::uint8_t> plaintext(data, data + size);
    auto encrypted = frp_udp_encrypt(flow->udp_send_key, plaintext);
    if (encrypted.empty()) return;
    FINFO("provider kcp_send flow_id={} plaintext_size={} encrypted_size={}", flow->flow_id, size, encrypted.size());
    if (ikcp_send(flow->kcp.get(), reinterpret_cast<const char*>(encrypted.data()), static_cast<int>(encrypted.size())) < 0) return;
    ikcp_update(flow->kcp.get(), frp_runtime_kcp_clock());
}

void frp_runtime_provider_agent::schedule_provider_kcp_update(const std::shared_ptr<provider_flow_runtime>& flow) {
    if (!flow || !flow->kcp || flow->closed) return;
    flow->kcp_update_timer.expires_after(std::chrono::milliseconds(20));
    flow->kcp_update_timer.async_wait([this, self = shared_from_this(), flow](const std::error_code& ec) {
        if (ec || !reference_.is_valid() || !flow->kcp || flow->closed) return;
        ikcp_update(flow->kcp.get(), frp_runtime_kcp_clock());
        if (flow->backend_disconnected && ikcp_waitsnd(flow->kcp.get()) == 0) {
            flow->closed = true;
            flow->p2p_timer.cancel();
            reset_runtime_p2p_state(*flow);
            flows_.erase(flow->flow_id);
            frp_runtime_flow_closed_data closed;
            closed.command = frp_runtime_flow_closed_command;
            closed.flow_id = flow->flow_id;
            if (channel_) channel_->send_command(closed);
            return;
        }
        schedule_provider_kcp_update(flow);
    });
}

void frp_runtime_provider_agent::start_provider_p2p_read_loop(const std::shared_ptr<provider_flow_runtime>& flow) {
    if (!flow || !flow->p2p_socket || flow->closed) return;
    flow->p2p_socket->async_receive_from(
        asio::buffer(flow->p2p_read_buf.data(), flow->p2p_read_buf.size()),
        flow->p2p_recv_endpoint,
        [this, self = shared_from_this(), flow](const asio::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (ec) {
                FINFO("provider p2p recv error flow_id={} err={}", flow->flow_id, ec.message());
                if (flow->closed) return;
                start_provider_p2p_read_loop(flow);
                return;
            }
            if (flow->closed || !flow->kcp) return;

            FINFO("provider p2p recv flow_id={} bytes={} from={}:{}", flow->flow_id, bytes_read,
                  flow->p2p_recv_endpoint.address().to_string(), flow->p2p_recv_endpoint.port());

            // 1-byte: keepalive, discard
            if (bytes_read == 1) {
                start_provider_p2p_read_loop(flow);
                return;
            }

            // 6-byte: udp_punch probe packet
            if (bytes_read == 6) {
                if (flow->punch_active && !flow->p2p_success) {
                    std::uint32_t pkt_flow_id = 0;
                    std::memcpy(&pkt_flow_id, flow->p2p_read_buf.data(), 4);
                    std::uint16_t peer_local_port = 0;
                    std::memcpy(&peer_local_port, flow->p2p_read_buf.data() + 4, 2);
                    FINFO("provider p2p recv punch probe flow_id={} pkt_flow_id={} peer_local_port={}", flow->flow_id, pkt_flow_id, peer_local_port);
                    if (pkt_flow_id == flow->flow_id) {
                        flow->p2p_success = true;
                        flow->punch_active = false;
                        flow->punch_timer.cancel();
                        flow->transport_ready = true;
                        // Promote the socket that received this packet
                        // (p2p_socket is already the receiving socket for provider)
                        // Close all punch sockets except p2p_socket
                        for (auto& sock : flow->punch_sockets) {
                            if (sock) { std::error_code ec; sock->close(ec); }
                        }
                        flow->punch_sockets.clear();
                        std::error_code ep_ec;
                        log_provider_channel_established(
                            flow->flow_id, flow->service_name, "p2p",
                            format_runtime_endpoint(flow->p2p_socket->local_endpoint(ep_ec)),
                            format_runtime_endpoint(flow->p2p_peer_endpoint));
                        FINFO("udp_punch succeeded flow_id={} peer_local_port={}", flow->flow_id, peer_local_port);
                        frp_runtime_p2p_connected_data connected;
                        connected.command = frp_runtime_p2p_connected_command;
                        connected.flow_id = flow->flow_id;
                        connected.matched_peer_local_port = peer_local_port;
                        if (channel_) channel_->send_command(connected);
                    }
                }
                start_provider_p2p_read_loop(flow);
                return;
            }

            // < 24 bytes: discard (too small for KCP)
            if (bytes_read < 24) {
                FINFO("provider p2p recv discarding short packet flow_id={} bytes={}", flow->flow_id, bytes_read);
                start_provider_p2p_read_loop(flow);
                return;
            }

            // KCP input
            FINFO("provider p2p recv KCP input flow_id={} bytes={}", flow->flow_id, bytes_read);
            ikcp_input(flow->kcp.get(), flow->p2p_read_buf.data(), static_cast<long>(bytes_read));
            ikcp_update(flow->kcp.get(), frp_runtime_kcp_clock());
            std::array<char, 16 * 1024> recv_buf {};
            while (true) {
                auto recv_size = ikcp_recv(flow->kcp.get(), recv_buf.data(), static_cast<int>(recv_buf.size()));
                if (recv_size < 0) break;
                // Decrypt application data received from KCP
                std::vector<std::uint8_t> encrypted(recv_buf.data(), recv_buf.data() + recv_size);
                auto plaintext = frp_udp_decrypt(flow->udp_recv_key, encrypted);
                if (!plaintext) {
                    FINFO("provider flow {} failed to decrypt KCP payload size={}", flow->flow_id, recv_size);
                    continue;
                }
                FINFO("provider p2p KCP recv decrypted flow_id={} size={}", flow->flow_id, plaintext->size());
                flow->pending_writes.emplace_back(reinterpret_cast<const char*>(plaintext->data()), plaintext->size());
                handle_backend_write_queue(flow);
            }
            start_provider_p2p_read_loop(flow);
        });
}

void frp_runtime_provider_agent::start_provider_backend_connect(const std::shared_ptr<provider_flow_runtime>& flow) {
    auto service = find_service(flow->service_name);
    if (!service || !channel_) {
        frp_runtime_flow_failed_data failed;
        failed.command = frp_runtime_flow_failed_command;
        failed.flow_id = flow->flow_id;
        failed.reason  = frp_runtime_flow_failed_relay_channel_open_failed;
        failed.message = "service not found locally";
        flow->closed   = true;
        flows_.erase(flow->flow_id);
        channel_->send_command(failed);
        return;
    }
    flow->resolver.async_resolve(
        service->target_host,
        std::to_string(service->target_port),
        [this, self = shared_from_this(), flow](const std::error_code& ec,
                                                const asio::ip::tcp::resolver::results_type& endpoints) {
            if (!reference_.is_valid() || !channel_ || flow->closed) return;
            if (ec) {
                frp_runtime_flow_failed_data failed;
                failed.command = frp_runtime_flow_failed_command;
                failed.flow_id = flow->flow_id;
                failed.reason  = frp_runtime_flow_failed_backend_connect_failed;
                failed.message = ec.message();
                flow->closed   = true;
                flows_.erase(flow->flow_id);
                channel_->send_command(failed);
                return;
            }
            asio::async_connect(
                flow->backend_socket,
                endpoints,
                [this, self = shared_from_this(), flow](const asio::error_code& ec, const asio::ip::tcp::endpoint&) {
                    if (!reference_.is_valid() || !channel_ || flow->closed) return;
                    if (ec) {
                        frp_runtime_flow_failed_data failed;
                        failed.command = frp_runtime_flow_failed_command;
                        failed.flow_id = flow->flow_id;
                        failed.reason  = frp_runtime_flow_failed_backend_connect_failed;
                        failed.message = ec.message();
                        flow->closed   = true;
                        flows_.erase(flow->flow_id);
                        channel_->send_command(failed);
                        return;
                    }
                    flow->backend_connected = true;
                    frp_runtime_flow_ready_data ready_signal;
                    ready_signal.command = frp_runtime_flow_ready_command;
                    ready_signal.flow_id = flow->flow_id;
                    channel_->send_command(ready_signal);
                    start_backend_read_loop(flow);
                });
        });
}

void frp_runtime_provider_agent::process_command(const frp_runtime_command_base& command, std::string payload) {
    if (!channel_) return;
    switch (command.command) {
    case frp_runtime_server_hello_command: {
        frp_runtime_server_hello_data hello;
        if (!Fundamental::io::from_json(payload, hello)) {
            channel_->release_obj();
            return;
        }
        FINFO("provider recv server_hello nonce={}", hello.server_nonce);
        frp_runtime_auth_request_data request;
        request.command = frp_runtime_auth_request_command;
        request.digest  = frp_hmac_sha256_hex(config_.traffic_secret, hello.server_nonce);
        channel_->send_command(request);
        return;
    }
    case frp_runtime_auth_response_command: {
        frp_runtime_auth_response_data response;
        if (!Fundamental::io::from_json(payload, response) || !response.ok) {
            FERR("provider auth failed uuid={} msg={}", uuid_, response.message);
            channel_->release_obj();
            return;
        }
        FINFO("provider recv auth_response ok uuid={}", uuid_);
        frp_runtime_join_request_data join;
        join.command      = frp_runtime_join_request_command;
        join.role         = frp_runtime_provider_role;
        join.uuid         = uuid_;
        join.register_key = config_.register_key;
        join.nat_type = (config_.nat_type != frp_runtime_nat_type_disabled && startup_probe_succeeded_)
                            ? config_.nat_type
                            : frp_runtime_nat_type_disabled;
        channel_->send_command(join);
        return;
    }
    case frp_runtime_join_response_command: {
        frp_runtime_join_response_data response;
        if (!Fundamental::io::from_json(payload, response) || !response.ok) {
            FERR("provider join failed uuid={} msg={}", uuid_, response.message);
            channel_->release_obj();
            return;
        }
        FINFO("provider recv join_response ok uuid={}", uuid_);
        frp_runtime_register_services_request_data request;
        request.command = frp_runtime_register_services_request_command;
        request.services.reserve(config_.services.size());
        for (const auto& service : config_.services) {
            frp_runtime_service_registration_data data;
            data.service_name = service.service_name;
            request.services.push_back(std::move(data));
        }
        channel_->send_command(request);
        return;
    }
    case frp_runtime_register_services_response_command: {
        frp_runtime_register_services_response_data response;
        if (!Fundamental::io::from_json(payload, response) || !response.ok) {
            FERR("provider register services failed uuid={} msg={}", uuid_, response.message);
            channel_->release_obj();
            return;
        }
        FINFO("provider runtime ready uuid={} services={}", uuid_, config_.services.size());
        return;
    }
    case frp_runtime_prepare_flow_command: {
        frp_runtime_prepare_flow_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            channel_->release_obj();
            return;
        }
        FINFO("provider recv prepare_flow flow_id={} service_name={} transport={} accessor_uuid={}",
              request.flow_id, request.service_name,
              request.transport == frp_runtime_transport_p2p ? "p2p" : "tcp_relay",
              request.accessor_uuid);
        auto service = find_service(request.service_name);
        if (!service) {
            frp_runtime_flow_failed_data failed;
            failed.command = frp_runtime_flow_failed_command;
            failed.flow_id = request.flow_id;
            failed.reason  = frp_runtime_flow_failed_relay_channel_open_failed;
            failed.message = "service not found locally";
            channel_->send_command(failed);
            return;
        }
        auto flow = std::make_shared<provider_flow_runtime>(reconnect_timer_.get_executor());
        flow->flow_id       = request.flow_id;
        flow->service_name  = request.service_name;
        flow->accessor_uuid = request.accessor_uuid;
        flows_[flow->flow_id] = flow;
        if (request.transport == frp_runtime_transport_p2p) {
            start_flow_endpoint_probe(flow);
            return;
        }
        flow->data_channel = frp_runtime_data_client_channel::make_shared(reconnect_timer_.get_executor(),
                                                                          config_.public_server_host,
                                                                          std::to_string(config_.public_server_tcp_port),
                                                                          flow->flow_id,
                                                                          uuid_);
        flow->data_channel->enable_ssl(to_network_config(config_.ssl));
        flow->data_channel->set_on_disconnected([this, self = shared_from_this(), flow] {
            if (!reference_.is_valid() || !channel_ || flow->closed) return;
            flow->closed = true;
            frp_runtime_flow_failed_data failed;
            failed.command = frp_runtime_flow_failed_command;
            failed.flow_id = flow->flow_id;
            failed.reason  = frp_runtime_flow_failed_relay_channel_open_failed;
            failed.message = "provider data channel disconnected";
            flows_.erase(flow->flow_id);
            channel_->send_command(failed);
        });
        flow->data_channel->set_on_data([this, self = shared_from_this(), flow](std::string data) {
            if (!reference_.is_valid() || flow->closed) return;
            flow->pending_writes.push_back(std::move(data));
            handle_backend_write_queue(flow);
        });
        flow->data_channel->start();
        return;
    }
    case frp_runtime_flow_transport_ready_command: {
        frp_runtime_flow_transport_ready_data ready;
        if (!Fundamental::io::from_json(payload, ready)) {
            channel_->release_obj();
            return;
        }
        FINFO("provider recv flow_transport_ready flow_id={}", ready.flow_id);
        auto it = flows_.find(ready.flow_id);
        if (it == flows_.end()) return;
        auto flow = it->second;
        if (flow->transport_ready || flow->closed) return;
        flow->transport_ready = true;
        if (flow->data_channel) {
            log_provider_channel_established(
                flow->flow_id, flow->service_name, "tcp_relay",
                flow->data_channel->local_endpoint_string(), flow->data_channel->remote_endpoint_string());
        }
        auto service = find_service(flow->service_name);
        if (!service) {
            frp_runtime_flow_failed_data failed;
            failed.command = frp_runtime_flow_failed_command;
            failed.flow_id = flow->flow_id;
            failed.reason  = frp_runtime_flow_failed_relay_channel_open_failed;
            failed.message = "service not found locally";
            flow->closed   = true;
            flows_.erase(flow->flow_id);
            channel_->send_command(failed);
            return;
        }
        flow->resolver.async_resolve(
            service->target_host,
            std::to_string(service->target_port),
            [this, self = shared_from_this(), flow](const std::error_code& ec,
                                                    const asio::ip::tcp::resolver::results_type& endpoints) {
                if (!reference_.is_valid() || !channel_ || flow->closed) return;
                if (ec) {
                    frp_runtime_flow_failed_data failed;
                    failed.command = frp_runtime_flow_failed_command;
                    failed.flow_id = flow->flow_id;
                    failed.reason  = frp_runtime_flow_failed_backend_connect_failed;
                    failed.message = ec.message();
                    flow->closed   = true;
                    flows_.erase(flow->flow_id);
                    channel_->send_command(failed);
                    return;
                }
                asio::async_connect(
                    flow->backend_socket,
                    endpoints,
                    [this, self = shared_from_this(), flow](const asio::error_code& ec, const asio::ip::tcp::endpoint&) {
                        if (!reference_.is_valid() || !channel_ || flow->closed) return;
                        if (ec) {
                            frp_runtime_flow_failed_data failed;
                            failed.command = frp_runtime_flow_failed_command;
                            failed.flow_id = flow->flow_id;
                            failed.reason  = frp_runtime_flow_failed_backend_connect_failed;
                            failed.message = ec.message();
                            flow->closed   = true;
                            flows_.erase(flow->flow_id);
                            channel_->send_command(failed);
                            return;
                        }
                        flow->backend_connected = true;
                        frp_runtime_flow_ready_data ready_signal;
                        ready_signal.command = frp_runtime_flow_ready_command;
                        ready_signal.flow_id = flow->flow_id;
                        channel_->send_command(ready_signal);
                        start_backend_read_loop(flow);
                    });
            });
        return;
    }
    case frp_runtime_flow_endpoint_ready_command: {
        frp_runtime_flow_endpoint_ready_data ep_ready;
        if (!Fundamental::io::from_json(payload, ep_ready)) {
            channel_->release_obj();
            return;
        }
        auto it = flows_.find(ep_ready.flow_id);
        if (it == flows_.end()) return;
        auto flow = it->second;
        flow->awaiting_endpoint_ready = false;
        flow->endpoint_probe_timer.cancel();
        FINFO("provider flow {} flow_endpoint_ready external={}:{}", flow->flow_id, ep_ready.external_ip, ep_ready.external_port);
        return;
    }
    case frp_runtime_flow_p2p_peer_command: {
        frp_runtime_flow_p2p_peer_data peer;
        if (!Fundamental::io::from_json(payload, peer)) {
            channel_->release_obj();
            return;
        }
        auto it = flows_.find(peer.flow_id);
        if (it == flows_.end()) return;
        auto flow = it->second;
        if (!flow->p2p_socket || !flow->kcp) return;
        std::error_code ec;
        flow->p2p_peer_endpoint =
            asio::ip::udp::endpoint(asio::ip::make_address(peer.peer_host, ec), peer.peer_port);
        if (ec) {
            frp_runtime_flow_failed_data failed;
            failed.command = frp_runtime_flow_failed_command;
            failed.flow_id = flow->flow_id;
            failed.reason  = frp_runtime_flow_failed_flow_endpoint_probe_timeout;
            failed.message = ec.message();
            flow->closed   = true;
            reset_runtime_p2p_state(*flow);
            flows_.erase(flow->flow_id);
            channel_->send_command(failed);
            return;
        }
        flow->peer_nat_type = peer.peer_nat_type;
        FINFO("provider flow {} flow_p2p_peer peer={}:{} peer_nat_type={} starting udp_punch",
              flow->flow_id, peer.peer_host, peer.peer_port, static_cast<int>(peer.peer_nat_type));
        start_udp_punch(flow);
        if (!flow->backend_connected) {
            start_provider_backend_connect(flow);
        }
        return;
    }
    case frp_runtime_peer_p2p_connected_command: {
        frp_runtime_peer_p2p_connected_data connected;
        if (!Fundamental::io::from_json(payload, connected)) {
            channel_->release_obj();
            return;
        }
        auto it = flows_.find(connected.flow_id);
        if (it == flows_.end()) return;
        auto flow = it->second;
        if (flow->p2p_success) return;
        // Find the punch socket matching matched_peer_local_port and promote it to p2p_socket
        if (connected.matched_peer_local_port != 0 && !flow->punch_sockets.empty()) {
            for (auto& sock : flow->punch_sockets) {
                if (!sock) continue;
                std::error_code ec;
                auto lp = sock->local_endpoint(ec).port();
                if (!ec && lp == connected.matched_peer_local_port) {
                    flow->p2p_socket = std::move(sock);
                    break;
                }
            }
        }
        // Close remaining punch sockets
        for (auto& sock : flow->punch_sockets) {
            if (sock) { std::error_code ec; sock->close(ec); }
        }
        flow->punch_sockets.clear();
        flow->punch_active = false;
        flow->punch_timer.cancel();
        flow->p2p_success = true;
        flow->transport_ready = true;
        if (flow->p2p_socket) {
            flow->p2p_send_context.socket        = &flow->p2p_socket;
            flow->p2p_send_context.peer_endpoint = &flow->p2p_peer_endpoint;
            start_provider_p2p_read_loop(flow);
        }
        FINFO("provider flow {} peer_p2p_connected p2p_success=true matched_port={}", flow->flow_id, connected.matched_peer_local_port);
        return;
    }
    case frp_runtime_flow_data_command: {
        return;
    }
    case frp_runtime_flow_closed_command: {
        frp_runtime_flow_closed_data closed;
        if (!Fundamental::io::from_json(payload, closed)) {
            channel_->release_obj();
            return;
        }
        auto it = flows_.find(closed.flow_id);
        if (it != flows_.end()) {
            it->second->closed = true;
            reset_runtime_p2p_state(*it->second);
            if (it->second->data_channel) {
                it->second->data_channel->release_obj();
                it->second->data_channel = nullptr;
            }
            std::error_code ec;
            it->second->backend_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            it->second->backend_socket.close(ec);
            flows_.erase(it);
        }
        return;
    }
    case frp_runtime_flow_failed_command: {
        frp_runtime_flow_failed_data failed;
        if (!Fundamental::io::from_json(payload, failed)) {
            channel_->release_obj();
            return;
        }
        auto it = flows_.find(failed.flow_id);
        if (it != flows_.end()) {
            it->second->closed = true;
            reset_runtime_p2p_state(*it->second);
            if (it->second->data_channel) {
                it->second->data_channel->release_obj();
                it->second->data_channel = nullptr;
            }
            std::error_code ec;
            it->second->backend_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            it->second->backend_socket.close(ec);
            flows_.erase(it);
        }
        return;
    }
    case frp_runtime_ping_response_command: return;
    default: return;
    }
}

void frp_runtime_provider_agent::start_flow_endpoint_probe(const std::shared_ptr<provider_flow_runtime>& flow) {
    if (!flow || !channel_ || config_.public_server_udp_port == 0) {
        frp_runtime_flow_failed_data failed;
        failed.command = frp_runtime_flow_failed_command;
        failed.flow_id = flow ? flow->flow_id : 0;
        failed.reason  = frp_runtime_flow_failed_flow_endpoint_probe_timeout;
        failed.message = "udp p2p is not available";
        if (flow) flows_.erase(flow->flow_id);
        channel_->send_command(failed);
        return;
    }

    // Derive UDP encryption key for this flow (shared traffic key)
    flow->udp_send_key = frp_derive_traffic_key(config_.traffic_secret);
    flow->udp_recv_key = frp_derive_traffic_key(config_.traffic_secret);
    if (flow->udp_send_key.empty() || flow->udp_recv_key.empty()) {
        frp_runtime_flow_failed_data failed;
        failed.command = frp_runtime_flow_failed_command;
        failed.flow_id = flow->flow_id;
        failed.reason  = frp_runtime_flow_failed_flow_endpoint_probe_timeout;
        failed.message = "failed to derive UDP encryption keys";
        flow->closed   = true;
        flows_.erase(flow->flow_id);
        channel_->send_command(failed);
        return;
    }

    flow->p2p_socket = std::make_unique<asio::ip::udp::socket>(reconnect_timer_.get_executor());
    std::error_code ec;
    protocal_helper::udp_bind_endpoint(*flow->p2p_socket, 0);
    flow->p2p_send_context.socket        = &flow->p2p_socket;
    flow->p2p_send_context.peer_endpoint = &flow->p2p_peer_endpoint;
    flow->kcp = std::unique_ptr<ikcpcb, kcp_releaser>(ikcp_create(flow->flow_id, &flow->p2p_send_context));
    ikcp_setoutput(flow->kcp.get(), frp_runtime_kcp_output);
    flow->kcp.get()->stream = 0;
    ikcp_wndsize(flow->kcp.get(), 256, 256);
    ikcp_nodelay(flow->kcp.get(), 1, 20, 2, 1);
    schedule_provider_kcp_update(flow);
    start_provider_p2p_read_loop(flow);

    flow->awaiting_endpoint_ready = true;
    flow->endpoint_probe_attempts = 0;
    auto local_port = flow->p2p_socket->local_endpoint(ec).port();
    FINFO("provider start_flow_endpoint_probe flow_id={} local_port={}", flow->flow_id, local_port);
    auto fn = std::make_shared<std::function<void()>>();
    *fn = [this, self = shared_from_this(), flow, local_port, fn]() mutable {
        if (!reference_.is_valid() || !channel_ || flow->closed || !flow->awaiting_endpoint_ready) return;
        if (flow->endpoint_probe_attempts >= kMaxEndpointProbeAttempts) {
            frp_runtime_flow_failed_data failed;
            failed.command = frp_runtime_flow_failed_command;
            failed.flow_id = flow->flow_id;
            failed.reason  = frp_runtime_flow_failed_flow_endpoint_probe_timeout;
            failed.message = "flow_endpoint_probe timeout";
            flow->closed   = true;
            reset_runtime_p2p_state(*flow);
            flows_.erase(flow->flow_id);
            channel_->send_command(failed);
            return;
        }
        frp_runtime_p2p_probe_data probe;
        probe.command    = frp_runtime_p2p_probe_command;
        probe.flow_id    = flow->flow_id;
        probe.uuid       = uuid_;
        probe.local_ip   = config_.local_ip;
        probe.local_port = local_port;
        auto payload = Fundamental::io::to_json(probe);
        auto encrypted = frp_udp_encrypt_string(flow->udp_send_key, payload);
        if (encrypted.empty()) return;
        auto server_endpoint = resolve_udp_endpoint(reconnect_timer_.get_executor(), config_.public_server_host,
                                                    config_.public_server_udp_port);
        if (!server_endpoint) return;
        flow->endpoint_probe_attempts++;
        FINFO("provider flow_endpoint_probe attempt={} flow_id={} local_port={} server={}:{} probe_size={}",
              flow->endpoint_probe_attempts, flow->flow_id, local_port,
              config_.public_server_host, config_.public_server_udp_port, encrypted.size());
        auto enc_ptr = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted));
        flow->p2p_socket->async_send_to(asio::buffer(*enc_ptr), *server_endpoint,
                                        [enc_ptr](const std::error_code&, std::size_t) {});
        flow->endpoint_probe_timer.expires_after(std::chrono::milliseconds(200));
        flow->endpoint_probe_timer.async_wait([fn](const std::error_code& ec) mutable {
            if (ec) return;
            (*fn)();
        });
    };
    (*fn)();
}

void frp_runtime_provider_agent::start_backend_read_loop(const std::shared_ptr<provider_flow_runtime>& flow) {
    if (!flow || !flow->backend_connected || !channel_ || (!flow->data_channel && !flow->kcp)) return;
    flow->backend_socket.async_read_some(
        network_read_buffer_t(flow->read_buf.data(), flow->read_buf.size()),
        [this, self = shared_from_this(), flow](const asio::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid() || !channel_) return;
            if (ec) {
                if (flow->closed) return;
                if (flow->kcp) {
                    // P2P mode: delay close to let KCP flush pending data
                    flow->backend_disconnected = true;
                    flow->p2p_timer.expires_after(std::chrono::seconds(5));
                    flow->p2p_timer.async_wait([this, self = shared_from_this(), flow](const std::error_code& timer_ec) {
                        if (timer_ec || flow->closed) return;
                        flow->closed = true;
                        reset_runtime_p2p_state(*flow);
                        flows_.erase(flow->flow_id);
                        frp_runtime_flow_closed_data closed;
                        closed.command = frp_runtime_flow_closed_command;
                        closed.flow_id = flow->flow_id;
                        if (channel_) channel_->send_command(closed);
                    });
                    return;
                }
                flow->closed = true;
                frp_runtime_flow_closed_data closed;
                closed.command = frp_runtime_flow_closed_command;
                closed.flow_id = flow->flow_id;
                reset_runtime_p2p_state(*flow);
                if (flow->data_channel) {
                    flow->data_channel->release_obj();
                    flow->data_channel = nullptr;
                }
                flows_.erase(flow->flow_id);
                channel_->send_command(closed);
                return;
            }
            if (flow->data_channel) {
                flow->data_channel->send_bytes(std::make_shared<std::string>(flow->read_buf.data(), bytes_read));
            } else if (flow->kcp) {
                provider_kcp_send(flow, flow->read_buf.data(), bytes_read);
            }
            start_backend_read_loop(flow);
        });
}

void frp_runtime_provider_agent::handle_backend_write_queue(const std::shared_ptr<provider_flow_runtime>& flow) {
    if (!flow || !flow->backend_connected || flow->writing || flow->pending_writes.empty()) return;
    flow->writing = true;
    auto& current = flow->pending_writes.front();
    asio::async_write(flow->backend_socket,
                      asio::buffer(current.data(), current.size()),
                      [this, self = shared_from_this(), flow](const asio::error_code& ec, std::size_t) {
                          flow->writing = false;
                          if (!reference_.is_valid() || !channel_) return;
                          if (ec) {
                              if (flow->closed) return;
                              flow->closed = true;
                              frp_runtime_flow_failed_data failed;
                              failed.command = frp_runtime_flow_failed_command;
                              failed.flow_id = flow->flow_id;
                              failed.reason  = frp_runtime_flow_failed_backend_connect_failed;
                              failed.message = ec.message();
                              reset_runtime_p2p_state(*flow);
                              if (flow->data_channel) {
                                  flow->data_channel->release_obj();
                                  flow->data_channel = nullptr;
                              }
                              flows_.erase(flow->flow_id);
                              channel_->send_command(failed);
                              return;
                          }
                          flow->pending_writes.pop_front();
                          handle_backend_write_queue(flow);
                      });
}

std::optional<frp_provider_service_config> frp_runtime_provider_agent::find_service(std::string_view service_name) const {
    for (const auto& service : config_.services) {
        if (service.service_name == service_name) return service;
    }
    return std::nullopt;
}

frp_runtime_accessor_agent::frp_runtime_accessor_agent(frp_accessor_config config) :
config_(std::move(config)),
uuid_(frp_generate_runtime_uuid()),
reconnect_timer_(io_context_pool::Instance().get_io_context()) {
}

void frp_runtime_accessor_agent::start() {
    if (config_.nat_type != frp_runtime_nat_type_disabled && config_.public_server_udp_port != 0) {
        run_startup_probe(reconnect_timer_.get_executor(), config_.traffic_secret, config_.public_server_host,
                          { config_.public_server_udp_port,
                            static_cast<std::uint16_t>(config_.public_server_udp_port + 1) },
                          [this, self = shared_from_this()](bool ok) {
                              if (!reference_.is_valid()) return;
                              startup_probe_succeeded_ = ok;
                              connect_signal_channel();
                          });
    } else {
        connect_signal_channel();
    }
}

void frp_runtime_accessor_agent::release_obj() {
    if (!reference_.release()) return;
    reconnect_timer_.cancel();
    clear_listeners();
    if (channel_) {
        channel_->release_obj();
        channel_ = nullptr;
    }
}

void frp_runtime_accessor_agent::connect_signal_channel() {
    if (!reference_.is_valid()) return;
    channel_ = frp_runtime_signal_client_channel::make_shared(reconnect_timer_.get_executor(), config_.public_server_host,
                                                              std::to_string(config_.public_server_tcp_port));
    channel_->enable_ssl(to_network_config(config_.ssl));
    channel_->set_on_connected([this, self = shared_from_this()] {
        FINFO("accessor signal connected server={}:{} uuid={}", config_.public_server_host,
              config_.public_server_tcp_port, uuid_);
    });
    channel_->set_on_disconnected([this, self = shared_from_this()] {
        if (!reference_.is_valid()) return;
        FERR("accessor signal disconnected uuid={}, clear listeners", uuid_);
        clear_listeners();
        channel_ = nullptr;
        schedule_reconnect();
    });
    channel_->set_on_command([this, self = shared_from_this()](const frp_runtime_command_base& command, std::string payload) {
        process_command(command, std::move(payload));
    });
    channel_->start();
}

void frp_runtime_accessor_agent::schedule_reconnect() {
    reconnect_timer_.expires_after(std::chrono::seconds(2));
    reconnect_timer_.async_wait([this, self = shared_from_this()](const asio::error_code& ec) {
        if (!reference_.is_valid() || ec) return;
        connect_signal_channel();
    });
}

void frp_runtime_accessor_agent::clear_listeners() {
    for (auto& [_, listener] : listeners_) {
        std::error_code ec;
        listener->acceptor.close(ec);
    }
    listeners_.clear();
    for (auto& [_, session] : sessions_by_flow_id_) {
        reset_runtime_p2p_state(*session);
        if (session->data_channel) {
            session->data_channel->release_obj();
            session->data_channel = nullptr;
        }
        std::error_code ec;
        session->local_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        session->local_socket.close(ec);
    }
    sessions_by_flow_id_.clear();
    for (auto& [_, session] : pending_sessions_) {
        reset_runtime_p2p_state(*session);
        if (session->data_channel) {
            session->data_channel->release_obj();
            session->data_channel = nullptr;
        }
        std::error_code ec;
        session->local_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        session->local_socket.close(ec);
    }
    pending_sessions_.clear();
}

void frp_runtime_accessor_agent::reconcile_listeners(const std::vector<frp_runtime_visible_service_data>& services) {
    std::unordered_map<std::string, frp_runtime_visible_service_data> services_by_name;
    for (const auto& service : services) {
        services_by_name[service.service_name] = service;
    }

    std::unordered_set<std::string> desired_keys;
    for (const auto& listener_config : config_.listeners) {
        auto service_it = services_by_name.find(listener_config.service_name);
        if (service_it == services_by_name.end()) {
            FWARN("accessor listener service_name={} not found in current service directory", listener_config.service_name);
            continue;
        }
        const auto key = Fundamental::StringFormat("{}:{}:{}", listener_config.service_name, listener_config.listen_host,
                                                   listener_config.listen_port);
        desired_keys.insert(key);
        if (listeners_.count(key) > 0) {
            continue;
        }

        auto listener = std::make_shared<listener_runtime>(reconnect_timer_.get_executor(), listener_config.service_name,
                                                           listener_config.listen_host, listener_config.listen_port,
                                                           listener_config.nat_type);
        std::error_code ec;
        auto address = asio::ip::make_address(listener_config.listen_host, ec);
        if (ec) {
            FERR("accessor invalid listen_host={} service_name={} err={}", listener_config.listen_host,
                 listener_config.service_name, ec.message());
            continue;
        }
        listener->acceptor.open(address.is_v6() ? asio::ip::tcp::v6() : asio::ip::tcp::v4(), ec);
        if (ec) {
            FERR("accessor open listener failed service_name={} endpoint={}:{} err={}", listener_config.service_name,
                 listener_config.listen_host, listener_config.listen_port, ec.message());
            continue;
        }
        listener->acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
        listener->acceptor.bind(asio::ip::tcp::endpoint(address, listener_config.listen_port), ec);
        if (ec) {
            FERR("accessor bind listener failed service_name={} endpoint={}:{} err={}", listener_config.service_name,
                 listener_config.listen_host, listener_config.listen_port, ec.message());
            listener->acceptor.close(ec);
            continue;
        }
        listener->acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            FERR("accessor listen failed service_name={} endpoint={}:{} err={}", listener_config.service_name,
                 listener_config.listen_host, listener_config.listen_port, ec.message());
            listener->acceptor.close(ec);
            continue;
        }
        FWARN("accessor listener active service_name={} endpoint={}:{} nat_type={} provider_uuid={} provider_nat_type={} flow_runtime_pending=true",
              listener->service_name,
              listener->listen_host,
              listener->listen_port,
              static_cast<int>(listener->nat_type),
              service_it->second.provider_uuid,
              static_cast<int>(service_it->second.provider_nat_type));
        start_accept_loop(listener);
        listeners_[key] = std::move(listener);
    }

    for (auto it = listeners_.begin(); it != listeners_.end();) {
        if (desired_keys.count(it->first) > 0) {
            ++it;
            continue;
        }
        std::error_code ec;
        it->second->acceptor.close(ec);
        it = listeners_.erase(it);
    }
}

void frp_runtime_accessor_agent::start_accept_loop(const std::shared_ptr<listener_runtime>& listener) {
    listener->acceptor.async_accept(
        [this, self = shared_from_this(), listener](asio::error_code ec, asio::ip::tcp::socket socket) mutable {
            if (!reference_.is_valid()) return;
            if (!listener->acceptor.is_open()) return;
            if (!ec) {
                if (!channel_) {
                    std::error_code close_ec;
                    socket.shutdown(asio::ip::tcp::socket::shutdown_both, close_ec);
                    socket.close(close_ec);
                } else {
                    auto session = std::make_shared<accessor_session_context>(next_session_id_++, std::move(socket));
                    session->service_name = listener->service_name;
                    session->nat_type     = listener->nat_type;
                    pending_sessions_[session->session_id] = session;
                    bool use_p2p = (listener->nat_type != frp_runtime_nat_type_disabled) && startup_probe_succeeded_;
                    request_flow(session, use_p2p);
                }
            }
            start_accept_loop(listener);
        });
}

void frp_runtime_accessor_agent::request_flow(const std::shared_ptr<accessor_session_context>& session,
                                              bool enable_p2p_request) {
    if (!session || !channel_) return;
    session->awaiting_p2p = enable_p2p_request;
    if (!enable_p2p_request) {
        session->relay_retry_used = true;
    }
    frp_runtime_create_flow_request_data request;
    request.command      = frp_runtime_create_flow_request_command;
    request.service_name = session->service_name;
    request.transport    = enable_p2p_request ? frp_runtime_transport_p2p : frp_runtime_transport_tcp_relay;
    channel_->send_command(request);
}

void frp_runtime_accessor_agent::accessor_kcp_send(const std::shared_ptr<accessor_session_context>& session,
                                                   const char* data,
                                                   std::size_t size) {
    if (!session || !session->kcp || session->closed) return;
    // Encrypt application data before sending through KCP
    std::vector<std::uint8_t> plaintext(data, data + size);
    auto encrypted = frp_udp_encrypt(session->udp_send_key, plaintext);
    if (encrypted.empty()) return;
    FINFO("accessor kcp_send flow_id={} plaintext_size={} encrypted_size={}", session->flow_id, size, encrypted.size());
    if (ikcp_send(session->kcp.get(), reinterpret_cast<const char*>(encrypted.data()), static_cast<int>(encrypted.size())) < 0) return;
    ikcp_update(session->kcp.get(), frp_runtime_kcp_clock());
}

void frp_runtime_accessor_agent::schedule_accessor_kcp_update(const std::shared_ptr<accessor_session_context>& session) {
    if (!session || !session->kcp || session->closed) return;
    session->kcp_update_timer.expires_after(std::chrono::milliseconds(20));
    session->kcp_update_timer.async_wait([this, self = shared_from_this(), session](const asio::error_code& ec) {
        if (ec || !reference_.is_valid() || !session->kcp || session->closed) return;
        ikcp_update(session->kcp.get(), frp_runtime_kcp_clock());
        schedule_accessor_kcp_update(session);
    });
}

void frp_runtime_accessor_agent::start_accessor_p2p_read_loop(const std::shared_ptr<accessor_session_context>& session) {
    if (!session || !session->p2p_socket || session->closed) return;
    session->p2p_socket->async_receive_from(
        asio::buffer(session->p2p_read_buf.data(), session->p2p_read_buf.size()),
        session->p2p_recv_endpoint,
        [this, self = shared_from_this(), session](const asio::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (ec) {
                FINFO("accessor p2p recv error flow_id={} err={}", session->flow_id, ec.message());
                if (session->closed) return;
                start_accessor_p2p_read_loop(session);
                return;
            }
            if (session->closed || !session->kcp) return;

            FINFO("accessor p2p recv flow_id={} bytes={} from={}:{}", session->flow_id, bytes_read,
                  session->p2p_recv_endpoint.address().to_string(), session->p2p_recv_endpoint.port());

            // 1-byte: keepalive, discard
            if (bytes_read == 1) {
                start_accessor_p2p_read_loop(session);
                return;
            }

            // 6-byte: udp_punch probe packet (peer's packet reached us)
            if (bytes_read == 6) {
                if (session->punch_active && !session->p2p_success) {
                    std::uint32_t pkt_flow_id = 0;
                    std::memcpy(&pkt_flow_id, session->p2p_read_buf.data(), 4);
                    std::uint16_t peer_local_port = 0;
                    std::memcpy(&peer_local_port, session->p2p_read_buf.data() + 4, 2);
                    FINFO("accessor p2p recv punch probe flow_id={} pkt_flow_id={} peer_local_port={}", session->flow_id, pkt_flow_id, peer_local_port);
                    if (pkt_flow_id == session->flow_id) {
                        session->p2p_success = true;
                        session->punch_active = false;
                        session->punch_timer.cancel();
                        // Close all punch sockets (p2p_socket is the receiving one)
                        for (auto& sock : session->punch_sockets) {
                            if (sock) { std::error_code ec; sock->close(ec); }
                        }
                        session->punch_sockets.clear();
                        FINFO("udp_punch succeeded flow_id={} peer_local_port={}", session->flow_id, peer_local_port);
                        frp_runtime_p2p_connected_data connected;
                        connected.command = frp_runtime_p2p_connected_command;
                        connected.flow_id = session->flow_id;
                        connected.matched_peer_local_port = peer_local_port;
                        if (channel_) channel_->send_command(connected);
                    }
                }
                start_accessor_p2p_read_loop(session);
                return;
            }

            // < 24 bytes: discard
            if (bytes_read < 24) {
                FINFO("accessor p2p recv discarding short packet flow_id={} bytes={}", session->flow_id, bytes_read);
                start_accessor_p2p_read_loop(session);
                return;
            }

            // KCP input
            FINFO("accessor p2p recv KCP input flow_id={} bytes={}", session->flow_id, bytes_read);
            ikcp_input(session->kcp.get(), session->p2p_read_buf.data(), static_cast<long>(bytes_read));
            ikcp_update(session->kcp.get(), frp_runtime_kcp_clock());
            std::array<char, 16 * 1024> recv_buf {};
            while (true) {
                auto recv_size = ikcp_recv(session->kcp.get(), recv_buf.data(), static_cast<int>(recv_buf.size()));
                if (recv_size < 0) break;
                // Decrypt application data received from KCP
                std::vector<std::uint8_t> encrypted(recv_buf.data(), recv_buf.data() + recv_size);
                auto plaintext = frp_udp_decrypt(session->udp_recv_key, encrypted);
                if (!plaintext) {
                    FINFO("accessor flow {} failed to decrypt KCP payload size={}", session->flow_id, recv_size);
                    continue;
                }
                FINFO("accessor p2p KCP recv decrypted flow_id={} size={}", session->flow_id, plaintext->size());
                session->pending_writes.emplace_back(reinterpret_cast<const char*>(plaintext->data()), plaintext->size());
                handle_local_write_queue(session);
            }
            start_accessor_p2p_read_loop(session);
        });
}

void frp_runtime_accessor_agent::process_command(const frp_runtime_command_base& command, std::string payload) {
    if (!channel_) return;
    switch (command.command) {
    case frp_runtime_server_hello_command: {
        frp_runtime_server_hello_data hello;
        if (!Fundamental::io::from_json(payload, hello)) {
            channel_->release_obj();
            return;
        }
        FINFO("accessor recv server_hello nonce={}", hello.server_nonce);
        frp_runtime_auth_request_data request;
        request.command = frp_runtime_auth_request_command;
        request.digest  = frp_hmac_sha256_hex(config_.traffic_secret, hello.server_nonce);
        channel_->send_command(request);
        return;
    }
    case frp_runtime_auth_response_command: {
        frp_runtime_auth_response_data response;
        if (!Fundamental::io::from_json(payload, response) || !response.ok) {
            FERR("accessor auth failed uuid={} msg={}", uuid_, response.message);
            channel_->release_obj();
            return;
        }
        FINFO("accessor recv auth_response ok uuid={}", uuid_);
        frp_runtime_join_request_data join;
        join.command      = frp_runtime_join_request_command;
        join.role         = frp_runtime_accessor_role;
        join.uuid         = uuid_;
        join.register_key = config_.register_key;
        join.nat_type = (config_.nat_type != frp_runtime_nat_type_disabled && startup_probe_succeeded_)
                            ? config_.nat_type
                            : frp_runtime_nat_type_disabled;
        channel_->send_command(join);
        return;
    }
    case frp_runtime_join_response_command: {
        frp_runtime_join_response_data response;
        if (!Fundamental::io::from_json(payload, response) || !response.ok) {
            FERR("accessor join failed uuid={} msg={}", uuid_, response.message);
            channel_->release_obj();
            return;
        }
        FINFO("accessor recv join_response ok uuid={}", uuid_);
        frp_runtime_fetch_services_request_data request;
        request.command = frp_runtime_fetch_services_request_command;
        channel_->send_command(request);
        return;
    }
    case frp_runtime_fetch_services_response_command: {
        frp_runtime_fetch_services_response_data response;
        if (!Fundamental::io::from_json(payload, response) || !response.ok) {
            FERR("accessor fetch services failed uuid={} msg={}", uuid_, response.message);
            channel_->release_obj();
            return;
        }
        FINFO("accessor recv fetch_services_response ok uuid={} services={}", uuid_, response.services.size());
        reconcile_listeners(response.services);
        FINFO("accessor runtime ready uuid={} visible_services={} local_listeners={}", uuid_, response.services.size(),
              listeners_.size());
        return;
    }
    case frp_runtime_create_flow_response_command: {
        frp_runtime_create_flow_response_data response;
        if (!Fundamental::io::from_json(payload, response)) {
            channel_->release_obj();
            return;
        }
        if (response.result == frp_runtime_flow_result_rejected) {
            channel_->release_obj();
            return;
        }
        if (pending_sessions_.empty()) {
            return;
        }
        auto it = pending_sessions_.begin();
        auto session = it->second;
        pending_sessions_.erase(it);
        if (response.result == frp_runtime_flow_result_p2p_unavailable) {
            // Server can't do p2p, retry with tcp_relay
            if (!session->relay_retry_used) {
                pending_sessions_[session->session_id] = session;
                request_flow(session, false);
                FWARN("accessor session service_name={} retry relay after p2p_unavailable", session->service_name);
            } else {
                fail_session(session, "p2p_unavailable and relay already tried");
            }
            return;
        }
        if (response.result != frp_runtime_flow_result_accepted || response.flow_id == 0) {
            fail_session(session, response.message.empty() ? "create_flow failed" : response.message);
            return;
        }
        session->flow_id = response.flow_id;
        sessions_by_flow_id_[session->flow_id] = session;
        FINFO("accessor recv create_flow_response accepted flow_id={} service_name={} awaiting_p2p={}",
              session->flow_id, session->service_name, session->awaiting_p2p);
        if (session->awaiting_p2p) {
            start_flow_endpoint_probe(session);
            return;
        }
        session->data_channel = frp_runtime_data_client_channel::make_shared(reconnect_timer_.get_executor(),
                                                                             config_.public_server_host,
                                                                             std::to_string(config_.public_server_tcp_port),
                                                                             session->flow_id,
                                                                             uuid_);
        session->data_channel->enable_ssl(to_network_config(config_.ssl));
        session->data_channel->set_on_disconnected([this, self = shared_from_this(), session] {
            if (!reference_.is_valid() || session->closed) return;
            fail_session(session, "accessor data channel disconnected");
        });
        session->data_channel->set_on_data([this, self = shared_from_this(), session](std::string data) {
            if (!reference_.is_valid() || session->closed) return;
            session->pending_writes.push_back(std::move(data));
            handle_local_write_queue(session);
        });
        session->data_channel->start();
        return;
    }
    case frp_runtime_flow_ready_command: {
        frp_runtime_flow_ready_data ready;
        if (!Fundamental::io::from_json(payload, ready)) {
            channel_->release_obj();
            return;
        }
        auto it = sessions_by_flow_id_.find(ready.flow_id);
        if (it == sessions_by_flow_id_.end()) {
            return;
        }
        const auto transport = it->second->data_channel ? "tcp_relay" : "p2p";
        std::string local_endpoint = "unknown";
        std::string remote_endpoint = "unknown";
        if (it->second->data_channel) {
            local_endpoint  = it->second->data_channel->local_endpoint_string();
            remote_endpoint = it->second->data_channel->remote_endpoint_string();
        } else if (it->second->p2p_socket) {
            std::error_code endpoint_ec;
            local_endpoint  = format_runtime_endpoint(it->second->p2p_socket->local_endpoint(endpoint_ec));
            remote_endpoint = format_runtime_endpoint(it->second->p2p_peer_endpoint);
        }
        log_accessor_channel_established(
            it->second->session_id, it->second->flow_id, it->second->service_name,
            transport, local_endpoint, remote_endpoint);
        it->second->ready = true;
        start_local_read_loop(it->second);
        return;
    }
    case frp_runtime_flow_endpoint_ready_command: {
        frp_runtime_flow_endpoint_ready_data ep_ready;
        if (!Fundamental::io::from_json(payload, ep_ready)) {
            channel_->release_obj();
            return;
        }
        auto it = sessions_by_flow_id_.find(ep_ready.flow_id);
        if (it == sessions_by_flow_id_.end()) return;
        auto session = it->second;
        session->awaiting_endpoint_ready = false;
        session->endpoint_probe_timer.cancel();
        FINFO("accessor flow {} flow_endpoint_ready external={}:{}", session->flow_id, ep_ready.external_ip, ep_ready.external_port);
        return;
    }
    case frp_runtime_flow_p2p_peer_command: {
        frp_runtime_flow_p2p_peer_data peer;
        if (!Fundamental::io::from_json(payload, peer)) {
            channel_->release_obj();
            return;
        }
        auto it = sessions_by_flow_id_.find(peer.flow_id);
        if (it == sessions_by_flow_id_.end()) return;
        auto session = it->second;
        if (!session->p2p_socket || !session->kcp) return;
        std::error_code ec;
        session->p2p_peer_endpoint =
            asio::ip::udp::endpoint(asio::ip::make_address(peer.peer_host, ec), peer.peer_port);
        if (ec) {
            frp_runtime_flow_failed_data failed;
            failed.command = frp_runtime_flow_failed_command;
            failed.flow_id = session->flow_id;
            failed.reason  = frp_runtime_flow_failed_flow_endpoint_probe_timeout;
            failed.message = ec.message();
            reset_runtime_p2p_state(*session);
            channel_->send_command(failed);
            return;
        }
        session->peer_nat_type = peer.peer_nat_type;
        FINFO("accessor flow {} flow_p2p_peer peer={}:{} peer_nat_type={} starting udp_punch",
              session->flow_id, peer.peer_host, peer.peer_port, static_cast<int>(peer.peer_nat_type));
        start_udp_punch(session);
        return;
    }
    case frp_runtime_peer_p2p_connected_command: {
        frp_runtime_peer_p2p_connected_data connected;
        if (!Fundamental::io::from_json(payload, connected)) {
            channel_->release_obj();
            return;
        }
        auto it = sessions_by_flow_id_.find(connected.flow_id);
        if (it == sessions_by_flow_id_.end()) return;
        auto session = it->second;
        if (session->p2p_success) return;
        // Find the punch socket matching matched_peer_local_port and promote it to p2p_socket
        if (connected.matched_peer_local_port != 0 && !session->punch_sockets.empty()) {
            for (auto& sock : session->punch_sockets) {
                if (!sock) continue;
                std::error_code ec;
                auto lp = sock->local_endpoint(ec).port();
                if (!ec && lp == connected.matched_peer_local_port) {
                    session->p2p_socket = std::move(sock);
                    break;
                }
            }
        }
        for (auto& sock : session->punch_sockets) {
            if (sock) { std::error_code ec; sock->close(ec); }
        }
        session->punch_sockets.clear();
        session->punch_active = false;
        session->punch_timer.cancel();
        session->p2p_success = true;
        if (session->p2p_socket) {
            session->p2p_send_context.socket        = &session->p2p_socket;
            session->p2p_send_context.peer_endpoint = &session->p2p_peer_endpoint;
            start_accessor_p2p_read_loop(session);
        }
        FINFO("accessor flow {} peer_p2p_connected p2p_success=true matched_port={}", session->flow_id, connected.matched_peer_local_port);
        return;
    }
    case frp_runtime_flow_data_command: {
        return;
    }
    case frp_runtime_flow_closed_command: {
        frp_runtime_flow_closed_data closed;
        if (!Fundamental::io::from_json(payload, closed)) {
            channel_->release_obj();
            return;
        }
        auto it = sessions_by_flow_id_.find(closed.flow_id);
        if (it != sessions_by_flow_id_.end()) {
            auto session = it->second;
            session->peer_closed = true;
            if (session->data_channel) {
                session->data_channel->release_obj();
                session->data_channel = nullptr;
            }
            if (!session->writing && session->pending_writes.empty()) {
                fail_session(session, "peer closed");
            }
        }
        return;
    }
    case frp_runtime_flow_failed_command: {
        frp_runtime_flow_failed_data failed;
        if (!Fundamental::io::from_json(payload, failed)) {
            channel_->release_obj();
            return;
        }
        auto it = sessions_by_flow_id_.find(failed.flow_id);
        if (it != sessions_by_flow_id_.end()) {
            auto session = it->second;
            sessions_by_flow_id_.erase(it);
            session->flow_id = 0;
            if (session->data_channel) {
                session->data_channel->release_obj();
                session->data_channel = nullptr;
            }
            if (!session->closed && !session->ready && session->nat_type != frp_runtime_nat_type_disabled &&
                session->awaiting_p2p && !session->relay_retry_used &&
                (failed.reason == frp_runtime_flow_failed_flow_endpoint_probe_timeout ||
                 failed.reason == frp_runtime_flow_failed_punch_timeout)) {
                pending_sessions_[session->session_id] = session;
                request_flow(session, false);
                FWARN("accessor session service_name={} retry relay after p2p failure", session->service_name);
                return;
            }
            fail_session(session, failed.message.empty() ? "flow failed" : failed.message);
        }
        return;
    }
    case frp_runtime_ping_response_command: return;
    default: return;
    }
}

void frp_runtime_accessor_agent::start_flow_endpoint_probe(const std::shared_ptr<accessor_session_context>& session) {
    if (!session || !channel_ || config_.public_server_udp_port == 0) {
        fail_session(session, "udp p2p is not available");
        return;
    }

    // Derive UDP encryption key for this flow (shared traffic key)
    session->udp_send_key = frp_derive_traffic_key(config_.traffic_secret);
    session->udp_recv_key = frp_derive_traffic_key(config_.traffic_secret);
    if (session->udp_send_key.empty() || session->udp_recv_key.empty()) {
        fail_session(session, "failed to derive UDP encryption keys");
        return;
    }

    session->p2p_socket = std::make_unique<asio::ip::udp::socket>(reconnect_timer_.get_executor());
    std::error_code ec;
    protocal_helper::udp_bind_endpoint(*session->p2p_socket, 0);
    session->p2p_send_context.socket        = &session->p2p_socket;
    session->p2p_send_context.peer_endpoint = &session->p2p_peer_endpoint;
    session->kcp = std::unique_ptr<ikcpcb, kcp_releaser>(ikcp_create(session->flow_id, &session->p2p_send_context));
    ikcp_setoutput(session->kcp.get(), frp_runtime_kcp_output);
    session->kcp.get()->stream = 0;
    ikcp_wndsize(session->kcp.get(), 256, 256);
    ikcp_nodelay(session->kcp.get(), 1, 20, 2, 1);
    schedule_accessor_kcp_update(session);
    start_accessor_p2p_read_loop(session);

    session->awaiting_endpoint_ready = true;
    session->endpoint_probe_attempts = 0;
    auto local_port = session->p2p_socket->local_endpoint(ec).port();
    FINFO("accessor start_flow_endpoint_probe flow_id={} local_port={}", session->flow_id, local_port);

    auto fn = std::make_shared<std::function<void()>>();
    *fn = [this, self = shared_from_this(), session, local_port, fn]() mutable {
        if (!reference_.is_valid() || !channel_ || session->closed || !session->awaiting_endpoint_ready) return;
        if (session->endpoint_probe_attempts >= kMaxEndpointProbeAttempts) {
            fail_session(session, "flow_endpoint_probe timeout");
            return;
        }
        frp_runtime_p2p_probe_data probe;
        probe.command    = frp_runtime_p2p_probe_command;
        probe.flow_id    = session->flow_id;
        probe.uuid       = uuid_;
        probe.local_ip   = config_.local_ip;
        probe.local_port = local_port;
        auto payload = Fundamental::io::to_json(probe);
        auto encrypted = frp_udp_encrypt_string(session->udp_send_key, payload);
        if (encrypted.empty()) return;
        auto server_endpoint = resolve_udp_endpoint(reconnect_timer_.get_executor(), config_.public_server_host,
                                                    config_.public_server_udp_port);
        if (!server_endpoint) return;
        session->endpoint_probe_attempts++;
        FINFO("accessor flow_endpoint_probe attempt={} flow_id={} local_port={} server={}:{} probe_size={}",
              session->endpoint_probe_attempts, session->flow_id, local_port,
              config_.public_server_host, config_.public_server_udp_port, encrypted.size());
        auto enc_ptr = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted));
        session->p2p_socket->async_send_to(asio::buffer(*enc_ptr), *server_endpoint,
                                           [enc_ptr](const std::error_code&, std::size_t) {});
        session->endpoint_probe_timer.expires_after(std::chrono::milliseconds(200));
        session->endpoint_probe_timer.async_wait([fn](const std::error_code& ec) mutable {
            if (ec) return;
            (*fn)();
        });
    };
    (*fn)();
}

void frp_runtime_accessor_agent::start_udp_punch(const std::shared_ptr<accessor_session_context>& session) {
    if (!session || !channel_ || session->closed || session->p2p_success) return;

    const bool i_am_symmetric = (session->nat_type == frp_runtime_nat_type_symmetric);
    const bool peer_is_symmetric = (session->peer_nat_type == frp_runtime_nat_type_symmetric);

    // Determine my role: if I am symmetric, I send from multiple sockets to peer's known port.
    // If peer is symmetric (and I am full), I scan peer's port range with 1 socket.
    // If both full: I scan with 1 socket (same as full-side logic).

    std::mt19937 rng(std::random_device{}());

    if (i_am_symmetric) {
        // --- symmetric side: bind 32 sockets near local port xxx ---
        std::error_code ec;
        std::uint16_t xxx = session->p2p_socket ? session->p2p_socket->local_endpoint(ec).port() : 0;
        if (ec || xxx == 0) {
            // fallback: use any port
            xxx = 40000;
        }
        session->punch_sockets.clear();
        std::set<std::uint16_t> used;
        // Always include xxx itself
        {
            auto sock = std::make_unique<asio::ip::udp::socket>(session->local_socket.get_executor());
            if (!protocal_helper::udp_bind_endpoint(*sock, xxx)) {
                session->punch_sockets.push_back(std::move(sock));
                used.insert(xxx);
            }
        }
        // Pick 31 more from [xxx-128, xxx+128] ∩ [1024, 65535]
        std::uniform_int_distribution<int> dist(
            std::max(1024, static_cast<int>(xxx) - 128),
            std::min(65535, static_cast<int>(xxx) + 128));
        int attempts = 0;
        while (static_cast<int>(session->punch_sockets.size()) < kPunchSocketCount && attempts < 2000) {
            attempts++;
            auto port = static_cast<std::uint16_t>(dist(rng));
            if (used.count(port)) continue;
            auto sock = std::make_unique<asio::ip::udp::socket>(session->local_socket.get_executor());
            if (protocal_helper::udp_bind_endpoint(*sock, port)) continue;
            used.insert(port);
            session->punch_sockets.push_back(std::move(sock));
        }
        FINFO("udp_punch symmetric accessor flow_id={} sockets={} target={}:{}",
              session->flow_id, session->punch_sockets.size(),
              session->p2p_peer_endpoint.address().to_string(), session->p2p_peer_endpoint.port());
    } else {
        // --- full side: 1 socket, scan peer's port range ---
        if (!session->p2p_socket) {
            FERR("udp_punch full accessor flow_id={} no p2p_socket", session->flow_id);
            return;
        }
        session->punch_sockets.clear();
        // We reuse p2p_socket for sending; no extra sockets needed
    }

    session->punch_active = true;
    session->punch_round  = 0;

    auto do_round = std::make_shared<std::function<void()>>();
    *do_round = [this, self = shared_from_this(), session, do_round, i_am_symmetric, peer_is_symmetric]() mutable {
        if (!reference_.is_valid() || session->closed || !session->punch_active || session->p2p_success) return;

        const int max_rounds = kPunchMaxRounds + (i_am_symmetric ? 0 : 1); // full side has round 0 extra
        if (session->punch_round >= max_rounds) {
            frp_runtime_flow_failed_data failed;
            failed.command = frp_runtime_flow_failed_command;
            failed.flow_id = session->flow_id;
            failed.reason  = frp_runtime_flow_failed_punch_timeout;
            failed.message = "udp_punch all rounds exhausted";
            reset_runtime_p2p_state(*session);
            sessions_by_flow_id_.erase(session->flow_id);
            if (channel_) channel_->send_command(failed);
            return;
        }

        // Build list of target endpoints for this round
        std::vector<asio::ip::udp::endpoint> targets;
        if (i_am_symmetric) {
            // symmetric: all sockets send to peer's known endpoint
            targets.push_back(session->p2p_peer_endpoint);
        } else {
            // full side: pick ports to scan
            std::mt19937 rng2(std::random_device{}());
            const std::uint16_t peer_base = session->p2p_peer_endpoint.port();
            const auto peer_addr = session->p2p_peer_endpoint.address();
            if (session->punch_round == 0) {
                // Round 0: [peer_base, peer_base+63] in random order
                std::vector<std::uint16_t> ports;
                for (int i = 0; i < kPunchScanRound0; ++i) {
                    std::uint16_t p = static_cast<std::uint16_t>(peer_base + i);
                    if (p < 23) continue;
                    ports.push_back(p);
                    session->punch_scanned_ports.insert(p);
                }
                std::shuffle(ports.begin(), ports.end(), rng2);
                for (auto p : ports) targets.emplace_back(peer_addr, p);
            } else {
                // Rounds 1+: 128 random ports from [23,65535] not already scanned
                std::uniform_int_distribution<int> dist(23, 65535);
                int picked = 0, tries = 0;
                while (picked < kPunchScanPerRound && tries < 100000) {
                    tries++;
                    auto p = static_cast<std::uint16_t>(dist(rng2));
                    if (session->punch_scanned_ports.count(p)) continue;
                    session->punch_scanned_ports.insert(p);
                    targets.emplace_back(peer_addr, p);
                    picked++;
                }
            }
        }

        // Send probes with retransmit
        // For each target, send kPunchRetransmitCount times at kPunchRetransmitMs intervals
        // We use a shared counter to chain retransmits
        auto retransmit_index = std::make_shared<int>(0);
        auto do_retransmit = std::make_shared<std::function<void()>>();
        *do_retransmit = [this, self, session, do_round, do_retransmit, retransmit_index,
                          targets = std::move(targets), i_am_symmetric]() mutable {
            if (!reference_.is_valid() || session->closed || !session->punch_active || session->p2p_success) return;
            if (*retransmit_index >= kPunchRetransmitCount) {
                // Done with this round, schedule next round
                session->punch_round++;
                session->punch_timer.expires_after(std::chrono::milliseconds(kPunchRoundMs));
                session->punch_timer.async_wait([do_round](const std::error_code& ec) mutable {
                    if (ec) return;
                    (*do_round)();
                });
                return;
            }
            (*retransmit_index)++;

            // Build 6-byte probe packet
            std::uint32_t fid = session->flow_id;
            if (i_am_symmetric) {
                // Each punch socket sends with its own local port
                for (auto& sock : session->punch_sockets) {
                    if (!sock) continue;
                    std::error_code ec;
                    std::uint16_t local_port = sock->local_endpoint(ec).port();
                    if (ec) continue;
                    auto pkt = std::make_shared<std::array<std::uint8_t, 6>>();
                    std::memcpy(pkt->data(), &fid, 4);
                    std::memcpy(pkt->data() + 4, &local_port, 2);
                    sock->async_send_to(asio::buffer(*pkt), targets[0],
                                        [pkt](const std::error_code&, std::size_t) {});
                }
            } else {
                // full side: p2p_socket sends to each target
                std::uint16_t local_port = 0;
                {
                    std::error_code ec;
                    local_port = session->p2p_socket->local_endpoint(ec).port();
                }
                for (const auto& tgt : targets) {
                    auto pkt = std::make_shared<std::array<std::uint8_t, 6>>();
                    std::memcpy(pkt->data(), &fid, 4);
                    std::memcpy(pkt->data() + 4, &local_port, 2);
                    session->p2p_socket->async_send_to(asio::buffer(*pkt), tgt,
                                                       [pkt](const std::error_code&, std::size_t) {});
                }
            }

            session->punch_timer.expires_after(std::chrono::milliseconds(kPunchRetransmitMs));
            session->punch_timer.async_wait([do_retransmit](const std::error_code& ec) mutable {
                if (ec) return;
                (*do_retransmit)();
            });
        };
        (*do_retransmit)();
    };

    // Start read loop on p2p_socket to detect incoming punch packets
    // (read loop already started in start_flow_endpoint_probe, but we need it active)
    // For symmetric side, also start read loops on punch_sockets
    if (i_am_symmetric) {
        for (auto& sock : session->punch_sockets) {
            if (!sock) continue;
            // Start a simple receive loop on each punch socket
            auto punch_sock_ptr = sock.get();
            auto recv_buf = std::make_shared<std::array<char, 64>>();
            auto recv_ep  = std::make_shared<asio::ip::udp::endpoint>();
            std::function<void()> do_recv;
            do_recv = [this, self = shared_from_this(), session, punch_sock_ptr, recv_buf, recv_ep, do_recv]() mutable {
                if (!reference_.is_valid() || session->closed || !session->punch_active || session->p2p_success) return;
                punch_sock_ptr->async_receive_from(
                    asio::buffer(recv_buf->data(), recv_buf->size()), *recv_ep,
                    [this, self, session, punch_sock_ptr, recv_buf, recv_ep, do_recv]
                    (const std::error_code& ec, std::size_t bytes) mutable {
                        if (!reference_.is_valid() || session->closed) return;
                        if (!ec && bytes == 6 && session->punch_active && !session->p2p_success) {
                            std::uint32_t pkt_fid = 0;
                            std::memcpy(&pkt_fid, recv_buf->data(), 4);
                            std::uint16_t peer_lp = 0;
                            std::memcpy(&peer_lp, recv_buf->data() + 4, 2);
                            if (pkt_fid == session->flow_id) {
                                // Promote this socket
                                for (auto& s : session->punch_sockets) {
                                    if (s.get() == punch_sock_ptr) {
                                        session->p2p_socket = std::move(s);
                                        break;
                                    }
                                }
                                for (auto& s : session->punch_sockets) {
                                    if (s) { std::error_code ce; s->close(ce); }
                                }
                                session->punch_sockets.clear();
                                session->punch_active = false;
                                session->punch_timer.cancel();
                                session->p2p_success = true;
                                session->p2p_send_context.socket        = &session->p2p_socket;
                                session->p2p_send_context.peer_endpoint = &session->p2p_peer_endpoint;
                                session->p2p_peer_endpoint = *recv_ep;
                                start_accessor_p2p_read_loop(session);
                                FINFO("udp_punch succeeded (symmetric recv) flow_id={} peer_local_port={}", session->flow_id, peer_lp);
                                frp_runtime_p2p_connected_data connected;
                                connected.command = frp_runtime_p2p_connected_command;
                                connected.flow_id = session->flow_id;
                                connected.matched_peer_local_port = peer_lp;
                                if (channel_) channel_->send_command(connected);
                                return;
                            }
                        }
                        if (!session->closed && session->punch_active && !session->p2p_success) do_recv();
                    });
            };
            do_recv();
        }
    }

    (*do_round)();
}

void frp_runtime_accessor_agent::start_local_read_loop(const std::shared_ptr<accessor_session_context>& session) {
    if (!session || !session->ready || !channel_) return;
    session->local_socket.async_read_some(
        network_read_buffer_t(session->read_buf.data(), session->read_buf.size()),
        [this, self = shared_from_this(), session](const asio::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (ec) {
                if (session->closed) return;
                if (channel_) {
                    frp_runtime_flow_closed_data closed;
                    closed.command = frp_runtime_flow_closed_command;
                    closed.flow_id = session->flow_id;
                    channel_->send_command(closed);
                }
                fail_session(session, ec.message());
                return;
            }
            if (session->data_channel) {
                session->data_channel->send_bytes(std::make_shared<std::string>(session->read_buf.data(), bytes_read));
            } else if (session->kcp) {
                accessor_kcp_send(session, session->read_buf.data(), bytes_read);
            }
            if (session->peer_closed) {
                return;
            }
            start_local_read_loop(session);
        });
}

void frp_runtime_accessor_agent::handle_local_write_queue(const std::shared_ptr<accessor_session_context>& session) {
    if (!session || session->writing || session->pending_writes.empty()) return;
    session->writing = true;
    auto& current = session->pending_writes.front();
    asio::async_write(session->local_socket,
                      asio::buffer(current.data(), current.size()),
                      [this, self = shared_from_this(), session](const asio::error_code& ec, std::size_t) {
                          session->writing = false;
                          if (!reference_.is_valid()) return;
                          if (ec) {
                              fail_session(session, ec.message());
                              return;
                          }
                          session->pending_writes.pop_front();
                          if (session->pending_writes.empty() && session->peer_closed) {
                              fail_session(session, "peer closed");
                              return;
                          }
                          handle_local_write_queue(session);
                      });
}

void frp_runtime_accessor_agent::fail_session(const std::shared_ptr<accessor_session_context>& session,
                                              const std::string& reason) {
    if (!session) return;
    if (session->closed) return;
    session->closed = true;
    session->peer_closed = true;
    FWARN("accessor session flow_id={} service_name={} closed reason={}", session->flow_id, session->service_name, reason);
    reset_runtime_p2p_state(*session);
    if (session->data_channel) {
        session->data_channel->release_obj();
        session->data_channel = nullptr;
    }
    std::error_code ec;
    session->local_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    session->local_socket.close(ec);
    if (session->flow_id != 0) {
        sessions_by_flow_id_.erase(session->flow_id);
    } else {
        pending_sessions_.erase(session->session_id);
    }
}

} // namespace network::proxy
