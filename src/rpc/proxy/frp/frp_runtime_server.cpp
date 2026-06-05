#include "frp_runtime_server.hpp"

#include "frp_runtime_common.hpp"
#include "frp_kcp_crypto.hpp"

namespace network::proxy
{

frp_runtime_public_server::frp_runtime_public_server(frp_public_server_config config) :
config_(std::move(config)), acceptor_(io_context_pool::Instance().get_io_context()) {
    protocal_helper::init_acceptor(acceptor_, config_.listen_tcp_port);
    allowed_register_keys_.insert(config_.allowed_register_keys.begin(), config_.allowed_register_keys.end());
    start_udp_servers();
    configure_ssl();
}

void frp_runtime_public_server::start() {
    bool expected = false;
    if (!has_started_.compare_exchange_strong(expected, true)) return;
    FINFO("start frp runtime public server on {}:{}", acceptor_.local_endpoint().address().to_string(),
          acceptor_.local_endpoint().port());
    do_accept();
    for (std::size_t i = 0; i < udp_servers_.size(); ++i) {
        start_udp_receive(i);
    }
}

void frp_runtime_public_server::start_udp_servers() {
    udp_servers_.clear();
    if (config_.listen_udp_port == 0) return;
    for (std::uint16_t port : { config_.listen_udp_port, static_cast<std::uint16_t>(config_.listen_udp_port + 1) }) {
        auto server = std::make_shared<udp_server_state>(io_context_pool::Instance().get_io_context().get_executor());
        auto ec     = protocal_helper::udp_bind_endpoint(server->socket, port);
        if (ec) {
            throw std::invalid_argument(Fundamental::StringFormat("bind udp port:{} failed err:{}", port, ec.message()));
        }
        FINFO("start_udp_servers bound udp port={}", port);
        udp_servers_.push_back(server);
    }
}

void frp_runtime_public_server::start_udp_receive(std::size_t index) {
    if (index >= udp_servers_.size()) return;
    auto server = udp_servers_[index];
    server->socket.async_receive_from(
        network_read_buffer_t(server->read_buf.data(), server->read_buf.size()), server->remote_endpoint,
        [this, self = shared_from_this(), server, index](const asio::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (!ec && bytes_read > 0) {
                auto current_endpoint = server->remote_endpoint;
                std::vector<std::uint8_t> encrypted_packet(server->read_buf.data(),
                                                           server->read_buf.data() + bytes_read);

                // All UDP packets are encrypted with the shared traffic key (flow_id=0)
                auto traffic_key = frp_derive_kcp_flow_key(config_.traffic_secret, 0);
                auto plaintext   = frp_kcp_decrypt(traffic_key, encrypted_packet);
                if (!plaintext) {
                    FINFO("udp_server failed to decrypt packet from {}:{} size={}",
                          current_endpoint.address().to_string(), current_endpoint.port(), bytes_read);
                    start_udp_receive(index);
                    return;
                }

                std::string payload(plaintext->begin(), plaintext->end());

                frp_runtime_command_base command;
                if (!Fundamental::io::from_json(payload, command)) {
                    start_udp_receive(index);
                    return;
                }

                // Time sync request: NTP-like clock sync via UDP
                if (command.command == frp_runtime_time_sync_request_command) {
                    frp_runtime_time_sync_request_data req;
                    if (!Fundamental::io::from_json(payload, req)) { start_udp_receive(index); return; }
                    std::int64_t T2 = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    frp_runtime_time_sync_response_data resp;
                    resp.command = frp_runtime_time_sync_response_command;
                    resp.seq = req.seq;
                    resp.client_send_ts = req.client_send_ts;
                    resp.server_recv_ts = T2;
                    resp.server_send_ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    auto resp_json = Fundamental::io::to_json(resp);
                    auto encrypted_resp = frp_kcp_encrypt_string(traffic_key, resp_json);
                    if (!encrypted_resp.empty()) {
                        auto resp_buf = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted_resp));
                        server->socket.async_send_to(asio::buffer(*resp_buf), current_endpoint,
                            [resp_buf](const std::error_code&, std::size_t) {});
                    }
                    start_udp_receive(index);
                    return;
                }

                if (command.command != frp_runtime_p2p_probe_command) {
                    start_udp_receive(index);
                    return;
                }
                frp_runtime_p2p_probe_data probe;
                if (!Fundamental::io::from_json(payload, probe)) {
                    start_udp_receive(index);
                    return;
                }

                // Echo back external endpoint (both startup and endpoint probe)
                frp_runtime_udp_echo_data echo;
                echo.command       = frp_runtime_udp_echo_command;
                echo.external_ip   = current_endpoint.address().to_string();
                echo.external_port = current_endpoint.port();
                auto echo_json      = Fundamental::io::to_json(echo);
                auto encrypted_resp = frp_kcp_encrypt_string(traffic_key, echo_json);
                if (!encrypted_resp.empty()) {
                    auto resp_buf = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted_resp));
                    server->socket.async_send_to(asio::buffer(*resp_buf), current_endpoint,
                                                 [resp_buf](const std::error_code&, std::size_t) {});
                }
            }
            start_udp_receive(index);
        });
}

