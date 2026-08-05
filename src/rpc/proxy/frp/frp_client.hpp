#pragma once

#include "frp_accessor.hpp"
#include "frp_client_command.hpp"
#include "frp_command.hpp"
#include "frp_config_types.hpp"
#include "frp_provider.hpp"
#include "frp_punch_engine.hpp"
#include "frp_signal_client.hpp"
#include "kcp_channel.hpp"
#include "rpc/proxy/rpc_forward_connection.hpp"

#include "network/async_utils.hpp"
#include "network/network.hpp"

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace network::proxy
{

// =============================================================================
// frp_tcp_channel — base: TCP connection helper with frame read/write
// =============================================================================

class frp_tcp_channel : public std::enable_shared_from_this<frp_tcp_channel>,
                        private asio::noncopyable,
                        public proxy::proxy_upstream_interface {
public:
    Fundamental::Signal<void(Fundamental::error_code, std::shared_ptr<frp_tcp_channel>)> notify_connect_result;

    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_tcp_channel>(std::forward<Args>(args)...);
    }

    frp_tcp_channel(const asio::any_io_executor& executor, const std::string& host, const std::string& service);
    ~frp_tcp_channel();

    void start_async_connect();
    void enable_ssl(network_client_ssl_config config);
    void release_obj() override;
    /// 传输死亡时触发（读写失败路径收敛到归属通道的关闭序列，规范 §7.2）。
    void set_on_release(std::function<void()> cb);

    void async_write_framed(const std::shared_ptr<std::string>& packet);
    void async_write_raw(const void* data, std::size_t len);
    void async_read_framed(std::function<void(std::string)> on_frame);
    void async_read_raw(std::function<void(const char*, std::size_t)> on_data);

    std::string local_endpoint_string() const;
    std::string remote_endpoint_string() const;

    // proxy_upstream_interface — used by raw read/write
    void async_buffers_read(network_read_buffers_t buffers, network_io_handler_t handler) override;
    void async_buffers_read_some(network_read_buffers_t buffers, network_io_handler_t handler) override;
    void async_buffers_write(network_write_buffers_t buffers, network_io_handler_t handler) override;
    void async_buffers_write_some(network_write_buffers_t buffers, network_io_handler_t handler) override;
    const asio::any_io_executor& get_current_executor() override;

private:
    void close();
    void handle_transfer_ready();
    void ssl_handshake();
    void protocol_ready();
    void do_write();
    bool is_ssl() const;

    network_data_reference reference_;
    const std::string host_;
    const std::string service_;
    const asio::any_io_executor executor_;
    ::asio::ip::tcp::socket socket_;
#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_ = nullptr;
    network_client_ssl_config ssl_config_;
#endif
    asio::ip::tcp::resolver resolver_;
    std::deque<std::shared_ptr<std::string>> write_queue_;

    // frame read state
    std::array<std::uint8_t, 4> frame_header_ {};
    std::string frame_payload_;
    std::function<void(std::string)> frame_callback_;

    // raw read state
    std::array<char, 16 * 1024> raw_buf_ {};
    std::function<void(const char*, std::size_t)> raw_callback_;
    std::function<void()> on_release_;
};

// =============================================================================
// frp_signal_channel — signal channel: framed commands to/from server
// =============================================================================

class frp_signal_channel : public std::enable_shared_from_this<frp_signal_channel>,
                           private asio::noncopyable {
public:
    using connect_callback_t    = std::function<void()>;
    using disconnect_callback_t = std::function<void()>;
    using command_callback_t    = std::function<void(const frp_command_base&, std::string)>;

    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_signal_channel>(std::forward<Args>(args)...);
    }

    frp_signal_channel(const asio::any_io_executor& executor, std::string host, std::string service);
    ~frp_signal_channel() = default;

    void enable_ssl(network_client_ssl_config config);
    void set_on_connected(connect_callback_t cb);
    void set_on_disconnected(disconnect_callback_t cb);
    void set_on_command(command_callback_t cb);
    void start();
    void release_obj();

    template <typename CommandData>
    void send_command(const CommandData& data) {
        if (!reference_.is_valid()) return;
        auto packet = packet_frp_command_data(data);
        if (!packet) return;
        if (tcp_) tcp_->async_write_framed(packet);
    }

    void read_next_command();

private:
    void on_frame_received(std::string payload);
    void on_raw_received(const char* data, std::size_t len);
    void notify_disconnect_once();

    network_data_reference reference_;
    const asio::any_io_executor executor_;
    std::string host_;
    std::string service_;
    network_client_ssl_config ssl_config_;
    std::shared_ptr<frp_tcp_channel> tcp_;
    connect_callback_t on_connected_;
    disconnect_callback_t on_disconnected_;
    command_callback_t on_command_;
    command_callback_t on_raw_command_; // for raw forwarded payloads
    bool disconnect_notified_ = false;
};

