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
        network_read_buffer_t(server->read_buf.data(), server->read_buf.size()),
        server->remote_endpoint,
        [this, self = shared_from_this(), server, index](const asio::error_code& ec, std::size_t bytes_read) {
            if (!reference_.is_valid()) return;
            if (!ec && bytes_read > 0) {
                FINFO("udp_server recv port_index={} bytes={} from={}:{}", index, bytes_read,
                      server->remote_endpoint.address().to_string(), server->remote_endpoint.port());
                std::vector<std::uint8_t> encrypted_packet(server->read_buf.data(), server->read_buf.data() + bytes_read);

                // All UDP packets are encrypted with the shared traffic key (flow_id=0)
                auto traffic_key = frp_derive_kcp_flow_key(config_.traffic_secret, 0);
                auto plaintext = frp_kcp_decrypt(traffic_key, encrypted_packet);
                if (!plaintext) {
                    FINFO("udp_server failed to decrypt packet from {}:{} size={}",
                          server->remote_endpoint.address().to_string(), server->remote_endpoint.port(), bytes_read);
                    start_udp_receive(index);
                    return;
                }

                std::string payload(plaintext->begin(), plaintext->end());
                FINFO("udp_receive decrypted from {}:{} payload_size={}",
                      server->remote_endpoint.address().to_string(), server->remote_endpoint.port(), payload.size());

                frp_runtime_command_base command;
                if (!Fundamental::io::from_json(payload, command) || command.command != frp_runtime_p2p_probe_command) {
                    start_udp_receive(index);
                    return;
                }
                frp_runtime_p2p_probe_data probe;
                if (!Fundamental::io::from_json(payload, probe)) {
                    start_udp_receive(index);
                    return;
                }

                if (probe.flow_id == 0) {
                    // startup probe: echo back external endpoint
                    frp_runtime_flow_endpoint_ready_data echo;
                    echo.command       = frp_runtime_flow_endpoint_ready_command;
                    echo.flow_id       = 0;
                    echo.external_ip   = server->remote_endpoint.address().to_string();
                    echo.external_port = server->remote_endpoint.port();
                    FINFO("startup_probe received from {}:{} echoing external={}:{}",
                          server->remote_endpoint.address().to_string(), server->remote_endpoint.port(),
                          echo.external_ip, echo.external_port);
                    auto echo_json = Fundamental::io::to_json(echo);
                    auto encrypted_resp = frp_kcp_encrypt_string(traffic_key, echo_json);
                    if (!encrypted_resp.empty()) {
                        auto resp_buf = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted_resp));
                        server->socket.async_send_to(asio::buffer(*resp_buf), server->remote_endpoint,
                                                     [resp_buf](const std::error_code&, std::size_t) {});
                    }
                } else {
                    // flow endpoint probe
                    std::string error_message;
                    if (!register_p2p_probe(probe, server->remote_endpoint, error_message)) {
                        FWARN("drop p2p probe flow_id={} uuid={} err={}", probe.flow_id, probe.uuid, error_message);
                    }
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

bool frp_runtime_public_server::bind_signal_identity(const std::shared_ptr<frp_runtime_signal_session>& session,
                                                     const frp_runtime_join_request_data& request,
                                                     std::string& error_message) {
    if (!session) {
        error_message = "invalid session";
        return false;
    }
    if (request.uuid.empty()) {
        error_message = "uuid is required";
        return false;
    }
    if (allowed_register_keys_.count(request.register_key) == 0) {
        error_message = "register_key is not allowed";
        return false;
    }
    if (request.role != frp_runtime_provider_role && request.role != frp_runtime_accessor_role) {
        error_message = "invalid role";
        return false;
    }

    std::shared_ptr<frp_runtime_signal_session> previous_session;
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        if (auto it = sessions_by_uuid_.find(request.uuid); it != sessions_by_uuid_.end()) {
            previous_session = it->second.lock();
        }
        if (previous_session && previous_session.get() != session.get()) {
            clear_session_state_locked(previous_session);
        }
        clear_session_state_locked(session);
        sessions_by_uuid_[request.uuid] = session;
        if (request.role == frp_runtime_provider_role) {
            auto& provider         = providers_by_uuid_[request.uuid];
            provider.uuid          = request.uuid;
            provider.register_key  = request.register_key;
            provider.nat_type      = request.nat_type;
            provider.startup_rtt_ms = request.startup_rtt_ms > 0 ? request.startup_rtt_ms : 100U;
            provider.session       = session;
        } else {
            auto& accessor         = accessors_by_uuid_[request.uuid];
            accessor.uuid          = request.uuid;
            accessor.register_key  = request.register_key;
            accessor.nat_type      = request.nat_type;
            accessor.startup_rtt_ms = request.startup_rtt_ms > 0 ? request.startup_rtt_ms : 100U;
            accessor.session       = session;
        }
        FINFO("bind_signal_identity uuid={} role={} nat={} startup_rtt={}ms",
              request.uuid, static_cast<int>(request.role),
              static_cast<int>(request.nat_type), request.startup_rtt_ms);
    }
    if (previous_session && previous_session.get() != session.get()) {
        previous_session->release_obj();
    }
    return true;
}