void frp_runtime_public_server::release_obj() {
    reference_.release();
    bool expected = true;
    if (!has_started_.compare_exchange_strong(expected, false)) return;
    asio::post(acceptor_.get_executor(), [this, self = shared_from_this()]() {
        std::error_code ec;
        acceptor_.close(ec);
        for (auto& server : udp_servers_) {
            if (!server) continue;
            server->socket.close(ec);
        }
    });
}

bool frp_runtime_public_server::verify_auth_digest(std::string_view nonce, std::string_view digest) const {
    return frp_hmac_sha256_hex(config_.traffic_secret, nonce) == digest;
}

frp_runtime_create_flow_response_data frp_runtime_public_server::create_flow(
    const std::shared_ptr<frp_runtime_signal_session>& accessor_session,
    const frp_runtime_create_flow_request_data& request) {
    frp_runtime_create_flow_response_data response;
    response.command = frp_runtime_create_flow_response_command;
    if (!accessor_session) {
        response.result  = frp_runtime_flow_result_rejected;
        response.message = "invalid accessor session";
        return response;
    }

    std::shared_ptr<frp_runtime_signal_session> provider_session;
    frp_runtime_prepare_flow_data prepare;
    prepare.command = frp_runtime_prepare_flow_command;

    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto accessor_it = clients_by_uuid_.find(accessor_session->get_uuid());
        if (accessor_it == clients_by_uuid_.end()) {
            FWARN("create_flow accessor_uuid={} not in clients_by_uuid_", accessor_session->get_uuid());
            response.result = frp_runtime_flow_result_rejected;
            response.message = "accessor state out of sync";
            return response;
        }
        auto registry_it = services_by_register_key_.find(request.register_key);
        if (registry_it == services_by_register_key_.end()) {
            response.result  = frp_runtime_flow_result_rejected;
            response.message = "service directory unavailable";
            return response;
        }
        auto service_it = registry_it->second.find(request.service_name);
        if (service_it == registry_it->second.end()) {
            response.result  = frp_runtime_flow_result_rejected;
            response.message = "service not found";
            return response;
        }
        auto provider_it = clients_by_uuid_.find(service_it->second.provider_uuid);
        if (provider_it == clients_by_uuid_.end()) {
            response.result  = frp_runtime_flow_result_rejected;
            response.message = "provider unavailable";
            return response;
        }
        provider_session = provider_it->second.session.lock();
        if (!provider_session) {
            response.result  = frp_runtime_flow_result_rejected;
            response.message = "provider session offline";
            return response;
        }

        std::uint32_t flow_id = next_flow_id_.fetch_add(1);
        flow_runtime_state flow;
        flow.flow_id       = flow_id;
        flow.service_name  = service_it->second.service_name;
        flow.enable_p2p    = service_it->second.enable_p2p;
        flow.provider_uuid = service_it->second.provider_uuid;
        flow.accessor_uuid = accessor_session->get_uuid();

        const std::uint8_t requested_transport = request.transport;
        if (requested_transport == frp_runtime_transport_tcp_relay ||
            requested_transport == frp_runtime_transport_udp_relay) {
            flow.transport = requested_transport;
        } else {
            response.result  = frp_runtime_flow_result_rejected;
            response.message = "invalid transport";
            return response;
        }
        flows_by_id_[flow_id] = flow;

        response.result        = frp_runtime_flow_result_accepted;
        response.flow_id       = flow_id;
        response.provider_uuid = flow.provider_uuid;
        response.message       = "";

        prepare.flow_id       = flow_id;
        prepare.accessor_uuid = accessor_session->get_uuid();
        prepare.service_name  = service_it->second.service_name;
        prepare.transport     = flow.transport;
    }

    provider_session->send_command(prepare);
    return response;
}

bool frp_runtime_public_server::provider_mark_flow_ready(const std::shared_ptr<frp_runtime_signal_session>& provider_session,
                                                         std::uint32_t flow_id,
                                                         std::string& error_message) {
    std::shared_ptr<frp_runtime_signal_session> accessor_session;
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto flow_it = flows_by_id_.find(flow_id);
        if (flow_it == flows_by_id_.end()) {
            error_message = "unknown flow_id";
            return false;
        }
        if (!provider_session || flow_it->second.provider_uuid != provider_session->get_uuid()) {
            error_message = "provider uuid mismatch";
            return false;
        }
        flow_it->second.provider_ready = true;
        auto accessor_it = clients_by_uuid_.find(flow_it->second.accessor_uuid);
        if (accessor_it == clients_by_uuid_.end()) {
            error_message = "accessor offline";
            return false;
        }
        accessor_session = accessor_it->second.session.lock();
        if (!accessor_session) {
            error_message = "accessor session offline";
            return false;
        }
    }

    frp_runtime_flow_ready_data ready;
    ready.command = frp_runtime_flow_ready_command;
    ready.flow_id = flow_id;
    accessor_session->send_command(ready);
    return true;
}

