#include "frp_client.hpp"

#include "frp_common.hpp"
#include "frp_kcp_crypto.hpp"

#include "fundamental/basic/utils.hpp"
#include "fundamental/basic/uuid_utils.hpp"

#include <algorithm>
#include <cstdio>
#include <openssl/sha.h>

namespace network::proxy
{

namespace
{

std::string sha256_hex(std::string_view input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
    return Fundamental::Utils::BufferToHex(hash, SHA256_DIGEST_LENGTH);
}

std::optional<asio::ip::udp::endpoint> resolve_udp_endpoint(const asio::any_io_executor& executor,
                                                            const std::string& host, std::uint16_t port) {
    std::error_code ec;
    auto addr = asio::ip::make_address(host, ec);
    if (!ec) {
        if (addr.is_v6()) return std::nullopt;
        return asio::ip::udp::endpoint(addr, port);
    }
    asio::ip::udp::resolver resolver(executor);
    auto eps = resolver.resolve(asio::ip::udp::v4(), host, std::to_string(port), ec);
    if (ec) return std::nullopt;
    auto it = eps.begin();
    if (it == eps.end()) return std::nullopt;
    return it->endpoint();
}

} // namespace

// =============================================================================
// frp_tcp_channel
// =============================================================================

frp_tcp_channel::frp_tcp_channel(const asio::any_io_executor& executor,
                                 const std::string& host, const std::string& service) :
host_(host), service_(service), executor_(executor), socket_(executor_), resolver_(executor_) {}
frp_tcp_channel::~frp_tcp_channel() = default;

void frp_tcp_channel::set_on_release(std::function<void()> cb) { on_release_ = std::move(cb); }

void frp_tcp_channel::release_obj() {
    if (!reference_.release()) return; // 幂等：重复调用直接返回（规范 §4.1）
    asio::post(executor_, [this, ref = shared_from_this()] {
        close();
        if (on_release_) on_release_(); // 传输死亡通知归属通道，收敛到统一关闭序列
    });
}

std::string frp_tcp_channel::local_endpoint_string() const {
    std::error_code ec;
    if (!socket_.is_open()) return "unknown";
    auto ep = socket_.local_endpoint(ec);
    if (ec) return "unknown";
    return Fundamental::StringFormat("{}:{}", ep.address().to_string(), ep.port());
}
std::string frp_tcp_channel::remote_endpoint_string() const {
    std::error_code ec;
    if (!socket_.is_open()) return "unknown";
    auto ep = socket_.remote_endpoint(ec);
    if (ec) return "unknown";
    return Fundamental::StringFormat("{}:{}", ep.address().to_string(), ep.port());
}

void frp_tcp_channel::start_async_connect() {
    resolver_.async_resolve(host_, service_,
        [this, ptr = shared_from_this()](const std::error_code& ec, const decltype(resolver_)::results_type& eps) {
            if (!reference_.is_valid()) return;
            if (ec) { notify_connect_result(Fundamental::error_code(ec, "resolve failed"), shared_from_this()); return; }
            asio::async_connect(socket_, eps,
                [this, ptr](const std::error_code& ec2, const asio::ip::tcp::endpoint&) {
                    if (!reference_.is_valid()) return;
                    if (ec2) { notify_connect_result(Fundamental::error_code(ec2, "connect failed"), shared_from_this()); return; }
                    enable_tcp_keep_alive(socket_);
                    handle_transfer_ready();
                });
        });
}
void frp_tcp_channel::enable_ssl(network_client_ssl_config c) {
#ifndef NETWORK_DISABLE_SSL
    ssl_config_ = std::move(c);
#endif
}
void frp_tcp_channel::handle_transfer_ready() {
    if (is_ssl()) ssl_handshake();
    else protocol_ready();
}
void frp_tcp_channel::ssl_handshake() {
#ifndef NETWORK_DISABLE_SSL
    asio::ssl::context ctx(asio::ssl::context::tlsv13);
    auto* a = &ctx;
    try {
        if (ssl_config_.load_exception) std::rethrow_exception(ssl_config_.load_exception);
        if (!ssl_config_.ssl_context) {
            if (!ssl_config_.ca_certificate_path.empty()) ctx.load_verify_file(ssl_config_.ca_certificate_path);
            else ctx.set_default_verify_paths();
            if (!ssl_config_.private_key_path.empty()) ctx.use_private_key_file(ssl_config_.private_key_path, asio::ssl::context::pem);
            if (!ssl_config_.certificate_path.empty()) ctx.use_certificate_chain_file(ssl_config_.certificate_path);
        } else { a = ssl_config_.ssl_context.get(); }
    } catch (const std::exception& e) {
        notify_connect_result(Fundamental::error_code::make_basic_error(1, e.what()), shared_from_this());
        return;
    }
    ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket_, *a);
    ssl_stream_->set_verify_mode(asio::ssl::verify_peer);
    SSL_set_tlsext_host_name(ssl_stream_->native_handle(), host_.c_str());
    ssl_stream_->async_handshake(asio::ssl::stream_base::client,
        [this, ptr = shared_from_this()](const asio::error_code& ec) {
            if (!reference_.is_valid()) return;
            if (!ec) protocol_ready();
            else notify_connect_result(Fundamental::error_code(ec, "ssl handshake failed"), shared_from_this());
        });
#endif
}
void frp_tcp_channel::protocol_ready() {
    socket_.set_option(asio::ip::tcp::no_delay(true));
    notify_connect_result(Fundamental::error_code::make_basic_error(0, "ok"), shared_from_this());
}
bool frp_tcp_channel::is_ssl() const {
#ifndef NETWORK_DISABLE_SSL
    return !ssl_config_.disable_ssl;
#else
    return false;
#endif
}