// =============================================================================
// relay_data_channel — data channel: raw TCP relay to server
// =============================================================================

class relay_data_channel : public std::enable_shared_from_this<relay_data_channel> {
public:
    relay_data_channel(const asio::any_io_executor& ex, std::string conn_id, std::string peer_uuid);
    ~relay_data_channel();

    /// 必须经 create() 创建：构造完成后注册池对象（构造函数内 shared_from_this() 不可用）。
    static std::shared_ptr<relay_data_channel> create(const asio::any_io_executor& ex,
                                                      std::string conn_id, std::string peer_uuid);

    // --- transport injection (async TCP connect completes before init_kcp) ---
    void set_transport(std::shared_ptr<frp_tcp_channel> tcp_channel);
    /// 注入传输通道并绑定"传输死亡 → 本通道关闭"（规范 §7.2 错误收敛）。
    void attach_tcp(std::shared_ptr<frp_tcp_channel> tcp_channel);
    void init_kcp();

    // --- data path ---
    void send_bytes(const char* data, std::size_t size);
    void feed_kcp(const char* data, std::size_t len);

    // --- P2P upgrade ---
    void accept_p2p(std::shared_ptr<asio::ip::udp::socket> socket,
                    const asio::ip::udp::endpoint& peer_endpoint);

    // --- lifecycle ---
    void set_on_release(std::function<void()> cb);
    /// 三层模型入口（规范 §4.1）：任意线程可调用，只投递；io 线程内直接执行关闭序列。
    void release_obj();
    void close();

    // --- identity (read-only) ---
    const std::string& connection_uuid() const { return connection_uuid_; }
    const std::string& peer_uuid() const { return peer_uuid_; }
    const std::string& register_key() const { return register_key_; }
    const std::string& service_name() const { return service_name_; }
    std::uint8_t transport() const { return transport_; }
    bool is_closed() const { return closed_.load(); }
    bool is_p2p_active() const { return p2p_success_; }
    void set_p2p_active(bool v) { p2p_success_ = v; }
    const asio::any_io_executor& get_executor() const { return executor_; }

    // --- identity setters (called once during handshake) ---
    void set_register_key(std::string key) { register_key_ = std::move(key); }
    void set_service_name(std::string name) { service_name_ = std::move(name); }
    void set_transport(std::uint8_t t) { transport_ = t; }

    // --- KCP setup (set traffic_secret before init_kcp) ---
    void set_traffic_secret(std::string secret) { traffic_secret_ = std::move(secret); }

    // --- pending writes (buffered before KCP is ready) ---
    std::deque<std::shared_ptr<std::string>>& pending_writes() { return pending_writes_; }

    // --- I/O resource access (non-const, for frp_unified_client read/write loops) ---
    std::shared_ptr<frp_tcp_channel>& tcp() { return tcp_; }
    asio::ip::tcp::socket& backend_socket() { return backend_socket_; }
    std::unique_ptr<asio::ip::udp::socket>& backend_udp_socket() { return backend_udp_socket_; }
    asio::ip::udp::endpoint& backend_udp_target() { return backend_udp_target_; }
    bool& backend_connected() { return backend_connected_; }
    std::array<char, 16 * 1024>& backend_read_buf() { return backend_read_buf_; }

    asio::ip::tcp::socket& local_socket() { return local_socket_; }
    /// 与 listener 共享所有权的 UDP socket（3.3：listener 关闭后通道仍可安全持有）
    std::shared_ptr<asio::ip::udp::socket>& local_udp_socket() { return local_udp_socket_; }
    asio::ip::udp::endpoint& local_udp_endpoint() { return local_udp_endpoint_; }
    std::array<char, 16 * 1024>& read_buf() { return read_buf_; }

    // --- KCP / P2P internal accessors ---
    bool has_kcp() const { return kcp_ch_ && kcp_ch_->is_active(); }
    std::shared_ptr<asio::ip::udp::socket>& p2p_socket() { return p2p_socket_; }

    // --- P2P helpers ---
    std::shared_ptr<frp_punch_engine>& punch_engine() { return punch_engine_; }
    std::uint16_t& my_external_port() { return my_external_port_; }
    std::string& my_external_ip() { return my_external_ip_; }
    bool has_on_release() const { return static_cast<bool>(on_release_); }

    // --- timers ---
    asio::steady_timer& idle_timer() { return idle_timer_; }
    asio::steady_timer& handshake_timer() { return handshake_timer_; }