bool frp_runtime_public_server::forward_flow_failed(const std::shared_ptr<frp_runtime_signal_session>& session,
                                                    const frp_runtime_flow_failed_data& data,
                                                    std::string& error_message) {
    std::shared_ptr<frp_runtime_signal_session> peer_session;
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto flow_it = flows_by_id_.find(data.flow_id);
        if (flow_it == flows_by_id_.end()) {
            return true;
        }
        const auto& flow = flow_it->second;
        const bool is_provider = session && flow.provider_uuid == session->get_uuid();
        const bool is_accessor = session && flow.accessor_uuid == session->get_uuid();
        if (!is_provider && !is_accessor) {
            error_message = "flow uuid mismatch";
            return false;
        }
        if (is_provider) {
            auto accessor_it = clients_by_uuid_.find(flow.accessor_uuid);
            if (accessor_it != clients_by_uuid_.end()) peer_session = accessor_it->second.session.lock();
        } else {
            auto provider_it = clients_by_uuid_.find(flow.provider_uuid);
            if (provider_it != clients_by_uuid_.end()) peer_session = provider_it->second.session.lock();
        }
        flows_by_id_.erase(flow_it);
    }
    if (peer_session) peer_session->send_command(data);
    return true;
}

bool frp_runtime_public_server::forward_flow_closed(const std::shared_ptr<frp_runtime_signal_session>& session,
                                                    const frp_runtime_flow_closed_data& data,
                                                    std::string& error_message) {
    std::shared_ptr<frp_runtime_signal_session> peer_session;
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto flow_it = flows_by_id_.find(data.flow_id);
        if (flow_it == flows_by_id_.end()) {
            return true;
        }
        auto& flow = flow_it->second;
        const bool is_provider = session && flow.provider_uuid == session->get_uuid();
        const bool is_accessor = session && flow.accessor_uuid == session->get_uuid();
        if (!is_provider && !is_accessor) {
            error_message = "flow uuid mismatch";
            return false;
        }
        if (is_provider) {
            auto accessor_it = clients_by_uuid_.find(flow.accessor_uuid);
            if (accessor_it != clients_by_uuid_.end()) peer_session = accessor_it->second.session.lock();
        } else {
            auto provider_it = clients_by_uuid_.find(flow.provider_uuid);
            if (provider_it != clients_by_uuid_.end()) peer_session = provider_it->second.session.lock();
        }
        // Keep flow in map; release_session_state() will erase when data sessions disconnect.
    }
    if (peer_session) peer_session->send_command(data);
    return true;
}

void frp_runtime_public_server::relay_punch_message(const std::shared_ptr<frp_runtime_signal_session>& session,
                                                      const frp_proxy_command_data& base,
                                                      const std::string& raw_payload) {
    std::shared_ptr<frp_runtime_signal_session> peer_session;
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto it = clients_by_uuid_.find(base.uuid);
        if (it != clients_by_uuid_.end()) peer_session = it->second.session.lock();
    }
    if (peer_session) {
        FINFO("relay_punch_message dest_uuid={} flow_id={} payload_size={} payload_preview={}",
              base.uuid, base.flow_id, raw_payload.size(),
              raw_payload.size() > 80 ? raw_payload.substr(0, 80) : raw_payload);
        // Forward raw payload directly to avoid re-serialization type slicing
        auto packet = std::make_shared<std::string>();
        std::uint32_t data_size = static_cast<std::uint32_t>(raw_payload.size());
        packet->resize(4 + raw_payload.size());
        Fundamental::net_buffer_copy(&data_size, packet->data(), 4);
        std::memcpy(packet->data() + 4, raw_payload.data(), raw_payload.size());
        asio::post(peer_session->executor_,
                   [session = peer_session, packet = std::move(packet)]() mutable {
                       session->write_queue_.push_back(std::move(packet));
                       if (session->write_queue_.size() == 1) {
                           session->do_write();
                       }
                   });
    } else {
        FWARN("relay_punch_message dest_uuid={} peer session lost", base.uuid);
    }
}