// --- framed I/O ---

void frp_tcp_channel::async_write_framed(const std::shared_ptr<std::string>& packet) {
    if (!packet || packet->empty()) return;
    asio::post(executor_, [this, self = shared_from_this(), packet]() mutable {
        if (!reference_.is_valid()) return;
        write_queue_.push_back(std::move(packet));
        if (write_queue_.size() == 1) do_write();
    });
}

void frp_tcp_channel::do_write() {
    if (write_queue_.empty()) return;
    auto& cur = write_queue_.front();
    if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
        asio::async_write(*ssl_stream_, asio::buffer(cur->data(), cur->size()),
            [this, self = shared_from_this()](std::error_code ec, std::size_t) {
                if (!reference_.is_valid()) return;
                if (ec) { release_obj(); return; }
                write_queue_.pop_front();
                if (!write_queue_.empty()) do_write();
            });
#endif
    } else {
        asio::async_write(socket_, asio::buffer(cur->data(), cur->size()),
            [this, self = shared_from_this()](std::error_code ec, std::size_t) {
                if (!reference_.is_valid()) return;
                if (ec) { release_obj(); return; }
                write_queue_.pop_front();
                if (!write_queue_.empty()) do_write();
            });
    }
}

void frp_tcp_channel::async_read_framed(std::function<void(std::string)> on_frame) {
    frame_callback_ = std::move(on_frame);
    auto read_payload = [this, self = shared_from_this()](std::error_code ec, std::size_t) {
        if (!reference_.is_valid()) return;
        if (ec) { release_obj(); return; }
        if (frame_callback_) frame_callback_(std::move(frame_payload_));
    };
    auto read_header = [this, self = shared_from_this(), read_payload](std::error_code ec, std::size_t) {
        if (!reference_.is_valid()) return;
        if (ec) { release_obj(); return; }
        std::uint32_t pl = 0;
        Fundamental::net_buffer_copy(frame_header_.data(), &pl, 4);
        if (pl == 0 || pl > frp_command_base::kMaxCommandPayloadLen) { release_obj(); return; }
        frame_payload_.resize(pl);
        if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            asio::async_read(*ssl_stream_, asio::buffer(frame_payload_.data(), frame_payload_.size()), read_payload);
#endif
        } else {
            asio::async_read(socket_, asio::buffer(frame_payload_.data(), frame_payload_.size()), read_payload);
        }
    };
    if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
        asio::async_read(*ssl_stream_, asio::buffer(frame_header_.data(), frame_header_.size()), read_header);
#endif
    } else {
        asio::async_read(socket_, asio::buffer(frame_header_.data(), frame_header_.size()), read_header);
    }
}

// --- raw I/O (for data channel forwarding) ---

void frp_tcp_channel::async_write_raw(const void* data, std::size_t len) {
    if (!data || len == 0) return;
    // 与 async_write_framed 共用写队列：同一 socket 至多一个在途写（规范 §3.4）。
    // 同一通道不会混用 framed/raw（signal 通道 framed、data 通道 raw），共用队列安全。
    async_write_framed(std::make_shared<std::string>(static_cast<const char*>(data), len));
}

