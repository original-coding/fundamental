#include "frp_runtime_client.hpp"

#include "frp_proxy_data_channel.hpp"
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

} // namespace

struct frp_runtime_provider_agent::provider_flow_runtime {
    std::uint32_t flow_id = 0;
    std::string service_name;
    std::string accessor_uuid;
    std::shared_ptr<frp_proxy_data_channel> data_channel;
    asio::ip::tcp::socket backend_socket;
    asio::ip::tcp::resolver resolver;
    std::array<char, 16 * 1024> read_buf {};
    std::deque<std::string> pending_writes;
    bool writing = false;
    bool backend_connected = false;
    bool backend_disconnected = false;
    bool closed = false;

    explicit provider_flow_runtime(const asio::any_io_executor& executor) :
    backend_socket(executor), resolver(executor) {
    }
};

struct frp_runtime_accessor_agent::accessor_session_context {
    std::uint64_t session_id = 0;
    std::uint32_t flow_id = 0;
    std::string service_name;
    std::uint8_t provider_nat_type = frp_runtime_nat_type_disabled;
    asio::ip::tcp::socket local_socket;
    std::shared_ptr<frp_proxy_data_channel> data_channel;
    std::array<char, 16 * 1024> read_buf {};
    std::deque<std::string> pending_writes;
    bool writing = false;
    bool ready = false;
    bool peer_closed = false;
    bool closed = false;