    // --- idle timeout ---
    /// 设置数据面空闲超时（连接建立时由 accessor/provider 从配置注入）
    void set_idle_timeout_seconds(std::uint32_t sec) { idle_timeout_sec_ = sec; }
    /// 数据面空闲检测（io 线程）：TCP 读 / P2P 读路径统一重置，到期关闭通道。
    /// P2P 模式对端断开无 TCP 通知，此机制是唯一断开检测（不能豁免 is_p2p_active）。
    void reset_idle_timer();

private:
    void start_p2p_read_loop();
    void reg_pool_object();

    std::string connection_uuid_;
    std::string peer_uuid_;
    std::string register_key_;
    std::string service_name_;
    std::uint8_t transport_    = 0;
    std::uint32_t idle_timeout_sec_ = 0;
    std::atomic<bool> closed_  = false;
    bool p2p_success_          = false;
    std::uint16_t my_external_port_ = 0;
    std::string my_external_ip_;
    std::shared_ptr<frp_tcp_channel> tcp_;
    const asio::any_io_executor executor_;
    asio::steady_timer idle_timer_;
    asio::steady_timer handshake_timer_;

    // punch engine (created on maybe_start_p2p)
    std::shared_ptr<frp_punch_engine> punch_engine_;

    // KCP channel (wraps IKCPCB lifecycle + encryption)
    std::shared_ptr<kcp_channel> kcp_ch_;
    std::string traffic_secret_;

    // backend/local 侧写串行化（规范 §3.4）：同一 socket 至多一个在途写。
    // 生命周期与 relay 相同（close 只 clear 不 reset——在途写完成回调持有 keepalive）。
    std::unique_ptr<network::serialized_writer> data_writer_;

    // P2P state (set by accept_p2p, KCP output switches here)
    std::shared_ptr<asio::ip::udp::socket> p2p_socket_;
    asio::ip::udp::endpoint p2p_peer_endpoint_;
    std::array<char, 16 * 1024> p2p_read_buf_{};

    // provider: backend state
    asio::ip::tcp::socket backend_socket_;
    std::unique_ptr<asio::ip::udp::socket> backend_udp_socket_;
    asio::ip::udp::endpoint backend_udp_target_;
    bool backend_connected_ = false;
    std::array<char, 16 * 1024> backend_read_buf_{};

    // accessor: local socket
    asio::ip::tcp::socket local_socket_;
    std::shared_ptr<asio::ip::udp::socket> local_udp_socket_;
    asio::ip::udp::endpoint local_udp_endpoint_;
    std::array<char, 16 * 1024> read_buf_{};
    std::deque<std::shared_ptr<std::string>> pending_writes_;

    std::function<void()> on_release_;
};

// =============================================================================
// frp_unified_client
// =============================================================================

class frp_unified_client : public std::enable_shared_from_this<frp_unified_client>,
                           private asio::noncopyable {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_unified_client>(std::forward<Args>(args)...);
    }

    explicit frp_unified_client(frp_proxy_client_config config);
    ~frp_unified_client() = default;

    void start();
    void release_obj();

private:
    struct listener_runtime {
        explicit listener_runtime(const asio::any_io_executor& ex) : acceptor(ex) {}
        std::string service_name;
        std::string listen_host;
        std::uint16_t listen_port          = 0;
        std::uint8_t service_type          = frp_service_tcp;
        bool enable_p2p                    = true;
        bool provider_enable_p2p           = true;
        std::uint8_t provider_nat_type     = frp_nat_type_disabled;
        std::uint32_t provider_startup_rtt_ms = 100;
        std::string register_key;
        std::string provider_uuid;
        asio::ip::tcp::acceptor acceptor;
        std::unique_ptr<asio::ip::udp::socket> udp_socket;
        std::array<char, 16 * 1024> udp_recv_buf {};
        asio::ip::udp::endpoint udp_recv_endpoint_;
        std::unordered_map<asio::ip::udp::endpoint, std::weak_ptr<relay_data_channel>> udp_sessions;
    };

    // Glue: routes signal callbacks to role modules
    void on_server_command(const frp_command_base& cmd, std::string payload);
    void on_client_command(std::uint8_t cmd, std::string payload);
    void on_subscribe(const std::vector<frp_visible_service_data>& services);

    frp_proxy_client_config config_;
    std::shared_ptr<frp_signal_client> signal_;
    std::shared_ptr<frp_accessor> accessor_;
    std::shared_ptr<frp_provider> provider_;
    std::atomic<bool> released_ = false;
};

} // namespace network::proxy