void frp_tcp_channel::async_read_raw(std::function<void(const char*, std::size_t)> on_data) {
    raw_callback_ = std::move(on_data);
    if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
        ssl_stream_->async_read_some(asio::buffer(raw_buf_.data(), raw_buf_.size()),
            [this, self = shared_from_this()](const std::error_code& ec, std::size_t n) {
                if (!reference_.is_valid()) return;
                if (ec) { release_obj(); return; }
                if (n > 0 && raw_callback_) raw_callback_(raw_buf_.data(), n);
            });
#endif
    } else {
        socket_.async_read_some(asio::buffer(raw_buf_.data(), raw_buf_.size()),
            [this, self = shared_from_this()](const std::error_code& ec, std::size_t n) {
                if (!reference_.is_valid()) return;
                if (ec) { release_obj(); return; }
                if (n > 0 && raw_callback_) raw_callback_(raw_buf_.data(), n);
            });
    }
}

// proxy_upstream_interface
void frp_tcp_channel::async_buffers_read(network_read_buffers_t buffers, network_io_handler_t h) {
    if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
        asio::async_read(*ssl_stream_, std::move(buffers), std::move(h));
#endif
    } else { asio::async_read(socket_, std::move(buffers), std::move(h)); }
}
void frp_tcp_channel::async_buffers_read_some(network_read_buffers_t buffers, network_io_handler_t h) {
    if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
        ssl_stream_->async_read_some(std::move(buffers), std::move(h));
#endif
    } else { socket_.async_read_some(std::move(buffers), std::move(h)); }
}
void frp_tcp_channel::async_buffers_write(network_write_buffers_t buffers, network_io_handler_t h) {
    if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
        asio::async_write(*ssl_stream_, std::move(buffers), std::move(h));
#endif
    } else { asio::async_write(socket_, std::move(buffers), std::move(h)); }
}
void frp_tcp_channel::async_buffers_write_some(network_write_buffers_t buffers, network_io_handler_t h) {
    if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
        ssl_stream_->async_write_some(std::move(buffers), std::move(h));
#endif
    } else { socket_.async_write_some(std::move(buffers), std::move(h)); }
}
const asio::any_io_executor& frp_tcp_channel::get_current_executor() { return executor_; }

void frp_tcp_channel::close() {
    if (!socket_.is_open()) return;
    auto fc = [this, ptr = shared_from_this()]() {
        asio::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    };
#ifndef NETWORK_DISABLE_SSL
    if (ssl_stream_) { asio::dispatch(ssl_stream_->get_executor(), std::move(fc)); return; }
#endif
    fc();
}

// =============================================================================
// frp_signal_channel
// =============================================================================

frp_signal_channel::frp_signal_channel(const asio::any_io_executor& executor,
                                       std::string host, std::string service) :
executor_(executor), host_(std::move(host)), service_(std::move(service)) {
#ifndef NETWORK_DISABLE_SSL
    ssl_config_.disable_ssl = true;
#endif
}
void frp_signal_channel::enable_ssl(network_client_ssl_config c) { ssl_config_ = std::move(c); }
void frp_signal_channel::set_on_connected(connect_callback_t cb) { on_connected_ = std::move(cb); }
void frp_signal_channel::set_on_disconnected(disconnect_callback_t cb) { on_disconnected_ = std::move(cb); }
void frp_signal_channel::set_on_command(command_callback_t cb) { on_command_ = std::move(cb); }

void frp_signal_channel::start() {
    if (!reference_.is_valid()) return;
    tcp_ = frp_tcp_channel::make_shared(executor_, host_, service_);
    tcp_->enable_ssl(ssl_config_);
    tcp_->notify_connect_result.Connect(shared_from_this(),
        [this](Fundamental::error_code ec, std::shared_ptr<frp_tcp_channel> tcp) {
            if (!reference_.is_valid()) return;
            if (ec || !tcp) { notify_disconnect_once(); return; }
            tcp_ = std::move(tcp);
            if (on_connected_) on_connected_();
            frp_signal_open_data open;
            open.command = frp_signal_open_command;
            send_command(open);
            read_next_command();
        });
    tcp_->start_async_connect();
}

void frp_signal_channel::release_obj() {
    if (!reference_.release()) return;
    asio::post(executor_, [this, self = shared_from_this()] {
        if (tcp_) { tcp_->release_obj(); tcp_ = nullptr; }
        notify_disconnect_once();
    });
}