bool frp_runtime_public_server::register_provider_services(
    const std::shared_ptr<frp_runtime_signal_session>& session,
    const std::vector<frp_runtime_service_registration_data>& services,
    std::string& error_message) {
    if (!session) {
        error_message = "invalid session";
        return false;
    }
    if (session->get_role() != frp_runtime_provider_role) {
        error_message = "session is not provider";
        return false;
    }
    if (services.empty()) {
        error_message = "services must not be empty";
        return false;
    }
    std::unordered_set<std::string> dedup_names;
    for (const auto& service : services) {
        if (service.service_name.empty()) {
            error_message = "service_name must not be empty";
            return false;
        }
        if (!dedup_names.insert(service.service_name).second) {
            error_message = Fundamental::StringFormat("duplicated service_name:{}", service.service_name);
            return false;
        }
    }

    std::scoped_lock<std::mutex> locker(runtime_mutex_);
    auto provider_it = providers_by_uuid_.find(session->get_uuid());
    if (provider_it == providers_by_uuid_.end()) {
        error_message = "provider session is not joined";
        return false;
    }
    auto& provider = provider_it->second;
    auto& registry = services_by_register_key_[session->get_register_key()];
    for (const auto& service : services) {
        auto existing_it = registry.find(service.service_name);
        if (existing_it != registry.end() && existing_it->second.provider_uuid != session->get_uuid()) {
            error_message = Fundamental::StringFormat("service_name conflict:{}", service.service_name);
            return false;
        }
    }

    for (const auto& service_name : provider.services) {
        registry.erase(service_name);
    }
    provider.services.clear();
    for (const auto& service : services) {
        provider.services.insert(service.service_name);
        registry[service.service_name] =
            frp_runtime_service_directory_entry { service.service_name, session->get_uuid(), session->get_nat_type() };
    }
    return true;
}

std::vector<frp_runtime_visible_service_data> frp_runtime_public_server::list_services_for_accessor(
    const std::shared_ptr<frp_runtime_signal_session>& session,
    std::string& error_message) const {
    std::vector<frp_runtime_visible_service_data> result;
    if (!session) {
        error_message = "invalid session";
        return result;
    }
    if (session->get_role() != frp_runtime_accessor_role) {
        error_message = "session is not accessor";
        return result;
    }

    std::scoped_lock<std::mutex> locker(runtime_mutex_);
    if (accessors_by_uuid_.count(session->get_uuid()) == 0) {
        error_message = "accessor session is not joined";
        return result;
    }

    if (auto key_it = services_by_register_key_.find(session->get_register_key()); key_it != services_by_register_key_.end()) {
        result.reserve(key_it->second.size());
        for (const auto& [_, service] : key_it->second) {
            frp_runtime_visible_service_data visible;
            visible.service_name           = service.service_name;
            visible.provider_uuid          = service.provider_uuid;
            visible.provider_nat_type      = service.provider_nat_type;
            if (auto pit = providers_by_uuid_.find(service.provider_uuid); pit != providers_by_uuid_.end()) {
                visible.provider_startup_rtt_ms = pit->second.startup_rtt_ms;
            }
            result.push_back(std::move(visible));
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.service_name < rhs.service_name;
    });
    return result;
}