bool frp_runtime_public_server::bind_data_session(const std::shared_ptr<frp_runtime_signal_session>& session,
                                                  const frp_runtime_data_open_data& data,
                                                  std::string& error_message) {
    if (!session) {
        error_message = "invalid data session";
        return false;
    }

    std::shared_ptr<frp_runtime_signal_session> provider_signal_session;
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto flow_it = flows_by_id_.find(data.flow_id);
        if (flow_it == flows_by_id_.end()) {
            error_message = "unknown flow_id";
            return false;
        }
        auto& flow = flow_it->second;
        if (data.uuid == flow.provider_uuid) {
            if (auto existed = flow.provider_data_session.lock(); existed && existed.get() != session.get()) {
                error_message = "provider data session already exists";
                return false;
            }
            flow.provider_data_session = session;
        } else if (data.uuid == flow.accessor_uuid) {
            if (auto existed = flow.accessor_data_session.lock(); existed && existed.get() != session.get()) {
                error_message = "accessor data session already exists";
                return false;
            }
            flow.accessor_data_session = session;
        } else {
            error_message = "flow uuid mismatch";
            return false;
        }

        if (!flow.transport_ready) {
            auto provider_data = flow.provider_data_session.lock();
            auto accessor_data = flow.accessor_data_session.lock();
            if (provider_data && accessor_data) {
                flow.transport_ready = true;
                FINFO("bind_data_session transport_ready flow_id={} both sides connected", data.flow_id);
                auto provider_it = clients_by_uuid_.find(flow.provider_uuid);
                if (provider_it != clients_by_uuid_.end()) {
                    provider_signal_session = provider_it->second.session.lock();
                }
            }
        }
    }

    if (provider_signal_session) {
        frp_runtime_flow_transport_ready_data ready;
        ready.command = frp_runtime_flow_transport_ready_command;
        ready.flow_id = data.flow_id;
        provider_signal_session->send_command(ready);
    }
    return true;
}

bool frp_runtime_public_server::forward_flow_bytes(const std::shared_ptr<frp_runtime_signal_session>& session,
                                                   const std::shared_ptr<std::string>& data,
                                                   std::string& error_message) {
    if (!session || !data) {
        error_message = "invalid data";
        return false;
    }

    std::shared_ptr<frp_runtime_signal_session> peer_session;
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto flow_it = flows_by_id_.find(session->get_flow_id());
        if (flow_it == flows_by_id_.end()) {
            error_message = "unknown flow_id";
            return false;
        }
        auto& flow = flow_it->second;
        if (!flow.transport_ready) {
            // KCP initial probes may arrive before transport is fully ready;
            // silently drop them so the data session is not released.
            return true;
        }
        if (flow.provider_data_session.lock().get() == session.get()) {
            peer_session = flow.accessor_data_session.lock();
        } else if (flow.accessor_data_session.lock().get() == session.get()) {
            if (!flow.provider_ready) {
                return true;
            }
            peer_session = flow.provider_data_session.lock();
        } else {
            error_message = "flow data session mismatch";
            return false;
        }
    }

    if (!peer_session) {
        error_message = "peer data session offline";
        return false;
    }
    peer_session->send_raw(data);
    return true;
}