void frp_signal_channel::read_next_command() {
    if (!tcp_) return;
    tcp_->async_read_framed([this, self = shared_from_this()](std::string payload) {
        on_frame_received(std::move(payload));
    });
}

void frp_signal_channel::on_frame_received(std::string payload) {
    frp_command_base base;
    if (!Fundamental::io::from_json(payload, base)) { release_obj(); return; }
    if (on_command_) on_command_(base, std::move(payload));
}

void frp_signal_channel::notify_disconnect_once() {
    if (disconnect_notified_) return;
    disconnect_notified_ = true;
    if (on_disconnected_) on_disconnected_();
}

// =============================================================================
// relay_data_channel
// =============================================================================

relay_data_channel::relay_data_channel(const asio::any_io_executor& ex,
                                       std::string conn_id, std::string peer) :
connection_uuid_(std::move(conn_id)), peer_uuid_(std::move(peer)), executor_(ex),
backend_socket_(ex), local_socket_(ex), idle_timer_(ex), handshake_timer_(ex) {
    io_context_pool::Instance().reg_timer(idle_timer_);
    io_context_pool::Instance().reg_timer(handshake_timer_);
}

std::shared_ptr<relay_data_channel> relay_data_channel::create(const asio::any_io_executor& ex,
                                                               std::string conn_id, std::string peer_uuid) {
    auto ch = std::make_shared<relay_data_channel>(ex, std::move(conn_id), std::move(peer_uuid));
    ch->reg_pool_object();
    return ch;
}

void relay_data_channel::reg_pool_object() {
    io_context_pool::Instance().reg_object(this,
        [self = shared_from_this()]() { self->release_obj(); });
}

relay_data_channel::~relay_data_channel() {
    io_context_pool::Instance().unreg_timer(idle_timer_);
    io_context_pool::Instance().unreg_timer(handshake_timer_);
    io_context_pool::Instance().unreg_object(this);
    if (kcp_ch_) kcp_ch_->close();
    if (p2p_socket_) { std::error_code ec; p2p_socket_->close(ec); }
}

void relay_data_channel::set_transport(std::shared_ptr<frp_tcp_channel> tc) { attach_tcp(std::move(tc)); }

void relay_data_channel::attach_tcp(std::shared_ptr<frp_tcp_channel> tc) {
    tcp_ = std::move(tc);
    tcp_->set_on_release([self = shared_from_this()]() {
        // p2p 升级期间/之后的中继传输释放是预期行为（accessor 升级成功后主动释放传输，
        // 服务端随之传播对端会话释放），不视为通道关闭——否则 kcp/p2p socket 被销毁，
        // 升级瞬间的在途数据丢失（ASAN 慢速下必现）。
        // 真实断链（无 punch 进行、未升级）仍走统一关闭序列。
        if (!self->is_p2p_active() && !self->punch_engine()) self->close();
    });
}

void relay_data_channel::set_on_release(std::function<void()> cb) { on_release_ = std::move(cb); }

void relay_data_channel::release_obj() {
    if (closed_.load()) return;
    if (io_context_pool::Instance().running_in_io_thread()) { close(); return; }
    // 入口只投递（规范 §4.2）：关闭序列在绑定 io 线程执行
    asio::post(executor_, [self = shared_from_this()]() { self->close(); });
}

void relay_data_channel::close() {
    if (closed_.exchange(true)) return; // CAS 幂等
    FASSERT(io_context_pool::Instance().running_in_io_thread(), "relay_data_channel::close must run on io thread");
    // 关闭路径可观测：中继断开（对端 FIN/错误/超时/主动）在此留痕，否则无法从日志判断释放
    FINFO("relay_data_channel close conn={} peer={} transport={} reason=closed", connection_uuid_, peer_uuid_,
          static_cast<int>(transport_));
    io_context_pool::Instance().unreg_object(this);
    io_context_pool::Instance().unreg_timer(idle_timer_);
    io_context_pool::Instance().unreg_timer(handshake_timer_);
    if (punch_engine_) { punch_engine_->release(); punch_engine_.reset(); }
    if (kcp_ch_) { kcp_ch_->close(); kcp_ch_.reset(); }
    if (p2p_socket_) { std::error_code ec; p2p_socket_->close(ec); p2p_socket_.reset(); }
    if (tcp_) { tcp_->release_obj(); tcp_.reset(); }
    std::error_code ec;
    idle_timer_.cancel();
    handshake_timer_.cancel();
    // 只 clear 不 reset：在途写的完成回调捕获 relay 强引用，writer 必须活到回调执行完
    if (data_writer_) data_writer_->clear();
    if (backend_socket_.is_open()) backend_socket_.close(ec);
    if (backend_udp_socket_) { backend_udp_socket_->close(ec); backend_udp_socket_.reset(); }
    if (local_socket_.is_open()) local_socket_.close(ec);
    if (local_udp_socket_) { local_udp_socket_->close(ec); local_udp_socket_.reset(); }
    if (on_release_) on_release_();
}