frp_runtime_create_flow_response_data frp_runtime_public_server::create_flow(
    const std::shared_ptr<frp_runtime_signal_session>& accessor_session,
    const frp_runtime_create_flow_request_data& request) {
    frp_runtime_create_flow_response_data response;
    response.command = frp_runtime_create_flow_response_command;
    if (!accessor_session || accessor_session->get_role() != frp_runtime_accessor_role) {
        response.result  = frp_runtime_flow_result_rejected;
        response.message = "invalid accessor session";
        return response;
    }

    std::shared_ptr<frp_runtime_signal_session> provider_session;
    frp_runtime_prepare_flow_data prepare;
    prepare.command = frp_runtime_prepare_flow_command;

    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto accessor_it = accessors_by_uuid_.find(accessor_session->get_uuid());
        if (accessor_it == accessors_by_uuid_.end() || accessor_it->second.register_key != accessor_session->get_register_key()) {
            response.result  = frp_runtime_flow_result_rejected;
            response.message = "accessor state out of sync";
            return response;
        }
        auto registry_it = services_by_register_key_.find(accessor_session->get_register_key());
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
        auto provider_it = providers_by_uuid_.find(service_it->second.provider_uuid);
        if (provider_it == providers_by_uuid_.end()) {
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
        flow.provider_uuid = service_it->second.provider_uuid;
        flow.accessor_uuid = accessor_session->get_uuid();

        const std::uint8_t requested_transport = request.transport;
        if (requested_transport != frp_runtime_transport_tcp_relay) {
            response.result  = frp_runtime_flow_result_rejected;
            response.message = "invalid transport: only tcp_relay is accepted";
            return response;
        }
        flow.transport = frp_runtime_transport_tcp_relay;
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
        auto accessor_it = accessors_by_uuid_.find(flow_it->second.accessor_uuid);
        if (accessor_it == accessors_by_uuid_.end()) {
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
            auto accessor_it = accessors_by_uuid_.find(flow.accessor_uuid);
            if (accessor_it != accessors_by_uuid_.end()) peer_session = accessor_it->second.session.lock();
        } else {
            auto provider_it = providers_by_uuid_.find(flow.provider_uuid);
            if (provider_it != providers_by_uuid_.end()) peer_session = provider_it->second.session.lock();
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
            auto accessor_it = accessors_by_uuid_.find(flow.accessor_uuid);
            if (accessor_it != accessors_by_uuid_.end()) peer_session = accessor_it->second.session.lock();
        } else {
            auto provider_it = providers_by_uuid_.find(flow.provider_uuid);
            if (provider_it != providers_by_uuid_.end()) peer_session = provider_it->second.session.lock();
        }
        if (flow.p2p_signaled) {
            // P2P upgrade completed; data sessions are already released.
            // release_session_state won't match them, so erase here explicitly.
            flows_by_id_.erase(flow_it);
        }
        // For non-p2p flows: keep in flows_by_id_ so in-flight P2P messages
        // can still be processed. release_session_state() will erase when
        // data sessions disconnect.
    }
    if (peer_session) peer_session->send_command(data);
    return true;
}

bool frp_runtime_public_server::forward_flow_data(const std::shared_ptr<frp_runtime_signal_session>& session,
                                                  const frp_runtime_flow_data_data& data,
                                                  std::string& error_message) {
    std::shared_ptr<frp_runtime_signal_session> peer_session;
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto flow_it = flows_by_id_.find(data.flow_id);
        if (flow_it == flows_by_id_.end()) {
            error_message = "unknown flow_id";
            return false;
        }
        const auto& flow = flow_it->second;
        if (session && flow.provider_uuid == session->get_uuid()) {
            auto accessor_it = accessors_by_uuid_.find(flow.accessor_uuid);
            if (accessor_it != accessors_by_uuid_.end()) peer_session = accessor_it->second.session.lock();
        } else if (session && flow.accessor_uuid == session->get_uuid()) {
            if (!flow.provider_ready) {
                error_message = "provider not ready";
                return false;
            }
            auto provider_it = providers_by_uuid_.find(flow.provider_uuid);
            if (provider_it != providers_by_uuid_.end()) peer_session = provider_it->second.session.lock();
        } else {
            error_message = "flow uuid mismatch";
            return false;
        }
    }
    if (!peer_session) {
        error_message = "peer session offline";
        return false;
    }
    peer_session->send_command(data);
    return true;
}