void frp_runtime_public_server::release_session_state(const frp_runtime_signal_session* session_ptr) {
    if (!session_ptr) return;
    std::vector<std::shared_ptr<frp_runtime_signal_session>> peer_data_sessions;
    std::vector<std::shared_ptr<frp_runtime_signal_session>> peer_signal_sessions;
    std::vector<std::uint32_t> erased_flows;
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        for (auto it = sessions_by_uuid_.begin(); it != sessions_by_uuid_.end();) {
            auto strong = it->second.lock();
            if (!strong || strong.get() == session_ptr) {
                if (strong && strong.get() == session_ptr) {
                    clear_session_state_locked(strong);
                }
                it = sessions_by_uuid_.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = flows_by_id_.begin(); it != flows_by_id_.end();) {
            auto& flow = it->second;
            auto provider_data = flow.provider_data_session.lock();
            auto accessor_data = flow.accessor_data_session.lock();
            bool is_provider_data = provider_data && provider_data.get() == session_ptr;
            bool is_accessor_data = accessor_data && accessor_data.get() == session_ptr;
            if (is_provider_data || is_accessor_data) {
                if (provider_data && !is_provider_data) peer_data_sessions.push_back(provider_data);
                if (accessor_data && !is_accessor_data) peer_data_sessions.push_back(accessor_data);
                auto provider_it = clients_by_uuid_.find(flow.provider_uuid);
                if (provider_it != clients_by_uuid_.end()) {
                    if (auto signal = provider_it->second.session.lock(); signal && signal.get() != session_ptr) {
                        peer_signal_sessions.push_back(signal);
                    }
                }
                auto accessor_it = clients_by_uuid_.find(flow.accessor_uuid);
                if (accessor_it != clients_by_uuid_.end()) {
                    if (auto signal = accessor_it->second.session.lock(); signal && signal.get() != session_ptr) {
                        peer_signal_sessions.push_back(signal);
                    }
                }
                erased_flows.push_back(it->first);
                it = flows_by_id_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& peer : peer_data_sessions) {
        peer->release_obj();
    }
    for (auto flow_id : erased_flows) {
        frp_runtime_flow_closed_data closed;
        closed.command = frp_runtime_flow_closed_command;
        closed.flow_id = flow_id;
        for (auto& peer_signal : peer_signal_sessions) {
            peer_signal->send_command(closed);
        }
    }
}

void frp_runtime_public_server::do_accept() {
    acceptor_.async_accept(io_context_pool::Instance().get_io_context(),
                           [this, self = shared_from_this()](asio::error_code ec, asio::ip::tcp::socket socket) {
                               if (!reference_.is_valid() || !acceptor_.is_open()) return;
                               if (!ec) {
                                   auto session =
                                       frp_runtime_signal_session::make_shared(std::move(socket), shared_from_this());
#ifndef NETWORK_DISABLE_SSL
                                   if (ssl_context_) {
                                       session->enable_ssl(*ssl_context_);
                                   }
#endif
                                   session->start();
                               }
                               do_accept();
                           });
}

void frp_runtime_public_server::configure_ssl() {
#ifndef NETWORK_DISABLE_SSL
    if (config_.ssl.disable_ssl) return;
    if (config_.ssl.certificate_path.empty() || config_.ssl.private_key_path.empty()) {
        throw std::invalid_argument("frp runtime server ssl requires certificate_path and private_key_path");
    }
    ssl_context_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv13);
    ssl_context_->set_options(asio::ssl::context::no_sslv2 | asio::ssl::context::single_dh_use);
    auto verify_flag = asio::ssl::verify_peer;
    if (!config_.ssl.ca_certificate_path.empty()) {
        ssl_context_->load_verify_file(config_.ssl.ca_certificate_path);
    } else {
        ssl_context_->set_default_verify_paths();
    }
    if (config_.ssl.verify_client) verify_flag |= asio::ssl::verify_fail_if_no_peer_cert;
    ssl_context_->set_verify_mode(verify_flag);
    ssl_context_->use_certificate_chain_file(config_.ssl.certificate_path);
    ssl_context_->use_private_key_file(config_.ssl.private_key_path, asio::ssl::context::pem);
    if (!config_.ssl.tmp_dh_path.empty()) {
        ssl_context_->use_tmp_dh_file(config_.ssl.tmp_dh_path);
    }
#endif
}

void frp_runtime_public_server::clear_session_state_locked(const std::shared_ptr<frp_runtime_signal_session>& session) {
    if (!session) return;
    clear_client_state_locked(session->get_uuid());
}

void frp_runtime_public_server::clear_client_state_locked(const std::string& uuid) {
    auto it = clients_by_uuid_.find(uuid);
    if (it == clients_by_uuid_.end()) return;
    if (auto sk = services_by_register_key_.find(it->second.register_key);
        sk != services_by_register_key_.end()) {
        for (const auto& sn : it->second.services) sk->second.erase(sn);
        if (sk->second.empty()) services_by_register_key_.erase(sk);
    }
    clients_by_uuid_.erase(it);
}

frp_runtime_signal_session::frp_runtime_signal_session(::asio::ip::tcp::socket&& socket,
                                                       std::shared_ptr<frp_runtime_public_server> owner) :
socket_(std::move(socket)), executor_(socket_.get_executor()), owner_(std::move(owner)) {
    enable_tcp_keep_alive(socket_);
}

void frp_runtime_signal_session::start() {
    if (!reference_.is_valid()) return;
#ifndef NETWORK_DISABLE_SSL
    if (ssl_context_ref_) {
        ssl_handshake();
        return;
    }
#endif
    start_protocol();
}

void frp_runtime_signal_session::release_obj() {
    if (!reference_.release()) return;
    FINFO("signal_session release_obj uuid={} mode={} role={}", uuid_,
          static_cast<int>(mode_), 0);
    if (owner_) owner_->release_session_state(this);
    asio::post(executor_, [this, self = shared_from_this()]() { close_socket(); });
}

#ifndef NETWORK_DISABLE_SSL
void frp_runtime_signal_session::enable_ssl(asio::ssl::context& ssl_context) {
    ssl_context_ref_ = &ssl_context;
}
#endif

void frp_runtime_signal_session::do_write() {
    if (write_queue_.empty()) return;
    auto& current = write_queue_.front();
    auto handler  = [this, self = shared_from_this()](std::error_code ec, std::size_t) {
        if (!reference_.is_valid()) return;
        if (ec) {
            release_obj();
            return;
        }
        write_queue_.pop_front();
        if (!write_queue_.empty()) do_write();
    };
#ifndef NETWORK_DISABLE_SSL
    if (ssl_stream_) {
        asio::async_write(*ssl_stream_, asio::buffer(current->data(), current->size()), std::move(handler));
        return;
    }
#endif
    asio::async_write(socket_, asio::buffer(current->data(), current->size()), std::move(handler));
}

void frp_runtime_signal_session::start_protocol() {
    read_next_command();
}

void frp_runtime_signal_session::read_next_command() {
    auto read_payload = [this, self = shared_from_this()](std::error_code ec, std::size_t) {
        if (!reference_.is_valid()) return;
        if (ec) {
            release_obj();
            return;
        }
        process_command(payload_);
    };

    auto read_header = [this, self = shared_from_this(), read_payload](std::error_code ec, std::size_t) {
        if (!reference_.is_valid()) return;
        if (ec) {
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
#ifndef NETWORK_DISABLE_SSL
        if (ssl_stream_) {
            asio::async_read(*ssl_stream_, asio::buffer(payload_.data(), payload_.size()), read_payload);
            return;
        }
#endif
        asio::async_read(socket_, asio::buffer(payload_.data(), payload_.size()), read_payload);
    };

#ifndef NETWORK_DISABLE_SSL
    if (ssl_stream_) {
        asio::async_read(*ssl_stream_, asio::buffer(header_buf_.data(), header_buf_.size()), read_header);
        return;
    }
#endif
    asio::async_read(socket_, asio::buffer(header_buf_.data(), header_buf_.size()), read_header);
}

void frp_runtime_signal_session::process_command(std::string payload) {
    frp_runtime_command_base base_command;
    if (!Fundamental::io::from_json(payload, base_command)) {
        release_obj();
        return;
    }
    if (mode_ == session_mode::undecided) {
        handle_initial_phase(base_command, std::move(payload));
        return;
    }
    if (mode_ == session_mode::data) {
        release_obj();
        return;
    }
    if (!authenticated_) {
        handle_server_hello_phase(base_command, std::move(payload));
        return;
    }
    handle_authenticated_phase(base_command, std::move(payload));
}

void frp_runtime_signal_session::handle_initial_phase(const frp_runtime_command_base& command, std::string payload) {
    switch (command.command) {
    case frp_runtime_signal_open_command: {
        mode_ = session_mode::signal;
        server_nonce_ = frp_generate_server_nonce();
        FINFO("signal_session signal_open received from {}",
              socket_.remote_endpoint().address().to_string());
        frp_runtime_server_hello_data hello;
        hello.command      = frp_runtime_server_hello_command;
        hello.server_nonce = server_nonce_;
        send_command(hello);
        read_next_command();
        return;
    }
    case frp_runtime_data_open_command: {
        frp_runtime_data_open_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        FINFO("signal_session data_open received flow_id={} uuid={}", request.flow_id, request.uuid);
        handle_data_open_phase(request);
        return;
    }
    default: release_obj(); return;
    }
}

void frp_runtime_signal_session::handle_server_hello_phase(const frp_runtime_command_base& command, std::string payload) {
    if (command.command != frp_runtime_auth_request_command) {
        send_auth_failure_and_close("expected auth_request");
        return;
    }
    frp_runtime_auth_request_data request;
    if (!Fundamental::io::from_json(payload, request)) {
        send_auth_failure_and_close("invalid auth_request");
        return;
    }
    FINFO("signal_session auth_request received nonce={}", server_nonce_);
    frp_runtime_auth_response_data response;
    response.command = frp_runtime_auth_response_command;
    response.ok      = owner_ && owner_->verify_auth_digest(server_nonce_, request.digest);
    response.message = response.ok ? "ok" : "auth failed";
    send_command(response);
    if (!response.ok) {
        release_obj();
        return;
    }
    authenticated_ = true;
    read_next_command();
}

void frp_runtime_signal_session::handle_authenticated_phase(const frp_runtime_command_base& command, std::string payload) {
    switch (command.command) {
    case frp_runtime_register_services_command: {
        frp_runtime_register_services_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        handle_register_services_phase(request);
        return;
    }
    case frp_runtime_create_flow_request_command: {
        frp_runtime_create_flow_request_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        handle_create_flow_phase(request);
        return;
    }
    case frp_runtime_flow_ready_command: {
        frp_runtime_flow_ready_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        handle_flow_ready_phase(request);
        return;
    }
    case frp_runtime_flow_failed_command: {
        frp_runtime_flow_failed_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        handle_flow_failed_phase(request);
        return;
    }
    case frp_runtime_flow_closed_command: {
        frp_runtime_flow_closed_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        handle_flow_closed_phase(request);
        return;
    }
    case frp_runtime_punch_confirm_command:
    case frp_runtime_punch_confirm_ack_command: 
    case frp_runtime_punch_confirm_ok_command:
    case frp_runtime_p2p_handshake_command:
    case frp_runtime_p2p_handshake_ack_command:
    case frp_runtime_punch_start_command:
     {
        frp_proxy_command_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        if (owner_) owner_->relay_punch_message(shared_from_this(), request, payload);
        read_next_command();
        return;
    }
    case frp_runtime_subscribe_services_command: {
        frp_runtime_subscribe_services_data request;
        if (!Fundamental::io::from_json(payload, request)) { release_obj(); return; }
        handle_subscribe_services_phase(request);
        return;
    }
    default: release_obj(); return;
    }
}

void frp_runtime_signal_session::handle_data_open_phase(const frp_runtime_data_open_data& request) {
    std::string error_message;
    if (!owner_ || !owner_->bind_data_session(shared_from_this(), request, error_message)) {
        FERR("data_open rejected flow_id={} uuid={} err={}", request.flow_id, request.uuid, error_message);
        release_obj();
        return;
    }
    FINFO("data_open accepted flow_id={} uuid={}", request.flow_id, request.uuid);
    mode_    = session_mode::data;
    flow_id_ = request.flow_id;
    uuid_    = request.uuid;
    start_data_read_loop();
}
void frp_runtime_signal_session::handle_create_flow_phase(const frp_runtime_create_flow_request_data& request) {
    FINFO("signal_session create_flow_request uuid={} service_name={} transport={}",
          uuid_, request.service_name, static_cast<int>(request.transport));
    frp_runtime_create_flow_response_data response;
    if (owner_) {
        response = owner_->create_flow(shared_from_this(), request);
    } else {
        response.command = frp_runtime_create_flow_response_command;
        response.result  = frp_runtime_flow_result_rejected;
        response.message = "server released";
    }
    send_command(response);
    if (response.result == frp_runtime_flow_result_rejected) {
        release_obj();
        return;
    }
    // accepted or p2p_unavailable: accessor continues (may retry with tcp_relay)
    read_next_command();
}

void frp_runtime_signal_session::handle_flow_ready_phase(const frp_runtime_flow_ready_data& request) {
    std::string error_message;
    if (!owner_ || !owner_->provider_mark_flow_ready(shared_from_this(), request.flow_id, error_message)) {
        FERR("flow_ready rejected uuid={} flow_id={} err={}", uuid_, request.flow_id, error_message);
        release_obj();
        return;
    }
    read_next_command();
}

void frp_runtime_signal_session::handle_flow_failed_phase(const frp_runtime_flow_failed_data& request) {
    std::string error_message;
    if (!owner_ || !owner_->forward_flow_failed(shared_from_this(), request, error_message)) {
        FERR("flow_failed rejected uuid={} flow_id={} err={}", uuid_, request.flow_id, error_message);
        release_obj();
        return;
    }
    read_next_command();
}

void frp_runtime_signal_session::handle_flow_closed_phase(const frp_runtime_flow_closed_data& request) {
    std::string error_message;
    if (!owner_ || !owner_->forward_flow_closed(shared_from_this(), request, error_message)) {
        FERR("flow_closed rejected uuid={} flow_id={} err={}", uuid_, request.flow_id, error_message);
        release_obj();
        return;
    }
    read_next_command();
}

void frp_runtime_signal_session::send_auth_failure_and_close(const std::string& message) {
    frp_runtime_auth_response_data response;
    response.command = frp_runtime_auth_response_command;
    response.ok      = false;
    response.message = message;
    send_command(response);
    release_obj();
}

void frp_runtime_signal_session::start_data_read_loop() {
    auto handler = [this, self = shared_from_this()](const asio::error_code& ec, std::size_t bytes_read) {
        if (!reference_.is_valid()) return;
        if (ec) {
            release_obj();
            return;
        }
        auto data = std::make_shared<std::string>(raw_read_buf_.data(), bytes_read);
        std::string error_message;
        if (!owner_ || !owner_->forward_flow_bytes(shared_from_this(), data, error_message)) {
            FERR("forward flow bytes rejected flow_id={} uuid={} err={}", flow_id_, uuid_, error_message);
            release_obj();
            return;
        }
        start_data_read_loop();
    };
#ifndef NETWORK_DISABLE_SSL
    if (ssl_stream_) {
        ssl_stream_->async_read_some(network_read_buffer_t(raw_read_buf_.data(), raw_read_buf_.size()), std::move(handler));
        return;
    }
#endif
    socket_.async_read_some(network_read_buffer_t(raw_read_buf_.data(), raw_read_buf_.size()), std::move(handler));
}

void frp_runtime_signal_session::send_raw(const std::shared_ptr<std::string>& packet) {
    asio::post(executor_, [this, self = shared_from_this(), packet]() mutable {
        if (!reference_.is_valid() || !packet) return;
        write_queue_.push_back(packet);
        if (write_queue_.size() == 1) {
            do_write();
        }
    });
}

void frp_runtime_signal_session::close_socket() {
    if (!socket_.is_open()) return;
    auto final_close = [this, self = shared_from_this()]() {
        asio::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    };
#ifndef NETWORK_DISABLE_SSL
    if (ssl_stream_) {
        asio::dispatch(ssl_stream_->get_executor(), std::move(final_close));
        return;
    }
#endif
    final_close();
}

void frp_runtime_signal_session::ssl_handshake() {
#ifndef NETWORK_DISABLE_SSL
    ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket_, *ssl_context_ref_);
    ssl_stream_->async_handshake(asio::ssl::stream_base::server, [this, self = shared_from_this()](const asio::error_code& ec) {
        if (!reference_.is_valid()) return;
        if (ec) {
            release_obj();
            return;
        }
        start_protocol();
    });
#endif
}

// --- new protocol handlers ---

bool frp_runtime_public_server::register_client_services(
    const std::shared_ptr<frp_runtime_signal_session>& session,
    const frp_runtime_register_services_data& request,
    std::string& error_message) {
    if (!session) { error_message = "invalid session"; return false; }
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto& state = clients_by_uuid_[request.uuid];
        state.uuid = request.uuid;
        state.nat_type = request.nat_type;
        state.startup_rtt_ms = request.startup_rtt_ms;
        state.session = session;
        sessions_by_uuid_[request.uuid] = session;
        for (const auto& group : request.groups) {
            if (allowed_register_keys_.count(group.register_key) == 0) {
                error_message = "register_key not allowed: " + group.register_key;
                return false;
            }
            state.register_key = group.register_key;
            if (group.services.empty()) continue;
            auto& dir = services_by_register_key_[group.register_key];
            for (const auto& svc : group.services) {
                frp_runtime_service_directory_entry e;
                e.service_name = svc.service_name; e.provider_uuid = request.uuid;
                e.provider_nat_type = request.nat_type; e.service_type = svc.service_type;
                e.enable_p2p = svc.enable_p2p;
                dir[svc.service_name] = std::move(e);
                state.services.insert(svc.service_name);
            }
        }
    }
    FINFO("client {} registered services groups={}", request.uuid, request.groups.size());
    return true;
}