void relay_data_channel::init_kcp() {
    if (kcp_ch_ || traffic_secret_.empty()) return;
    kcp_ch_ = std::make_shared<kcp_channel>(executor_, traffic_secret_, connection_uuid_);
    kcp_ch_->set_on_output([this](const std::uint8_t* data, std::size_t len) {
        if (p2p_socket_) {
            auto d = std::make_shared<std::string>(reinterpret_cast<const char*>(data), len);
            p2p_socket_->async_send_to(asio::buffer(*d), p2p_peer_endpoint_,
                [d](const std::error_code&, std::size_t) {});
        } else if (tcp_) {
            tcp_->async_write_raw(data, len);
        }
    });
    // backend/local 侧写串行化（规范 §3.4）：launcher 按当前活跃目的地发起写。
    // 完成回调捕获 keepalive（relay 强引用）→ relay 存活 → writer（成员）存活，裸 this 安全。
    data_writer_ = std::make_unique<network::serialized_writer>(
        executor_,
        [this](const std::shared_ptr<std::string>& data, network_io_handler_t completion) {
            if (backend_socket_.is_open()) {
                asio::async_write(backend_socket_, asio::buffer(*data), std::move(completion));
            } else if (backend_udp_socket_) {
                backend_udp_socket_->async_send_to(asio::buffer(*data), backend_udp_target_,
                                                   std::move(completion));
            } else if (local_socket_.is_open()) {
                asio::async_write(local_socket_, asio::buffer(*data), std::move(completion));
            } else if (local_udp_socket_) {
                local_udp_socket_->async_send_to(asio::buffer(*data), local_udp_endpoint_,
                                                 std::move(completion));
            } else {
                completion(asio::error::not_connected, 0);
            }
        },
        [self = shared_from_this()]() { (void)self; });
    data_writer_->set_error_handler([self = shared_from_this()](std::error_code) {
        self->close();
    });
    kcp_ch_->set_on_frame([this](std::vector<std::uint8_t> plaintext) {
        reset_idle_timer(); // 空闲检测针对业务数据：探测帧（无 payload）不触发 on_frame
        if (data_writer_) {
            data_writer_->push(std::make_shared<std::string>(plaintext.begin(), plaintext.end()));
            return;
        }
        FERR("kcp on_frame without data_writer conn={}", connection_uuid_);
    });
    // 链路检测：keepalive 判定死链 -> 关闭通道（失联传播到对端与 server）
    kcp_ch_->set_on_dead([self = shared_from_this()]() { self->close(); });
    kcp_ch_->init();
    FINFO("kcp init conn={}", connection_uuid_);
}

void relay_data_channel::accept_p2p(std::shared_ptr<asio::ip::udp::socket> socket,
                                     const asio::ip::udp::endpoint& peer_endpoint) {
    if (p2p_success_ || !socket) return;
    p2p_socket_ = std::move(socket);
    p2p_peer_endpoint_ = peer_endpoint;
    FINFO("p2p data path active conn={} peer={}:{}",
          connection_uuid_, peer_endpoint.address().to_string(), peer_endpoint.port());
    if (tcp_) {
        // 主动升级路径：传输释放是预期行为，不触发"传输死亡 → 通道关闭"
        tcp_->set_on_release({});
        tcp_->release_obj();
        tcp_ = nullptr;
    }
    start_p2p_read_loop();
}

void relay_data_channel::send_bytes(const char* data, std::size_t size) {
    if (!kcp_ch_ || closed_) return;
    kcp_ch_->send_plaintext(data, size);
}

void relay_data_channel::feed_kcp(const char* data, std::size_t len) {
    if (!kcp_ch_ || closed_) return;
    kcp_ch_->feed_encrypted(data, len);
}