bool frp_runtime_public_server::register_p2p_probe(const frp_runtime_p2p_probe_data& data,
                                                   const udp::endpoint& remote_endpoint,
                                                   std::string& error_message) {
    std::shared_ptr<frp_runtime_signal_session> provider_signal_session;
    std::shared_ptr<frp_runtime_signal_session> accessor_signal_session;
    frp_runtime_flow_p2p_peer_data provider_peer;
    frp_runtime_flow_p2p_peer_data accessor_peer;
    provider_peer.command = frp_runtime_flow_p2p_peer_command;
    accessor_peer.command = frp_runtime_flow_p2p_peer_command;
    frp_runtime_flow_endpoint_ready_data provider_ep_ready;
    frp_runtime_flow_endpoint_ready_data accessor_ep_ready;
    provider_ep_ready.command = frp_runtime_flow_endpoint_ready_command;
    accessor_ep_ready.command = frp_runtime_flow_endpoint_ready_command;
    bool both_ready = false;

    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto flow_it = flows_by_id_.find(data.flow_id);
        if (flow_it == flows_by_id_.end()) {
            error_message = "unknown flow_id";
            return false;
        }
        auto& flow = flow_it->second;

        const bool is_provider = (data.uuid == flow.provider_uuid);
        const bool is_accessor = (data.uuid == flow.accessor_uuid);
        if (!is_provider && !is_accessor) {
            error_message = "flow uuid mismatch";
            return false;
        }

        // Treat probe as implicit upgrade request -- set the flag for the probing side
        if (is_provider) flow.provider_p2p_upgrade_requested = true;
        if (is_accessor) flow.accessor_p2p_upgrade_requested = true;

        p2p_probe_state* current = is_provider ? &flow.provider_probe : &flow.accessor_probe;
        current->local_ip            = data.local_ip;
        current->local_port          = data.local_port;
        current->observed_endpoint   = remote_endpoint;
        current->ready               = true;
        FINFO("register_p2p_probe flow_id={} uuid={} local_ip={} local_port={} observed={}:{}",
              data.flow_id, data.uuid, data.local_ip, data.local_port,
              remote_endpoint.address().to_string(), remote_endpoint.port());

        if (!flow.provider_probe.ready || !flow.accessor_probe.ready) {
            return true;
        }

        // Check NAT compatibility before coordinating P2P -- both sides must be p2p-capable
        {
            auto provider_it = providers_by_uuid_.find(flow.provider_uuid);
            auto accessor_it = accessors_by_uuid_.find(flow.accessor_uuid);
            if (provider_it != providers_by_uuid_.end() && accessor_it != accessors_by_uuid_.end()) {
                std::uint8_t p_nat = provider_it->second.nat_type;
                std::uint8_t a_nat = accessor_it->second.nat_type;
                if (p_nat == frp_runtime_nat_type_disabled || a_nat == frp_runtime_nat_type_disabled ||
                    (p_nat == frp_runtime_nat_type_symmetric && a_nat == frp_runtime_nat_type_symmetric)) {
                    FINFO("register_p2p_probe flow_id={} p2p not viable provider_nat={} accessor_nat={}",
                          data.flow_id, static_cast<int>(p_nat), static_cast<int>(a_nat));
                    return true; // p2p not viable, probe recorded but no coordination
                }
            }
        }
        both_ready = true;

        const bool same_public_ip =
            flow.provider_probe.observed_endpoint.address() == flow.accessor_probe.observed_endpoint.address();
        FINFO("register_p2p_probe both_ready flow_id={} same_public_ip={} provider_observed={}:{} accessor_observed={}:{}",
              data.flow_id, same_public_ip,
              flow.provider_probe.observed_endpoint.address().to_string(), flow.provider_probe.observed_endpoint.port(),
              flow.accessor_probe.observed_endpoint.address().to_string(), flow.accessor_probe.observed_endpoint.port());
        provider_peer.flow_id      = data.flow_id;
        accessor_peer.flow_id      = data.flow_id;
        provider_peer.use_local_candidate = same_public_ip;
        accessor_peer.use_local_candidate = same_public_ip;
        if (same_public_ip && !flow.accessor_probe.local_ip.empty() && flow.accessor_probe.local_port != 0) {
            provider_peer.peer_host = flow.accessor_probe.local_ip;
            provider_peer.peer_port = flow.accessor_probe.local_port;
        } else {
            provider_peer.peer_host = flow.accessor_probe.observed_endpoint.address().to_string();
            provider_peer.peer_port = flow.accessor_probe.observed_endpoint.port();
        }
        if (same_public_ip && !flow.provider_probe.local_ip.empty() && flow.provider_probe.local_port != 0) {
            accessor_peer.peer_host = flow.provider_probe.local_ip;
            accessor_peer.peer_port = flow.provider_probe.local_port;
        } else {
            accessor_peer.peer_host = flow.provider_probe.observed_endpoint.address().to_string();
            accessor_peer.peer_port = flow.provider_probe.observed_endpoint.port();
        }

        // flow_endpoint_ready: each side gets its own observed external endpoint + echoed timestamp
        provider_ep_ready.flow_id       = data.flow_id;
        provider_ep_ready.external_ip   = flow.provider_probe.observed_endpoint.address().to_string();
        provider_ep_ready.external_port = flow.provider_probe.observed_endpoint.port();
        accessor_ep_ready.flow_id       = data.flow_id;
        accessor_ep_ready.external_ip   = flow.accessor_probe.observed_endpoint.address().to_string();
        accessor_ep_ready.external_port = flow.accessor_probe.observed_endpoint.port();

        auto provider_it = providers_by_uuid_.find(flow.provider_uuid);
        if (provider_it != providers_by_uuid_.end()) provider_signal_session = provider_it->second.session.lock();
        auto accessor_it = accessors_by_uuid_.find(flow.accessor_uuid);
        if (accessor_it != accessors_by_uuid_.end()) accessor_signal_session = accessor_it->second.session.lock();
        // peer_nat_type: each side gets the other side's nat_type
        provider_peer.peer_nat_type = accessor_it != accessors_by_uuid_.end() ? accessor_it->second.nat_type : static_cast<std::uint8_t>(frp_runtime_nat_type_disabled);
        accessor_peer.peer_nat_type = provider_it != providers_by_uuid_.end() ? provider_it->second.nat_type : static_cast<std::uint8_t>(frp_runtime_nat_type_disabled);
        provider_peer.peer_startup_rtt_ms = accessor_it != accessors_by_uuid_.end() ? accessor_it->second.startup_rtt_ms : 100U;
        accessor_peer.peer_startup_rtt_ms = provider_it != providers_by_uuid_.end() ? provider_it->second.startup_rtt_ms : 100U;

        // Mark flow so release_session_state tolerates relay disconnects
        // that will happen when both sides call switch_to_p2p().
        flow.p2p_signaled = true;
    }

    if (both_ready) {
        // Send flow_endpoint_ready first, then flow_p2p_peer
        if (provider_signal_session) provider_signal_session->send_command(provider_ep_ready);
        if (accessor_signal_session) accessor_signal_session->send_command(accessor_ep_ready);
        if (provider_signal_session) provider_signal_session->send_command(provider_peer);
        if (accessor_signal_session) accessor_signal_session->send_command(accessor_peer);
    }
    return true;
}