    explicit accessor_session_context(std::uint64_t session_id, asio::ip::tcp::socket&& socket) :
    session_id(session_id),
    local_socket(std::move(socket)) {
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
                       std::function<void(frp_runtime_nat_type)> on_done) {
    if (udp_ports.empty()) {
        on_done(frp_runtime_nat_type_disabled);
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
        std::function<void(frp_runtime_nat_type)> on_done;
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
        state->on_done(frp_runtime_nat_type_disabled);
        return;
    }
    state->socket->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0), ec);
    if (ec) {
        FINFO("startup_probe socket bind failed err={}", ec.message());
        state->on_done(frp_runtime_nat_type_disabled);
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
                                frp_runtime_nat_type detected;
                                if (state->result1.empty()) {
                                    detected = frp_runtime_nat_type_disabled;
                                } else if (state->udp_ports.size() == 1 || state->result1 == state->result2) {
                                    detected = frp_runtime_nat_type_full;
                                } else {
                                    detected = frp_runtime_nat_type_symmetric;
                                }
                                FINFO("startup_probe probe_result_1={} probe_result_2={} detected_nat_type={}",
                                      state->result1, state->result2, static_cast<int>(detected));
                                state->on_done(detected);
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
            state->on_done(frp_runtime_nat_type_disabled);
            return;
        }
        auto server_endpoint = resolve_udp_endpoint(state->executor, state->public_server_host,
                                                    state->udp_ports[state->port_index]);
        if (!server_endpoint) {
            state->done = true;
            state->on_done(frp_runtime_nat_type_disabled);
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
        auto encrypted = frp_kcp_encrypt_string(state->traffic_key, payload);
        if (encrypted.empty()) {
            state->done = true;
            state->on_done(frp_runtime_nat_type_disabled);
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
                          [this, self = shared_from_this()](frp_runtime_nat_type detected) {
                              if (!reference_.is_valid()) return;
                              probed_nat_type_ = detected;
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
        FERR("provider signal disconnected uuid={}, clear flows", uuid_);
        for (auto& [_, flow] : flows_) {
            flow->closed = true; // prevent on_disconnected_ from erasing during iteration
            if (flow->data_channel) {
                flow->data_channel->release_obj();
                flow->data_channel = nullptr;
            }
            std::error_code ec;
            flow->backend_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            flow->backend_socket.close(ec);
        }
        flows_.clear();
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
                    // Initiate p2p upgrade if capable
                    if (probed_nat_type_ != frp_runtime_nat_type_disabled &&
                        config_.public_server_udp_port != 0 && flow->data_channel) {
                        frp_runtime_p2p_upgrade_request_data upgrade_req;
                        upgrade_req.command = frp_runtime_p2p_upgrade_request_command;
                        upgrade_req.flow_id = flow->flow_id;
                        channel_->send_command(upgrade_req);
                        flow->data_channel->start_p2p_upgrade();
                    }
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
        join.nat_type = (config_.nat_type != frp_runtime_nat_type_disabled &&
                         probed_nat_type_ != frp_runtime_nat_type_disabled)
                            ? probed_nat_type_
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
        FINFO("provider recv prepare_flow flow_id={} service_name={} accessor_uuid={}",
              request.flow_id, request.service_name, request.accessor_uuid);
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

        // Always relay-first: create frp_proxy_data_channel
        flow->data_channel = frp_proxy_data_channel::make_shared(
            reconnect_timer_.get_executor(),
            config_.public_server_host,
            std::to_string(config_.public_server_tcp_port),
            flow->flow_id,
            uuid_,
            config_.traffic_secret,
            config_.public_server_host,
            config_.public_server_udp_port,
            static_cast<std::uint8_t>(probed_nat_type_));
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
        flow->data_channel->set_on_p2p_upgraded([this, self = shared_from_this(), flow] {
            if (!reference_.is_valid()) return;
            FINFO("provider flow {} p2p upgrade complete", flow->flow_id);
            log_provider_channel_established(
                flow->flow_id, flow->service_name, "p2p",
                flow->data_channel ? flow->data_channel->local_p2p_endpoint() : "?",
                "peer");
        });
        flow->data_channel->set_on_p2p_upgrade_failed([flow] {
            FINFO("provider flow {} p2p upgrade failed, relay continues", flow->flow_id);
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
        if (flow->closed) return;
        if (flow->data_channel) {
            log_provider_channel_established(
                flow->flow_id, flow->service_name, "tcp_relay",
                flow->data_channel->local_relay_endpoint(),
                flow->data_channel->remote_relay_endpoint());
        }
        // Relay is ready -- connect backend
        start_provider_backend_connect(flow);
        return;
    }
    case frp_runtime_flow_ready_command: {
        // Provider receives flow_ready after sending it -- this is the accessor's flow_ready
        // In new architecture, provider sends flow_ready after backend connects.
        // This case handles the server echoing flow_ready to provider (not used in new flow).
        return;
    }
    case frp_runtime_flow_endpoint_ready_command: {
        // Handled internally by frp_proxy_data_channel -- ignore here
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
        if (!flow->data_channel) return;
        FINFO("provider flow {} flow_p2p_peer peer={}:{} peer_nat_type={}",
              flow->flow_id, peer.peer_host, peer.peer_port, static_cast<int>(peer.peer_nat_type));
        flow->data_channel->set_p2p_peer(peer.peer_host, peer.peer_port, peer.peer_nat_type);
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

void frp_runtime_provider_agent::start_backend_read_loop(const std::shared_ptr<provider_flow_runtime>& flow) {
    if (!flow || !flow->backend_connected || !channel_ || !flow->data_channel) return;
    flow->backend_socket.async_read_some(
        network_read_buffer_t(flow->read_buf.data(), flow->read_buf.size()),
        [this, self = shared_from_this(), flow](const asio::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid() || !channel_) return;
            if (ec) {
                if (flow->closed) return;
                flow->closed = true;
                frp_runtime_flow_closed_data closed;
                closed.command = frp_runtime_flow_closed_command;
                closed.flow_id = flow->flow_id;
                if (flow->data_channel) {
                    flow->data_channel->release_obj();
                    flow->data_channel = nullptr;
                }
                flows_.erase(flow->flow_id);
                channel_->send_command(closed);
                return;
            }
            if (flow->data_channel) {
                flow->data_channel->send_bytes(flow->read_buf.data(), bytes_read);
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
                          [this, self = shared_from_this()](frp_runtime_nat_type detected) {
                              if (!reference_.is_valid()) return;
                              probed_nat_type_ = detected;
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
                                                           listener_config.listen_host, listener_config.listen_port);
        listener->provider_nat_type = service_it->second.provider_nat_type;
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
        FWARN("accessor listener active service_name={} endpoint={}:{} provider_uuid={} provider_nat_type={} flow_runtime_pending=true",
              listener->service_name,
              listener->listen_host,
              listener->listen_port,
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
                    session->provider_nat_type = listener->provider_nat_type;
                    pending_sessions_[session->session_id] = session;
                    request_flow(session);
                }
            }
            start_accept_loop(listener);
        });
}

void frp_runtime_accessor_agent::request_flow(const std::shared_ptr<accessor_session_context>& session) {
    if (!session || !channel_) return;
    frp_runtime_create_flow_request_data request;
    request.command      = frp_runtime_create_flow_request_command;
    request.service_name = session->service_name;
    request.transport    = frp_runtime_transport_tcp_relay;
    channel_->send_command(request);
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
        join.nat_type = (config_.nat_type != frp_runtime_nat_type_disabled &&
                         probed_nat_type_ != frp_runtime_nat_type_disabled)
                            ? probed_nat_type_
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
        if (response.result != frp_runtime_flow_result_accepted || response.flow_id == 0) {
            fail_session(session, response.message.empty() ? "create_flow failed" : response.message);
            return;
        }
        session->flow_id = response.flow_id;
        sessions_by_flow_id_[session->flow_id] = session;
        FINFO("accessor recv create_flow_response accepted flow_id={} service_name={}",
              session->flow_id, session->service_name);

        // Always relay-first: create frp_proxy_data_channel
        session->data_channel = frp_proxy_data_channel::make_shared(
            session->local_socket.get_executor(),
            config_.public_server_host,
            std::to_string(config_.public_server_tcp_port),
            session->flow_id,
            uuid_,
            config_.traffic_secret,
            config_.public_server_host,
            config_.public_server_udp_port,
            static_cast<std::uint8_t>(probed_nat_type_));
        session->data_channel->enable_ssl(to_network_config(config_.ssl));
        session->data_channel->set_on_connected([this, self = shared_from_this(), session] {
            if (!reference_.is_valid() || session->closed) return;
            // Relay connected -- wait for flow_ready from server
        });
        session->data_channel->set_on_disconnected([this, self = shared_from_this(), session] {
            if (!reference_.is_valid() || session->closed) return;
            fail_session(session, "accessor data channel disconnected");
        });
        session->data_channel->set_on_data([this, self = shared_from_this(), session](std::string data) {
            if (!reference_.is_valid() || session->closed) return;
            session->pending_writes.push_back(std::move(data));
            handle_local_write_queue(session);
        });
        session->data_channel->set_on_p2p_upgraded([this, self = shared_from_this(), session] {
            if (!reference_.is_valid()) return;
            FINFO("accessor session {} flow {} p2p upgrade complete", session->session_id, session->flow_id);
            log_accessor_channel_established(
                session->session_id, session->flow_id, session->service_name, "p2p",
                session->data_channel ? session->data_channel->local_p2p_endpoint() : "?",
                "peer");
        });
        session->data_channel->set_on_p2p_upgrade_failed([session] {
            FINFO("accessor session {} flow {} p2p upgrade failed, relay continues",
                  session->session_id, session->flow_id);
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
        if (it == sessions_by_flow_id_.end()) return;
        auto session = it->second;
        if (session->closed) return;
        log_accessor_channel_established(
            session->session_id, session->flow_id, session->service_name, "tcp_relay",
            session->data_channel ? session->data_channel->local_relay_endpoint() : "?",
            session->data_channel ? session->data_channel->remote_relay_endpoint() : "?");
        session->ready = true;
        // Initiate p2p upgrade if both sides are capable
        if (probed_nat_type_ != frp_runtime_nat_type_disabled &&
            session->provider_nat_type != frp_runtime_nat_type_disabled &&
            config_.public_server_udp_port != 0 && session->data_channel) {
            frp_runtime_p2p_upgrade_request_data upgrade_req;
            upgrade_req.command = frp_runtime_p2p_upgrade_request_command;
            upgrade_req.flow_id = session->flow_id;
            channel_->send_command(upgrade_req);
            session->data_channel->start_p2p_upgrade();
        }
        start_local_read_loop(session);
        return;
    }
    case frp_runtime_flow_endpoint_ready_command: {
        // Handled internally by frp_proxy_data_channel -- ignore here
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
        if (!session->data_channel) return;
        FINFO("accessor session {} flow {} flow_p2p_peer peer={}:{} peer_nat_type={}",
              session->session_id, session->flow_id, peer.peer_host, peer.peer_port,
              static_cast<int>(peer.peer_nat_type));
        session->data_channel->set_p2p_peer(peer.peer_host, peer.peer_port, peer.peer_nat_type);
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
            fail_session(session, failed.message.empty() ? "flow failed" : failed.message);
        }
        return;
    }
    case frp_runtime_ping_response_command: return;
    default: return;
    }
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
                session->data_channel->send_bytes(session->read_buf.data(), bytes_read);
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
    FWARN("accessor session flow_id={} service_name={} closed reason={}",
          session->flow_id, session->service_name, reason);

    // Keep the data_channel alive briefly so that P2P upgrade can complete
    // even after the local client has disconnected. The UDP socket must stay
    // open to receive punch packets from the peer.
    // Schedule deferred cleanup via a timer.
    if (session->data_channel) {
        auto timer = std::make_shared<asio::steady_timer>(session->local_socket.get_executor());
        timer->expires_after(std::chrono::seconds(5));
        auto flow_id = session->flow_id;
        timer->async_wait([this, self = shared_from_this(), flow_id, timer](const std::error_code&) {
            auto it = sessions_by_flow_id_.find(flow_id);
            if (it != sessions_by_flow_id_.end()) {
                if (it->second->data_channel) {
                    it->second->data_channel->release_obj();
                    it->second->data_channel = nullptr;
                }
                sessions_by_flow_id_.erase(it);
            }
            });
    }

    std::error_code ec;
    session->local_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    session->local_socket.close(ec);
    // NOTE: session stays in sessions_by_flow_id_ until the deferred cleanup fires,
    // so that flow_p2p_peer / peer_p2p_connected can still be delivered.
}

} // namespace network::proxy
