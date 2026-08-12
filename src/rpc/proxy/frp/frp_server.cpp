#include "frp_server.hpp"

#include "frp_common.hpp"
#include "frp_kcp_crypto.hpp"

#include "fundamental/basic/utils.hpp"

#include "network/async_utils.hpp"

namespace network::proxy
{

frp_public_server::frp_public_server(frp_public_server_config config) :
config_(std::move(config)), acceptor_(io_context_pool::Instance().get_io_context()) {
    protocal_helper::init_acceptor(acceptor_, config_.listen_tcp_port);
    for (auto& key : config_.allowed_register_keys) {
        allowed_register_keys_cache.emplace(key, std::unordered_set<std::string> {});
    }
    start_udp_servers();
    configure_ssl();
}

void frp_public_server::start() {
    bool expected = false;
    if (!has_started_.compare_exchange_strong(expected, true)) return;
    io_context_pool::Instance().reg_object(this,
        [self = shared_from_this()]() { self->release_obj(); });
    FINFO("start frp runtime public server on {}:{}", acceptor_.local_endpoint().address().to_string(),
          acceptor_.local_endpoint().port());
    do_accept();
    for (std::size_t i = 0; i < udp_servers_.size(); ++i) {
        start_udp_receive(i);
    }
}

void frp_public_server::start_udp_servers() {
    udp_servers_.clear();
    if (config_.listen_udp_port == 0) return;
    for (std::uint16_t port : { config_.listen_udp_port, static_cast<std::uint16_t>(config_.listen_udp_port + 1) }) {
        auto server = std::make_shared<udp_server_state>(io_context_pool::Instance().get_io_context().get_executor());
        auto ec     = protocal_helper::udp_bind_endpoint(server->socket, port);
        if (ec) {
            throw std::invalid_argument(
                Fundamental::StringFormat("bind udp port:{} failed err:{}", port, ec.message()));
        }
        FINFO("start_udp_servers bound udp port={}", port);
        udp_servers_.push_back(server);
    }
}

void frp_public_server::start_udp_receive(std::size_t index) {
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

                // All UDP packets are encrypted with the shared traffic key
                auto traffic_key = frp_derive_kcp_key(config_.traffic_secret);
                auto plaintext   = frp_kcp_decrypt(traffic_key, encrypted_packet);
                if (!plaintext) {
                    FINFO("udp_server failed to decrypt packet from {}:{} size={}",
                          current_endpoint.address().to_string(), current_endpoint.port(), bytes_read);
                    start_udp_receive(index);
                    return;
                }

                std::string payload(plaintext->begin(), plaintext->end());

                frp_command_base command;
                if (!Fundamental::io::from_json(payload, command)) {
                    start_udp_receive(index);
                    return;
                }

                // Time sync request: NTP-like clock sync via UDP
                if (command.command == frp_time_sync_request_command) {
                    frp_time_sync_request_data req;
                    if (!Fundamental::io::from_json(payload, req)) {
                        start_udp_receive(index);
                        return;
                    }
                    std::int64_t T2 = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count();
                    frp_time_sync_response_data resp;
                    resp.command        = frp_time_sync_response_command;
                    resp.seq            = req.seq;
                    resp.client_send_ts = req.client_send_ts;
                    resp.server_recv_ts = T2;
                    resp.server_send_ts = std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count();
                    auto resp_json      = Fundamental::io::to_json(resp);
                    auto encrypted_resp = frp_kcp_encrypt_string(traffic_key, resp_json);
                    if (!encrypted_resp.empty()) {
                        auto resp_buf = std::make_shared<std::vector<std::uint8_t>>(std::move(encrypted_resp));
                        server->socket.async_send_to(asio::buffer(*resp_buf), current_endpoint,
                                                     [resp_buf](const std::error_code&, std::size_t) {});
                    }
                    start_udp_receive(index);
                    return;
                }

                if (command.command != frp_p2p_probe_command) {
                    start_udp_receive(index);
                    return;
                }
                frp_p2p_probe_data probe;
                if (!Fundamental::io::from_json(payload, probe)) {
                    start_udp_receive(index);
                    return;
                }

                // Echo back external endpoint (both startup and endpoint probe)
                frp_udp_echo_data echo;
                echo.command        = frp_udp_echo_command;
                echo.external_ip    = current_endpoint.address().to_string();
                echo.external_port  = current_endpoint.port();
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

void frp_public_server::release_obj() {
    reference_.release();
    bool expected = true;
    if (!has_started_.compare_exchange_strong(expected, false)) return;
    io_context_pool::Instance().unreg_object(this);
    asio::post(acceptor_.get_executor(), [this, self = shared_from_this()]() {
        std::error_code ec;
        acceptor_.close(ec);
        for (auto& server : udp_servers_) {
            if (!server) continue;
            server->socket.close(ec);
        }
        decltype(sessions_by_uuid_) copy_sessions;
        {
            std::scoped_lock<std::mutex> locker(mutex_);
            copy_sessions = std::move(sessions_by_uuid_);
        }
        for (auto& item : copy_sessions) {
            for (auto& session : item.second) {
                auto strong = session.second.lock();
                if (strong) strong->release_obj();
            }
        }
    });
}

bool frp_public_server::verify_auth_digest(std::string_view nonce, std::string_view digest) const {
    return frp_hmac_sha256_hex(config_.traffic_secret, nonce) == digest;
}

void frp_public_server::do_accept() {
    acceptor_.async_accept(io_context_pool::Instance().get_io_context(),
                           [this, self = shared_from_this()](asio::error_code ec, asio::ip::tcp::socket socket) {
                               if (!reference_.is_valid() || !acceptor_.is_open()) return;
                               if (!ec) {
                                   auto session = frp_signal_session::make_shared(std::move(socket), self);
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

void frp_public_server::configure_ssl() {
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

void frp_public_server::remove_session(const std::string& uuid, const std::string& connection_uuid) {
    std::list<std::weak_ptr<frp_signal_session>> removed_session;
    {
        std::scoped_lock<std::mutex> locker(mutex_);
        auto iter = sessions_by_uuid_.find(uuid);

        if (iter == sessions_by_uuid_.end()) return;
        if (uuid == connection_uuid) {
            for (auto& item : iter->second) {
                if (item.first == uuid) {
                    auto session = item.second.lock();
                    if (!session) continue;
                    // update key cache
                    for (auto& svc_group : session->groups) {
                        auto& group_cache = allowed_register_keys_cache[svc_group.register_key];
                        for (auto& svc : svc_group.services) {
                            group_cache.erase(svc.service_name);
                        }
                    }
                }
                removed_session.emplace_back(item.second);
            }
            sessions_by_uuid_.erase(iter);
        } else {
            auto removed_iter = iter->second.find(connection_uuid);
            if (removed_iter == iter->second.end()) return;
            removed_session.emplace_back(removed_iter->second);
            iter->second.erase(removed_iter);
        }
    }
    for (auto& item : removed_session) {
        auto session = item.lock();
        if (!session) continue;
        session->release_obj();
    }
}

frp_signal_session::frp_signal_session(::asio::ip::tcp::socket&& socket,
                                       std::shared_ptr<frp_public_server> owner) :
socket_(std::move(socket)), executor_(socket_.get_executor()), owner_(std::move(owner)), timeout_timer_(executor_) {
    enable_tcp_keep_alive(socket_);
    io_context_pool::Instance().reg_timer(timeout_timer_);
}

frp_signal_session::~frp_signal_session() {
    io_context_pool::Instance().unreg_timer(timeout_timer_);
    io_context_pool::Instance().unreg_object(this);
}

void frp_signal_session::start() {
    if (!reference_.is_valid()) return;
    io_context_pool::Instance().reg_object(this,
        [self = shared_from_this()]() { self->release_obj(); });
#ifndef NETWORK_DISABLE_SSL
    if (ssl_context_ref_) {
        ssl_handshake();
        return;
    }
#endif
    start_protocol();
}

void frp_signal_session::release_obj() {
    if (!reference_.release()) return;
    // conn= 打印 connection_uuid_：signal session（=自身 uuid）与中继 session（=中继 id）可区分
    FINFO("signal_session release_obj uuid={} conn={} mode={}", uuid_, connection_uuid_, static_cast<int>(mode_));
    io_context_pool::Instance().unreg_object(this);
    // 三层模型（规范 §4.2）：入口只投递，关闭序列在绑定 io 线程执行——
    // 不再在任意调用者线程直接 cancel 定时器/触发用户回调
    network::post_keepalive(executor_, shared_from_this(),
        [](const std::shared_ptr<frp_signal_session>& self) {
            self->timeout_timer_.cancel();
            self->owner_->remove_session(self->uuid_, self->connection_uuid_);
            if (self->release_cb_) self->release_cb_();
            self->close_socket();
        });
}

#ifndef NETWORK_DISABLE_SSL
void frp_signal_session::enable_ssl(asio::ssl::context& ssl_context) {
    ssl_context_ref_ = &ssl_context;
}
#endif

void frp_signal_session::enable_timeout(std::uint32_t timeout_sec) {
    timeout_sec_ = timeout_sec;
    reset_timeout_timer();
}

void frp_signal_session::do_write() {
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

void frp_signal_session::start_protocol() {
    auto using_timeout_sec = owner_->config_.data_channel_idle_timeout_seconds / 4;
    if (using_timeout_sec < 15) using_timeout_sec = 15;
    enable_timeout(using_timeout_sec);
    read_next_command();
}

void frp_signal_session::read_next_command() {
    FASSERT(io_context_pool::Instance().running_in_io_thread(), "read_next_command must run on io thread");
    auto read_payload = [this, self = shared_from_this()](std::error_code ec, std::size_t) {
        if (!reference_.is_valid()) return;
        if (ec) {
            release_obj();
            return;
        }
        reset_timeout_timer();
        process_command(payload_);
    };

    auto read_header = [this, self = shared_from_this(), read_payload](std::error_code ec, std::size_t) {
        if (!reference_.is_valid()) return;
        if (ec) {
            release_obj();
            return;
        }
        reset_timeout_timer();
        std::uint32_t payload_len = 0;
        Fundamental::net_buffer_copy(header_buf_.data(), &payload_len, 4);
        if (payload_len == 0 || payload_len > frp_command_base::kMaxCommandPayloadLen) {
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
    reset_timeout_timer();
#ifndef NETWORK_DISABLE_SSL
    if (ssl_stream_) {
        asio::async_read(*ssl_stream_, asio::buffer(header_buf_.data(), header_buf_.size()), read_header);
        return;
    }
#endif
    asio::async_read(socket_, asio::buffer(header_buf_.data(), header_buf_.size()), read_header);
}

void frp_signal_session::process_command(std::string payload) {
    FASSERT(io_context_pool::Instance().running_in_io_thread(), "process_command must run on io thread");
    frp_command_base base_command;
    if (!Fundamental::io::from_json(payload, base_command)) {
        FWARN("signal_session failed to parse command uuid={} mode={}", uuid_, static_cast<int>(mode_));
        release_obj();
        return;
    }
    if (mode_ == session_mode::undecided) {
        handle_initial_phase(base_command, std::move(payload));
        return;
    }
    if (mode_ == session_mode::data) {
        FWARN("signal_session data channel received unexpected command uuid={}", uuid_);
        release_obj();
        return;
    }
    enable_timeout(owner_->config_.data_channel_idle_timeout_seconds);
    if (!authenticated_) {
        handle_server_hello_phase(base_command, std::move(payload));
        return;
    }
    handle_authenticated_phase(base_command, std::move(payload));
}

void frp_signal_session::handle_initial_phase(const frp_command_base& command, std::string payload) {
    switch (command.command) {
    case frp_signal_open_command: {
        mode_         = session_mode::signal;
        server_nonce_ = frp_generate_server_nonce();
        FINFO("signal_session signal_open received from {}", socket_.remote_endpoint().address().to_string());
        frp_server_hello_data hello;
        hello.command      = frp_server_hello_command;
        hello.server_nonce = server_nonce_;
        send_command(hello);
        read_next_command();
        return;
    }
    case frp_channel_open_command: {
        frp_channel_open_request_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            FWARN("signal_session failed to parse channel_open payload");
            release_obj();
            return;
        }
        if (request.connection_uuid == request.dst_uuid || request.connection_uuid == request.from_uuid ||
            request.dst_uuid == request.from_uuid) {
            FWARN("signal_session rejected channel_open from={} to={} connection_id={} status={}", request.from_uuid,
                  request.dst_uuid, request.connection_uuid, request.status);
            release_obj();
            return;
        }
        FINFO("signal_session channel_open received from={} to={} connection_id={} status={}", request.from_uuid,
              request.dst_uuid, request.connection_uuid, request.status);
        handle_channel_open_phase(request);
        return;
    }
    default:
        handle_unknown_command(command.command, "initial");
        return;
    }
}

void frp_signal_session::handle_server_hello_phase(const frp_command_base& command, std::string payload) {
    if (command.command != frp_auth_request_command) {
        FWARN("signal_session expected auth_request but got command={}", command.command);
        send_auth_failure_and_close("expected auth_request");
        return;
    }
    frp_auth_request_data request;
    if (!Fundamental::io::from_json(payload, request)) {
        FWARN("signal_session failed to parse auth_request payload");
        send_auth_failure_and_close("invalid auth_request");
        return;
    }
    FINFO("signal_session auth_request received nonce={}", server_nonce_);
    frp_auth_response_data response;
    response.command = frp_auth_response_command;
    response.ok      = owner_->verify_auth_digest(server_nonce_, request.digest);
    response.message = response.ok ? "ok" : "auth failed";
    send_command(response);
    if (!response.ok) {
        FWARN("signal_session auth failed digest mismatch uuid={}", uuid_);
        release_obj();
        return;
    }
    authenticated_ = true;
    read_next_command();
}

void frp_signal_session::handle_authenticated_phase(const frp_command_base& command, std::string payload) {
    switch (command.command) {
    case frp_register_services_command: {
        frp_register_services_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            FWARN("signal_session failed to parse register_services payload uuid={}", uuid_);
            release_obj();
            return;
        }
        handle_register_services_phase(request);
        return;
    }
    case frp_forward_command: {
        frp_forward_command_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            FWARN("signal_session failed to parse forward_command payload uuid={}", uuid_);
            release_obj();
            return;
        }
        owner_->forward_data(request.dst_uuid, std::move(request.payload));
        read_next_command();
        return;
    }
    case frp_subscribe_services_command: {
        frp_subscribe_services_data request;
        if (!Fundamental::io::from_json(payload, request)) {
            FWARN("signal_session failed to parse subscribe_services payload uuid={}", uuid_);
            release_obj();
            return;
        }
        handle_subscribe_services_phase(request);
        return;
    }
    case frp_signal_ping_command: {
        // 信令保活探测：回 pong；read_next_command 内部重置空闲超时
        frp_command_base pong;
        pong.command = frp_signal_pong_command;
        send_command(pong);
        read_next_command();
        return;
    }
    default:
        handle_unknown_command(command.command, "authenticated");
        return;
    }
}

void frp_signal_session::handle_unknown_command(std::uint32_t command, const char* phase) {
    ++bad_command_cnt_;
    FWARN("signal_session unknown command={} in {} phase uuid={} bad_cnt={}", command, phase, uuid_, bad_command_cnt_);
    if (bad_command_cnt_ >= kMaxBadCommandCount) {
        // 累计异常命令过多：释放防滥用（正常/新版客户端不会触发）
        release_obj();
        return;
    }
    // 忽略并继续读取：服务端不作为重连风暴的主动方，同时兼容未知的新命令类型
    read_next_command();
}

void frp_signal_session::send_auth_failure_and_close(const std::string& message) {
    frp_auth_response_data response;
    response.command = frp_auth_response_command;
    response.ok      = false;
    response.message = message;
    send_command(response);
    release_obj();
}

void frp_signal_session::start_data_forward_read_loop() {
    reset_timeout_timer();
    auto handler = [this, self = shared_from_this()](const asio::error_code& ec, std::size_t bytes_read) {
        if (!reference_.is_valid()) return;
        if (ec) {
            FWARN("signal_session data forward read error uuid={} connection={} ec={}", uuid_, connection_uuid_,
                  ec.message());
            release_obj();
            return;
        }

        if (!forward_cb_(raw_read_buf_.data(), bytes_read)) {
            FWARN("signal_session data forward failed uuid={} connection={} peer lost", uuid_, connection_uuid_);
            release_obj();
            return;
        }
        start_data_forward_read_loop();
    };
#ifndef NETWORK_DISABLE_SSL
    if (ssl_stream_) {
        ssl_stream_->async_read_some(network_read_buffer_t(raw_read_buf_.data(), raw_read_buf_.size()),
                                     std::move(handler));
        return;
    }
#endif
    socket_.async_read_some(network_read_buffer_t(raw_read_buf_.data(), raw_read_buf_.size()), std::move(handler));
}

void frp_signal_session::send_raw(const std::shared_ptr<std::string>& packet) {
    if (!packet || packet->empty()) return;
    asio::post(executor_, [this, self = shared_from_this(), packet]() mutable {
        if (!reference_.is_valid()) return;
        write_queue_.push_back(packet);
        if (write_queue_.size() == 1) {
            do_write();
        }
    });
}

void frp_signal_session::send_raw(std::string packet) {
    if (packet.empty()) return;
    send_raw(std::make_shared<std::string>(std::move(packet)));
}

void frp_signal_session::send_raw(const void* data, std::size_t len) {
    if (len == 0 || !data) return;
    send_raw(std::make_shared<std::string>(static_cast<const char*>(data), len));
}

void frp_signal_session::upgrade(const upgrade_forward_function& forward_function,
                                 const upgrade_release_function& release_function) {
    forward_cb_ = forward_function;
    release_cb_ = release_function;
}

void frp_signal_session::reset_timeout_timer() {
    timeout_timer_.cancel();
    if (timeout_sec_ == 0) return;
    timeout_timer_.expires_after(std::chrono::milliseconds(timeout_sec_ * 1000));
    timeout_timer_.async_wait([this, self = shared_from_this()](const asio::error_code& ec) {
        if (!reference_.is_valid()) {
            return;
        }
        if (ec) {
            return;
        }
        try {
            FERR("broken timeout data session group:{} connection:{}", uuid_, connection_uuid_);
            release_obj();
        } catch (...) {
        }
    });
}

void frp_signal_session::close_socket() {
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

void frp_signal_session::ssl_handshake() {
#ifndef NETWORK_DISABLE_SSL
    ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket_, *ssl_context_ref_);
    ssl_stream_->async_handshake(asio::ssl::stream_base::server,
                                 [this, self = shared_from_this()](const asio::error_code& ec) {
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

bool frp_public_server::register_client_services(frp_signal_session& session,
                                                 const frp_register_services_data& request,
                                                 std::string& error_message) {
    {
        remove_session(request.uuid, request.uuid);
        std::scoped_lock<std::mutex> locker(mutex_);
        session.uuid_            = request.uuid;
        session.connection_uuid_ = request.uuid;
        session.nat_type_        = request.nat_type;
        session.startup_rtt_ms   = request.startup_rtt_ms;
        session.groups           = request.groups;
        auto new_cache           = allowed_register_keys_cache;
        for (const auto& group : request.groups) {
            auto iter = new_cache.find(group.register_key);
            if (iter == new_cache.end()) {
                error_message = "register_key not allowed: " + group.register_key;
                FWARN("register_client_services uuid={} rejected register_key={}", request.uuid, group.register_key);
                return false;
            }
            // verify sevices' name
            for (auto& svc : group.services) {
                if (!iter->second.insert(svc.service_name).second) {
                    error_message = "register_service not allowed: " + svc.service_name;
                    FWARN("register_client_services uuid={} duplicate service_name={} key={}", request.uuid,
                          svc.service_name, group.register_key);
                    return false;
                }
            }
        }
        allowed_register_keys_cache = std::move(new_cache);
        sessions_by_uuid_[request.uuid].emplace(request.uuid, session.shared_from_this());
    }
    FINFO("client {} registered services groups={}", request.uuid, request.groups.size());
    return true;
}

void frp_public_server::register_data_channel(frp_signal_session& session,
                                              const frp_channel_open_request_data& request) {
    bool connection_need_cleared = true;
    do {
        Fundamental::ScopeGuard release_g([&]() {
            if (!connection_need_cleared) return;
            remove_session(request.dst_uuid, request.connection_uuid);
            remove_session(request.from_uuid, request.connection_uuid);
        });
        std::scoped_lock<std::mutex> locker(mutex_);
        auto iter1 = sessions_by_uuid_.find(request.from_uuid);
        if (iter1 == sessions_by_uuid_.end()) {
            FWARN("register_data_channel from_uuid={} not found in sessions", request.from_uuid);
            break;
        }
        auto iter2 = sessions_by_uuid_.find(request.dst_uuid);
        if (iter2 == sessions_by_uuid_.end()) {
            FWARN("register_data_channel dst_uuid={} not found in sessions", request.dst_uuid);
            break;
        }
        auto& src           = iter1->second;
        auto& dst           = iter2->second;
        auto& connection_id = request.connection_uuid;
        if (request.status == 0) { // channel open request phase 1
            if (src.count(connection_id) > 0 || dst.count(connection_id) > 0) {
                FWARN("register_data_channel connection_id={} already exists from={} to={} status=0", connection_id,
                      request.from_uuid, request.dst_uuid);
                connection_need_cleared = false;
                return;
            }
            auto connection_iter = dst.find(request.dst_uuid);
            if (connection_iter == dst.end()) {
                FWARN("register_data_channel dst signal session not found dst_uuid={}", request.dst_uuid);
                break;
            }
            auto dst_session = connection_iter->second.lock();
            if (!dst_session) {
                FWARN("register_data_channel dst signal session expired dst_uuid={}", request.dst_uuid);
                break;
            }
            session.uuid_            = request.from_uuid;
            session.connection_uuid_ = request.connection_uuid;
            // record this session
            src[connection_id] = session.shared_from_this();
            // notify other side — frame payload with 4-byte header for signal channel
            {
                auto framed = std::make_shared<std::string>();
                std::uint32_t len = static_cast<std::uint32_t>(request.payload.size());
                framed->resize(4 + len);
                Fundamental::net_buffer_copy(&len, framed->data(), 4);
                std::memcpy(framed->data() + 4, request.payload.data(), len);
                dst_session->send_raw(framed);
            }
            connection_need_cleared = false;
        } else if (request.status == 1) {
            if (src.count(connection_id) > 0 || dst.count(connection_id) == 0) {
                FWARN("register_data_channel status=1 mismatch src_has={} dst_has={} connection={} from={} to={}",
                      src.count(connection_id), dst.count(connection_id), connection_id, request.from_uuid,
                      request.dst_uuid);
                connection_need_cleared = false;
                return;
            }
            auto connection_iter = dst.find(request.connection_uuid);
            if (connection_iter == dst.end()) {
                FWARN("register_data_channel dst connection not found dst_uuid={} connection={}", request.dst_uuid,
                      connection_id);
                break;
            }
            auto dst_session = connection_iter->second.lock();
            if (!dst_session) {
                FWARN("register_data_channel dst connection expired dst_uuid={} connection={}", request.dst_uuid,
                      connection_id);
                break;
            }
            session.uuid_            = request.from_uuid;
            session.connection_uuid_ = request.connection_uuid;
            // record this session
            src[connection_id] = session.shared_from_this();
            // Notify accessor via its signal channel (dst is accessor's uuid since
            // status=1 from=provider to=accessor). Frame with 4-byte header so the
            // receiving signal channel can parse it via read_next_command.
            {
                auto& dst_map = dst; // dst = sessions_by_uuid_[accessor_uuid]
                auto sig_iter = dst_map.find(request.dst_uuid);
                if (sig_iter != dst_map.end()) {
                    if (auto sig = sig_iter->second.lock()) {
                        auto framed = std::make_shared<std::string>();
                        auto& pl    = request.payload;
                        std::uint32_t len = static_cast<std::uint32_t>(pl.size());
                        framed->resize(4 + len);
                        Fundamental::net_buffer_copy(&len, framed->data(), 4);
                        std::memcpy(framed->data() + 4, pl.data(), len);
                        sig->send_raw(framed);
                    }
                }
            }
            // set timeout（src 会话自己的线程）
            session.enable_timeout(config_.data_channel_idle_timeout_seconds);
            // upgrade src → 转发到 dst
            auto w_s1 = dst_session->weak_from_this();
            session.upgrade(
                [forward_session = w_s1](const void* data, std::size_t len) -> bool {
                    auto strong = forward_session.lock();
                    if (!strong) return false;
                    strong->send_raw(data, len);
                    return true;
                },
                [uuid_group = dst_session->uuid_, connection_id, this, ptr = weak_from_this()]() {
                    auto strong = ptr.lock();
                    if (!strong) return;
                    strong->remove_session(uuid_group, connection_id);
                });
            session.start_data_forward_read_loop();
            // dst 会话的全部操作投递到其绑定 io 线程（规范 §3.1/3.3）：
            // enable_timeout/upgrade/start_data_forward_read_loop 禁止跨线程直接调用
            auto dst_ex       = dst_session->executor_;
            auto dst_strong   = dst_session;
            auto server_weak  = weak_from_this();
            auto timeout_sec  = config_.data_channel_idle_timeout_seconds;
            auto src_uuid_grp = session.uuid_;
            auto src_weak     = session.weak_from_this();
            network::post_keepalive(std::move(dst_ex), dst_strong,
                [src_weak, src_uuid_grp = std::move(src_uuid_grp),
                 connection_id, server_weak = std::move(server_weak), timeout_sec]
                (const std::shared_ptr<frp_signal_session>& dst) {
                    dst->enable_timeout(timeout_sec);
                    dst->upgrade(
                        [forward_session = src_weak](const void* data, std::size_t len) -> bool {
                            auto strong = forward_session.lock();
                            if (!strong) return false;
                            strong->send_raw(data, len);
                            return true;
                        },
                        [src_uuid_grp, connection_id, server_weak]() {
                            auto strong = server_weak.lock();
                            if (!strong) return;
                            strong->remove_session(src_uuid_grp, connection_id);
                        });
                    dst->start_data_forward_read_loop();
                });
            connection_need_cleared = false;
        }
    } while (0);
}

std::vector<frp_visible_service_data> frp_public_server::list_services_for_subscriber(
    frp_signal_session& session,
    const std::vector<std::string>& register_keys,
    std::string& error_message) const {
    std::vector<frp_visible_service_data> result;
    std::scoped_lock<std::mutex> locker(mutex_);
    for (const auto& [uuid, connections] : sessions_by_uuid_) {
        if (uuid == session.get_uuid()) continue;
        auto iter = connections.find(uuid);
        if (iter == connections.end()) continue;
        auto data_channel = iter->second.lock();
        if (!data_channel) {
            FWARN("list_services_for_subscriber expired session uuid={}", uuid);
            continue;
        }
        auto& group_data = data_channel->groups;
        for (const auto& key : register_keys) {
            for (const auto& svc : group_data) {
                if (svc.register_key == key) {
                    for (auto& item : svc.services) {
                        frp_visible_service_data v;
                        v.service_name            = item.service_name;
                        v.provider_uuid           = uuid;
                        v.provider_nat_type       = data_channel->nat_type_;
                        v.service_type            = item.service_type;
                        v.enable_p2p              = item.enable_p2p;
                        v.provider_startup_rtt_ms = data_channel->startup_rtt_ms;
                        result.push_back(std::move(v));
                    }
                }
            }
        }
    }
    return result;
}

void frp_public_server::forward_data(const std::string& uuid, std::string packet) {
    std::unique_lock<std::mutex> locker(mutex_);
    auto iter = sessions_by_uuid_.find(uuid);
    if (iter == sessions_by_uuid_.end()) {
        FWARN("forward_data target uuid={} not found in sessions", uuid);
        return;
    }
    auto iter2 = iter->second.find(uuid);
    if (iter2 == iter->second.end()) {
        FWARN("forward_data target uuid={} signal session not found", uuid);
        return;
    }
    auto session = iter2->second.lock();
    if (!session) {
        FWARN("forward_data target uuid={} signal session expired", uuid);
        return;
    }
    auto framed = std::make_shared<std::string>();
    std::uint32_t len = static_cast<std::uint32_t>(packet.size());
    framed->resize(4 + len);
    Fundamental::net_buffer_copy(&len, framed->data(), 4);
    std::memcpy(framed->data() + 4, packet.data(), len);
    session->send_raw(framed);
}

void frp_signal_session::handle_register_services_phase(const frp_register_services_data& request) {
    frp_register_services_resp_data resp;
    resp.command = frp_register_services_resp_command;
    std::string em;
    resp.ok      = owner_->register_client_services(*this, request, em);
    resp.message = resp.ok ? "ok" : em;
    // uuid_/nat_type_/groups 已在 register_client_services 内锁内写入（P2-19：勿在锁外重复写）
    send_command(resp);
    read_next_command();
}

void frp_signal_session::handle_subscribe_services_phase(const frp_subscribe_services_data& request) {
    frp_subscribe_services_resp_data resp;
    resp.command = frp_subscribe_services_resp_command;
    std::string em;
    auto svcs = owner_->list_services_for_subscriber(*this, request.register_keys, em);
    if (!em.empty()) {
        resp.ok      = false;
        resp.message = em;
    } else {
        resp.ok       = true;
        resp.services = std::move(svcs);
    }
    send_command(resp);
    read_next_command();
}

void frp_signal_session::handle_channel_open_phase(const frp_channel_open_request_data& request) {
    mode_ = session_mode::data;
    owner_->register_data_channel(*this, request);
}

} // namespace network::proxy