bool frp_runtime_public_server::handle_p2p_upgrade_request(
    const std::shared_ptr<frp_runtime_signal_session>& session,
    const frp_runtime_p2p_upgrade_request_data& data,
    std::string& error_message) {
    if (!session) {
        error_message = "invalid session";
        return false;
    }
    if (config_.listen_udp_port == 0) {
        // Server has no UDP -- silently ignore, p2p upgrade won't proceed
        FINFO("handle_p2p_upgrade_request flow_id={} server has no UDP, ignoring", data.flow_id);
        return true;
    }

    bool both_requested = false;
    std::string provider_uuid;
    std::string accessor_uuid;
    std::uint8_t provider_nat = frp_runtime_nat_type_disabled;
    std::uint8_t accessor_nat = frp_runtime_nat_type_disabled;
    std::shared_ptr<frp_runtime_signal_session> provider_signal_session;
    std::shared_ptr<frp_runtime_signal_session> accessor_signal_session;
    frp_runtime_flow_p2p_peer_data provider_peer;
    frp_runtime_flow_p2p_peer_data accessor_peer;
    provider_peer.command = frp_runtime_flow_p2p_peer_command;
    accessor_peer.command = frp_runtime_flow_p2p_peer_command;
    frp_runtime_flow_endpoint_ready_data provider_ep_ready;
    frp_runtime_flow_endpoint_ready_data accessor_ep_ready;
    provider_ep_ready.command = frp_runtime_flow_endpoint_ready_command;
    accessor_ep_ready.command = frp_runtime_flow_endpoint_ready_command;
    {
        std::scoped_lock<std::mutex> locker(runtime_mutex_);
        auto flow_it = flows_by_id_.find(data.flow_id);
        if (flow_it == flows_by_id_.end()) {
            error_message = "unknown flow_id";
            return false;
        }
        auto& flow = flow_it->second;
        const bool is_provider = flow.provider_uuid == session->get_uuid();
        const bool is_accessor = flow.accessor_uuid == session->get_uuid();
        if (!is_provider && !is_accessor) {
            error_message = "p2p_upgrade_request uuid mismatch";
            return false;
        }
        if (is_provider) flow.provider_p2p_upgrade_requested = true;
        if (is_accessor) flow.accessor_p2p_upgrade_requested = true;

        FINFO("handle_p2p_upgrade_request flow_id={} uuid={} provider_req={} accessor_req={}",
              data.flow_id, session->get_uuid(),
              flow.provider_p2p_upgrade_requested, flow.accessor_p2p_upgrade_requested);

        if (!flow.provider_p2p_upgrade_requested || !flow.accessor_p2p_upgrade_requested) {
            return true; // wait for the other side
        }

        // Check NAT compatibility
        auto provider_it = providers_by_uuid_.find(flow.provider_uuid);
        auto accessor_it = accessors_by_uuid_.find(flow.accessor_uuid);
        if (provider_it == providers_by_uuid_.end() || accessor_it == accessors_by_uuid_.end()) {
            return true;
        }
        provider_nat = provider_it->second.nat_type;
        accessor_nat = accessor_it->second.nat_type;
        const bool accessor_p2p_capable = (accessor_nat != frp_runtime_nat_type_disabled);
        const bool provider_p2p_capable = (provider_nat != frp_runtime_nat_type_disabled);
        const bool both_symmetric = (accessor_nat == frp_runtime_nat_type_symmetric &&
                                     provider_nat == frp_runtime_nat_type_symmetric);
        if (!accessor_p2p_capable || !provider_p2p_capable || both_symmetric) {
            FINFO("handle_p2p_upgrade_request flow_id={} p2p not viable accessor_nat={} provider_nat={}",
                  data.flow_id, static_cast<int>(accessor_nat), static_cast<int>(provider_nat));
            return true; // silently skip -- relay continues
        }
        both_requested = true;
        provider_uuid = flow.provider_uuid;
        accessor_uuid = flow.accessor_uuid;
    }

    if (both_requested) {
        FINFO("handle_p2p_upgrade_request flow_id={} both sides ready, p2p upgrade coordinated", data.flow_id);

        // Check if probes already arrived before the second upgrade request.
        // If both probes are ready, build and send peer info immediately.
        bool probes_ready = false;

        {
            std::scoped_lock<std::mutex> locker(runtime_mutex_);
            auto flow_it = flows_by_id_.find(data.flow_id);
            if (flow_it == flows_by_id_.end()) return true;
            auto& flow = flow_it->second;
            if (!flow.provider_probe.ready || !flow.accessor_probe.ready) return true;
            probes_ready = true;

            const bool same_public_ip =
                flow.provider_probe.observed_endpoint.address() == flow.accessor_probe.observed_endpoint.address();
            provider_peer.flow_id      = data.flow_id;
            accessor_peer.flow_id      = data.flow_id;
            provider_peer.use_local_candidate = same_public_ip;
            accessor_peer.use_local_candidate = same_public_ip;
            if (same_public_ip && !flow.accessor_probe.local_ip.empty() && flow.accessor_probe.local_port != 0) {
                provider_peer.peer_host = flow.accessor_probe.local_ip;
                provider_peer.peer_port = flow.accessor_probe.local_port;
            } else {
                provider_peer.peer_host = flow.accessor_probe.observed_endpoint.address().to_string();
                provider_peer.peer_port = flow.accessor_probe.observed_endpoint.port();
            }
            if (same_public_ip && !flow.provider_probe.local_ip.empty() && flow.provider_probe.local_port != 0) {
                accessor_peer.peer_host = flow.provider_probe.local_ip;
                accessor_peer.peer_port = flow.provider_probe.local_port;
            } else {
                accessor_peer.peer_host = flow.provider_probe.observed_endpoint.address().to_string();
                accessor_peer.peer_port = flow.provider_probe.observed_endpoint.port();
            }
            provider_ep_ready.flow_id       = data.flow_id;
            provider_ep_ready.external_ip   = flow.provider_probe.observed_endpoint.address().to_string();
            provider_ep_ready.external_port = flow.provider_probe.observed_endpoint.port();
            accessor_ep_ready.flow_id       = data.flow_id;
            accessor_ep_ready.external_ip   = flow.accessor_probe.observed_endpoint.address().to_string();
            accessor_ep_ready.external_port = flow.accessor_probe.observed_endpoint.port();
            auto provider_it = providers_by_uuid_.find(flow.provider_uuid);
            if (provider_it != providers_by_uuid_.end()) provider_signal_session = provider_it->second.session.lock();
            auto accessor_it = accessors_by_uuid_.find(flow.accessor_uuid);
            if (accessor_it != accessors_by_uuid_.end()) accessor_signal_session = accessor_it->second.session.lock();
            provider_peer.peer_nat_type = accessor_it != accessors_by_uuid_.end() ? accessor_it->second.nat_type : static_cast<std::uint8_t>(frp_runtime_nat_type_disabled);
            accessor_peer.peer_nat_type = provider_it != providers_by_uuid_.end() ? provider_it->second.nat_type : static_cast<std::uint8_t>(frp_runtime_nat_type_disabled);
            provider_peer.peer_startup_rtt_ms = accessor_it != accessors_by_uuid_.end() ? accessor_it->second.startup_rtt_ms : 100U;
            accessor_peer.peer_startup_rtt_ms = provider_it != providers_by_uuid_.end() ? provider_it->second.startup_rtt_ms : 100U;
        }

        if (probes_ready) {
            if (provider_signal_session) provider_signal_session->send_command(provider_ep_ready);
            if (accessor_signal_session) accessor_signal_session->send_command(accessor_ep_ready);
            if (provider_signal_session) provider_signal_session->send_command(provider_peer);
            if (accessor_signal_session) accessor_signal_session->send_command(accessor_peer);
        }
    }
    return true;
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
                auto provider_it = providers_by_uuid_.find(flow.provider_uuid);
                if (provider_it != providers_by_uuid_.end()) {
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
            error_message = "transport not ready";
            return false;
        }
        if (flow.provider_data_session.lock().get() == session.get()) {
            peer_session = flow.accessor_data_session.lock();
        } else if (flow.accessor_data_session.lock().get() == session.get()) {
            if (!flow.provider_ready) {
                error_message = "provider not ready";
                return false;
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
                if (flow.p2p_signaled) {
                    // Relay release after successful P2P upgrade -- expected.
                    // Clear the matching weak_ptr but keep the flow alive.
                    // The peer's relay is still active (or will be released
                    // by its own switch_to_p2p soon).
                    if (is_provider_data) flow.provider_data_session = {};
                    if (is_accessor_data) flow.accessor_data_session = {};
                    FINFO("release_session_state p2p_signaled flow_id={} {} relay released, flow kept",
                          flow.flow_id, is_provider_data ? "provider" : "accessor");
                    ++it;
                } else {
                    if (provider_data && !is_provider_data) peer_data_sessions.push_back(provider_data);
                    if (accessor_data && !is_accessor_data) peer_data_sessions.push_back(accessor_data);
                    auto provider_it = providers_by_uuid_.find(flow.provider_uuid);
                    if (provider_it != providers_by_uuid_.end()) {
                        if (auto signal = provider_it->second.session.lock(); signal && signal.get() != session_ptr) {
                            peer_signal_sessions.push_back(signal);
                        }
                    }
                    auto accessor_it = accessors_by_uuid_.find(flow.accessor_uuid);
                    if (accessor_it != accessors_by_uuid_.end()) {
                        if (auto signal = accessor_it->second.session.lock(); signal && signal.get() != session_ptr) {
                            peer_signal_sessions.push_back(signal);
                        }
                    }
                    erased_flows.push_back(it->first);
                    it = flows_by_id_.erase(it);
                }
            } else {
                ++it;
            }
        }

        // When a signal session disconnects, clean up p2p-signaled flows
        // associated with its uuid. Relays are already released so the
        // data-session loop above won't match; we must erase explicitly
        // to prevent the flow from leaking in flows_by_id_.
        if (!session_ptr->is_data_session()) {
            const auto& uuid = session_ptr->get_uuid();
            for (auto it = flows_by_id_.begin(); it != flows_by_id_.end();) {
                auto& flow = it->second;
                if (flow.p2p_signaled && (uuid == flow.provider_uuid || uuid == flow.accessor_uuid)) {
                    if (uuid == flow.provider_uuid) {
                        auto accessor_it = accessors_by_uuid_.find(flow.accessor_uuid);
                        if (accessor_it != accessors_by_uuid_.end()) {
                            if (auto signal = accessor_it->second.session.lock()) {
                                peer_signal_sessions.push_back(signal);
                            }
                        }
                    } else {
                        auto provider_it = providers_by_uuid_.find(flow.provider_uuid);
                        if (provider_it != providers_by_uuid_.end()) {
                            if (auto signal = provider_it->second.session.lock()) {
                                peer_signal_sessions.push_back(signal);
                            }
                        }
                    }
                    erased_flows.push_back(it->first);
                    it = flows_by_id_.erase(it);
                } else {
                    ++it;
                }
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
    if (session->get_role() == frp_runtime_provider_role) {
        clear_provider_state_locked(session->get_uuid());
    } else if (session->get_role() == frp_runtime_accessor_role) {
        clear_accessor_state_locked(session->get_uuid());
    }
    // Note: callers manage sessions_by_uuid_ erasure to avoid iterator invalidation during iteration
}

void frp_runtime_public_server::clear_provider_state_locked(const std::string& uuid) {
    auto provider_it = providers_by_uuid_.find(uuid);
    if (provider_it == providers_by_uuid_.end()) return;
    if (auto services_it = services_by_register_key_.find(provider_it->second.register_key);
        services_it != services_by_register_key_.end()) {
        for (const auto& service_name : provider_it->second.services) {
            services_it->second.erase(service_name);
        }
        if (services_it->second.empty()) {
            services_by_register_key_.erase(services_it);
        }
    }
    providers_by_uuid_.erase(provider_it);
}

void frp_runtime_public_server::clear_accessor_state_locked(const std::string& uuid) {
    accessors_by_uuid_.erase(uuid);
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
          static_cast<int>(mode_), static_cast<int>(role_));
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
    case frp_runtime_join_request_command: {
        frp_runtime_join_request_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        handle_join_phase(request);
        return;
    }
    case frp_runtime_register_services_request_command: {
        frp_runtime_register_services_request_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        handle_register_services_phase(request);
        return;
    }
    case frp_runtime_fetch_services_request_command: {
        handle_fetch_services_phase();
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
    case frp_runtime_flow_data_command: {
        frp_runtime_flow_data_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        handle_flow_data_phase(request);
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
    case frp_runtime_ping_request_command: {
        frp_runtime_ping_response_data response;
        response.command = frp_runtime_ping_response_command;
        send_command(response);
        read_next_command();
        return;
    }
    case frp_runtime_p2p_upgrade_request_command: {
        frp_runtime_p2p_upgrade_request_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            release_obj();
            return;
        }
        handle_p2p_upgrade_request_phase(request);
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

void frp_runtime_signal_session::handle_join_phase(const frp_runtime_join_request_data& request) {
    frp_runtime_join_response_data response;
    response.command = frp_runtime_join_response_command;
    std::string error_message;
    response.ok      = owner_ && owner_->bind_signal_identity(shared_from_this(), request, error_message);
    response.message = response.ok ? "ok" : error_message;
    if (response.ok) {
        role_         = static_cast<role_type>(request.role);
        uuid_         = request.uuid;
        register_key_ = request.register_key;
        nat_type_     = request.nat_type;
        joined_       = true;
        FINFO("signal_session join_request role={} uuid={} register_key={} nat_type={}",
              request.role == frp_runtime_provider_role ? "provider" : "accessor",
              uuid_, register_key_, static_cast<int>(nat_type_));
    }
    send_command(response);
    if (!response.ok) {
        release_obj();
        return;
    }
    read_next_command();
}

void frp_runtime_signal_session::handle_register_services_phase(
    const frp_runtime_register_services_request_data& request) {
    frp_runtime_register_services_response_data response;
    response.command = frp_runtime_register_services_response_command;
    std::string error_message;
    response.ok      = owner_ && owner_->register_provider_services(shared_from_this(), request.services, error_message);
    response.message = response.ok ? "ok" : error_message;
    send_command(response);
    if (!response.ok) {
        release_obj();
        return;
    }
    FINFO("provider {} registered {} services for register_key={}", uuid_, request.services.size(), register_key_);
    read_next_command();
}

void frp_runtime_signal_session::handle_fetch_services_phase() {
    frp_runtime_fetch_services_response_data response;
    response.command = frp_runtime_fetch_services_response_command;
    std::string error_message;
    if (owner_) {
        response.services = owner_->list_services_for_accessor(shared_from_this(), error_message);
        response.ok       = error_message.empty();
        response.message  = response.ok ? "ok" : error_message;
    } else {
        response.ok      = false;
        response.message = "server released";
    }
    send_command(response);
    if (!response.ok) {
        release_obj();
        return;
    }
    read_next_command();
}

void frp_runtime_signal_session::handle_create_flow_phase(const frp_runtime_create_flow_request_data& request) {
    FINFO("signal_session create_flow_request uuid={} service_name={} transport=tcp_relay",
          uuid_, request.service_name);
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

void frp_runtime_signal_session::handle_flow_data_phase(const frp_runtime_flow_data_data& request) {
    std::string error_message;
    if (!owner_ || !owner_->forward_flow_data(shared_from_this(), request, error_message)) {
        FERR("flow_data rejected uuid={} flow_id={} err={}", uuid_, request.flow_id, error_message);
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

void frp_runtime_signal_session::handle_p2p_upgrade_request_phase(const frp_runtime_p2p_upgrade_request_data& request) {
    std::string error_message;
    if (!owner_ || !owner_->handle_p2p_upgrade_request(shared_from_this(), request, error_message)) {
        FERR("p2p_upgrade_request rejected uuid={} flow_id={} err={}", uuid_, request.flow_id, error_message);
        // Non-fatal: relay continues, just skip p2p upgrade
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

} // namespace network::proxy