void relay_data_channel::reset_idle_timer() {
    if (idle_timeout_sec_ == 0) return;
    idle_timer_.cancel();
    idle_timer_.expires_after(std::chrono::seconds(idle_timeout_sec_));
    idle_timer_.async_wait([self = shared_from_this()](const std::error_code& ec) {
        if (ec || self->closed_.load()) return;
        FWARN("relay data channel idle timeout conn={} peer={}", self->connection_uuid_, self->peer_uuid_);
        self->close();
    });
}

void relay_data_channel::start_p2p_read_loop() {
    if (!p2p_socket_ || closed_) return;
    auto self = shared_from_this();
    p2p_socket_->async_receive_from(
        asio::buffer(p2p_read_buf_.data(), p2p_read_buf_.size()), p2p_peer_endpoint_,
        [this, self](const std::error_code& ec, std::size_t n) {
            if (ec || closed_ || !kcp_ch_) return;
            kcp_ch_->feed_encrypted(p2p_read_buf_.data(), n);
            start_p2p_read_loop();
        });
}

// =============================================================================
// frp_unified_client

frp_unified_client::frp_unified_client(frp_proxy_client_config config) :
config_(std::move(config)) {
    signal_ = std::make_shared<frp_signal_client>(config_);
    accessor_ = std::make_shared<frp_accessor>(signal_);
    provider_ = std::make_shared<frp_provider>(signal_);
    provider_->set_service_map({});
}


void frp_unified_client::start() {
    if (!signal_->is_reference_valid()) return;
    FINFO("frp_unified_client start uuid={}", signal_->uuid());
    for (const auto& group : signal_->config().groups) {
        for (const auto& svc : group.services)
            provider_->set_service_map({{group.register_key, {{svc.service_name, svc}}}});
        for (const auto& svc : group.services)
            ; // service map set above
    }
    // Populate provider service map properly
    std::unordered_map<std::string, std::unordered_map<std::string, frp_provider_service_config>> smap;
    for (const auto& group : signal_->config().groups)
        for (const auto& svc : group.services)
            smap[group.register_key][svc.service_name] = svc;
    provider_->set_service_map(std::move(smap));

    signal_->set_on_server_command([this](const frp_command_base& cmd, std::string payload) {
        on_server_command(cmd, std::move(payload));
    });
    signal_->set_on_client_command([this](std::uint8_t cmd, std::string payload) {
        on_client_command(cmd, std::move(payload));
    });
    signal_->set_on_subscribe([this](const std::vector<frp_visible_service_data>& services) {
        on_subscribe(services);
    });
    signal_->start();
}


void frp_unified_client::release_obj() {
    // unified 用自己的 CAS：signal 的 reference 留给 signal 自己的关闭链
    // （否则抢先 release 会让 signal_->release_obj() 短路，通道永不关闭）
    if (released_.exchange(true)) return;
    FINFO("frp_unified_client release_obj uuid={}", signal_->uuid());
    auto executor = signal_->get_executor();
    asio::post(executor, [this, self = shared_from_this()] {
        signal_->release_obj();
        if (accessor_) accessor_->close();
        if (provider_) provider_->close();
    });
}


void frp_unified_client::on_server_command(const frp_command_base& cmd, std::string payload) {
    switch (cmd.command) {
    case frp_auth_response_command:
        provider_->register_all_services();
        accessor_->subscribe_all_keys();
        break;
    case frp_register_services_resp_command: {
        frp_register_services_resp_data resp;
        if (Fundamental::io::from_json(payload, resp) && resp.ok)
            FINFO("register ok uuid={}", signal_->uuid());
        break;
    }
    default: break;
    }
}


void frp_unified_client::on_client_command(std::uint8_t cmd, std::string payload) {
    frp_client_command_base ccmd;
    if (!Fundamental::io::from_json(payload, ccmd)) return;
    // Route to appropriate role module
    switch (ccmd.command) {
    case frp_client_open:
        provider_->on_client_command(cmd, std::move(payload));
        break;
    case frp_client_accept:
    case frp_client_reject:
        accessor_->on_client_command(cmd, std::move(payload));
        break;
    default:
        // P2P commands — try both modules
        accessor_->on_client_command(cmd, payload);
        provider_->on_client_command(cmd, std::move(payload));
        break;
    }
}


void frp_unified_client::on_subscribe(const std::vector<frp_visible_service_data>& services) {
    accessor_->on_subscribe(services);
}


} // namespace network::proxy