std::vector<frp_runtime_visible_service_data> frp_runtime_public_server::list_services_for_subscriber(
    const std::shared_ptr<frp_runtime_signal_session>& session,
    const std::vector<std::string>& register_keys,
    std::string& error_message) const {
    std::vector<frp_runtime_visible_service_data> result;
    if (!session) { error_message = "invalid session"; return result; }
    std::scoped_lock<std::mutex> locker(runtime_mutex_);
    for (const auto& key : register_keys) {
        auto kit = services_by_register_key_.find(key);
        if (kit == services_by_register_key_.end()) continue;
        for (const auto& [_, svc] : kit->second) {
            if (svc.provider_uuid == session->get_uuid()) continue;
            frp_runtime_visible_service_data v;
            v.service_name = svc.service_name; v.provider_uuid = svc.provider_uuid;
            v.provider_nat_type = svc.provider_nat_type; v.service_type = svc.service_type;
            v.enable_p2p = svc.enable_p2p;
            if (auto pit = clients_by_uuid_.find(svc.provider_uuid); pit != clients_by_uuid_.end())
                v.provider_startup_rtt_ms = pit->second.startup_rtt_ms;
            result.push_back(std::move(v));
        }
    }
    return result;
}

void frp_runtime_signal_session::handle_register_services_phase(const frp_runtime_register_services_data& request) {
    frp_runtime_register_services_resp_data resp;
    resp.command = frp_runtime_register_services_resp_command;
    std::string em;
    resp.ok = owner_ && owner_->register_client_services(shared_from_this(), request, em);
    resp.message = resp.ok ? "ok" : em;
    if (resp.ok) {
        uuid_ = request.uuid; nat_type_ = request.nat_type;
        register_key_ = request.groups.empty() ? "" : request.groups[0].register_key;
        joined_ = true;
    }
    send_command(resp);
    read_next_command();
}

void frp_runtime_signal_session::handle_subscribe_services_phase(const frp_runtime_subscribe_services_data& request) {
    frp_runtime_subscribe_services_resp_data resp;
    resp.command = frp_runtime_subscribe_services_resp_command;
    std::string em;
    auto svcs = owner_ ? owner_->list_services_for_subscriber(shared_from_this(), request.register_keys, em)
                       : std::vector<frp_runtime_visible_service_data>{};
    if (!em.empty()) { resp.ok = false; resp.message = em; }
    else { resp.ok = true; resp.services = std::move(svcs); }
    send_command(resp);
    read_next_command();
}

} // namespace network::proxy
