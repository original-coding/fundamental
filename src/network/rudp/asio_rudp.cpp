//
// @author lightning1993 <469953258@qq.com> 2025/08
//
#include "asio_rudp.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "fundamental/basic/endian_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
#include "kcp_imp/ikcp.h"
#include "network/network.hpp"

namespace network::rudp
{
namespace
{
static inline std::int64_t get_current_time() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
static inline std::uint32_t get_clock_t() {
    return static_cast<std::uint32_t>(get_current_time());
}
} // namespace

using rudp_id_t = std::uint32_t;
enum rudp_command_t : std::uint8_t
{
    RUDP_UNKNOWN_COMMAND  = 0,
    RUDP_SYN_COMMAND      = 1,
    RUDP_SYN_ACK_COMMAND  = 2,
    RUDP_SYN_ACK2_COMMAND = 4,
    RUDP_PING_COMMAND     = 8,
    RUDP_PONG_COMMAND     = 16,
    RUDP_RST_COMMAND      = 32,
    RUDP_SERVER_INIT_MASK = RUDP_SYN_COMMAND | RUDP_SYN_ACK2_COMMAND | RUDP_RST_COMMAND,
    RUDP_CLIENT_INIT_MASK = RUDP_SYN_ACK_COMMAND | RUDP_RST_COMMAND,
    RUDP_CONNECTED_MASK   = RUDP_SYN_ACK_COMMAND | RUDP_PING_COMMAND | RUDP_PONG_COMMAND | RUDP_RST_COMMAND,
    RUDP_VERIFY_SRC_MASK  = RUDP_SYN_ACK2_COMMAND | RUDP_PING_COMMAND | RUDP_PONG_COMMAND,
};

enum rudp_connection_status : std::uint32_t
{
    RUDP_CLOSED_STATUS    = 0,
    RUDP_INIT_STATUS      = 1,
    RUDP_SYN_SENT_STATUS  = 2,
    RUDP_SYN_RECV_STATUS  = 4,
    RUDP_CONNECTED_STATUS = 8,
};

enum rudp_force_update_status : std::size_t
{
    RUDP_FORCE_UPDATE_NONE,
    RUDP_FORCE_UPDATE_REQUESTING,
    RUDP_FORCE_UPDATE_PROCESSING
};

struct control_frame_data {
    static constexpr std::size_t kRudpControlFrameSize = 21;
    static constexpr rudp_id_t kControlMagicNum        = 0xffffffff;
    static constexpr std::size_t kFieldSize            = sizeof(std::uint32_t);
    rudp_command_t command                             = rudp_command_t::RUDP_UNKNOWN_COMMAND;
    std::uint32_t command_ts                           = 0;
    rudp_id_t src_id                                   = 0;
    rudp_id_t dst_id                                   = kControlMagicNum;
    std::uint32_t payload                              = kControlMagicNum;

    std::vector<std::uint8_t> encode() const {
        std::vector<std::uint8_t> ret;
        ret.resize(kRudpControlFrameSize);
        auto ptr = ret.data();
        Fundamental::net_buffer_copy(&kControlMagicNum, ptr, kFieldSize);
        Fundamental::net_buffer_copy(&command, ptr + 4, 1);
        Fundamental::net_buffer_copy(&command_ts, ptr + 5, kFieldSize);
        Fundamental::net_buffer_copy(&src_id, ptr + 9, kFieldSize);
        Fundamental::net_buffer_copy(&dst_id, ptr + 13, kFieldSize);
        Fundamental::net_buffer_copy(&payload, ptr + 17, kFieldSize);
        return ret;
    }

    inline bool decode(const void* data, std::size_t len) {
        do {
            if (kRudpControlFrameSize != len) break;

            auto ptr            = static_cast<const std::uint8_t*>(data);
            rudp_id_t magic_num = 0;
            Fundamental::net_buffer_copy(ptr, &magic_num, kFieldSize);
            if (magic_num != kControlMagicNum) break;
            Fundamental::net_buffer_copy(ptr + 4, &command, 1);
            Fundamental::net_buffer_copy(ptr + 5, &command_ts, kFieldSize);
            Fundamental::net_buffer_copy(ptr + 9, &src_id, kFieldSize);
            Fundamental::net_buffer_copy(ptr + 13, &dst_id, kFieldSize);
            Fundamental::net_buffer_copy(ptr + 17, &payload, kFieldSize);
            return true;
        } while (0);
        return false;
    }
};

struct release_kcp_wrapper {
    constexpr release_kcp_wrapper() noexcept = default;
    void operator()(ikcpcb* ptr) const {
        if (!ptr) return;
        ikcp_release(ptr);
    }
};

struct rudp_config_item {
    std::size_t current_value = 0;
    std::size_t min_value     = 0;
    std::size_t max_value     = std::numeric_limits<std::size_t>::max();
    std::size_t set_value(std::size_t new_value) {
        if (new_value < min_value) new_value = min_value;
        if (new_value > max_value) new_value = max_value;
        current_value = new_value;
        return new_value;
    }
    operator std::size_t() const {
        return current_value;
    }
};

struct rudp_socket;
struct rudp_kernel {
    static std::shared_ptr<rudp_socket> create_socket(const asio::any_io_executor& ios, Fundamental::error_code& ec) {
        std::scoped_lock<std::mutex> locker(rudp::rudp_kernel::data_mutex);
        if (storage_.size() >= kMaxRudpDescriptorNums) {
            ec = error::make_error_code(error::rudp_errors::rudp_device_or_resource_busy, "too many descriptors");
            return {};
        }
        while (storage_.find(id_) != storage_.end() || id_ == control_frame_data::kControlMagicNum) {
            ++id_;
        }
        auto ret      = std::make_shared<rudp_socket>(id_, ios);
        storage_[id_] = ret;
        return ret;
    }

    static rudp_id_t get_id(Fundamental::error_code& ec) {
        if (storage_.size() >= kMaxRudpDescriptorNums) {
            ec = error::make_error_code(error::rudp_errors::rudp_device_or_resource_busy, "too many descriptors");
            return 0;
        }
        while (storage_.find(id_) != storage_.end() || id_ == control_frame_data::kControlMagicNum) {
            ++id_;
        }
        return id_;
    }

    constexpr static std::size_t kMaxRudpDescriptorNums        = 1024 * 1024;
    constexpr static std::size_t kMaxRudpSocketCacheBufferNums = 1024 * 32;
    constexpr static std::size_t kMaxRudpProtocalCacheNums     = kMaxRudpSocketCacheBufferNums;
    constexpr static std::size_t kMaxMtuSize                   = 32 * 1024;
    // system config
    inline static rudp_config_item kConnectTimeoutMs { 250, 10, 20000 };
    inline static rudp_config_item kCommandMaxTryCnt { 20, 2, 500 };
    inline static rudp_config_item kMaxSendWindowSize { 128, 2, kMaxRudpProtocalCacheNums };
    inline static rudp_config_item kMaxRecvWindowSize { 128, 2, kMaxRudpProtocalCacheNums };
    inline static rudp_config_item kMtuSize { 1200, 64, kMaxMtuSize };
    inline static rudp_config_item kEnableNoDelay { 1, 0, 1 };
    inline static rudp_config_item kUpdateIntervalMs { 10, 1, 5000 };
    inline static rudp_config_item kResendSkipCnt { 0, 0, 10 };
    inline static rudp_config_item kEnbableNoCongestionControl { 1, 0, 1 };
    inline static rudp_config_item kEnbableAutoKeepAlive { 0, 0, 1 };
    inline static rudp_config_item kEnbableStreamMode { 0, 0, 1 };
    inline static rudp_config_item kMaxConnectionIdleTimeMs { 10000, 200, 60000 };

    inline static std::mutex data_mutex;
    inline static rudp_id_t id_ = control_frame_data::kControlMagicNum;
    inline static std::map<rudp_id_t, std::weak_ptr<rudp_socket>> storage_;
};

struct rudp_client_context_imp {
    struct rudp_read_session {
        void* buf               = nullptr;
        std::size_t buf_len     = 0;
        std::size_t read_offset = 0;
        std::function<void(std::size_t, Fundamental::error_code)> complete_func;
    };
    struct rudp_write_session {
        std::size_t data_len     = 0;
        std::size_t write_offset = 0;
        std::function<void(std::size_t, Fundamental::error_code)> complete_func;
    };

    rudp_client_context_imp(rudp_socket* parent);
    // external interface  active client read begin entrypoint 1
    void connect(const std::string& address,
                 std::uint16_t port,
                 const std::function<void(Fundamental::error_code)>& complete_func);
    void rudp_send(const void* buf,
                   std::size_t len,
                   const std::function<void(std::size_t, Fundamental::error_code)>& complete_func);

    void rudp_recv(void* buf,
                   std::size_t len,
                   const std::function<void(std::size_t, Fundamental::error_code)>& complete_func);
    // internal passive client read begin entrypoint 1
    void passive_connect(asio::ip::udp::endpoint sender_endpoint,
                         rudp_id_t src_id,
                         std::uint32_t payload,
                         const std::function<void(Fundamental::error_code, rudp_connection_status)>& complete_func);
    void unique_passive_connect(asio::ip::udp::endpoint sender_endpoint,
                                rudp_id_t src_id,
                                std::uint32_t payload,
                                const std::function<void(Fundamental::error_code)>& complete_func);
    bool is_connected() const;
    bool is_closed() const;
    bool is_active() const;
    void destroy(Fundamental::error_code ec);
    void set_status(rudp_connection_status dst_status, Fundamental::error_code ec = {});

    void send_syn();
    void send_ack();
    void send_ack2();
    void send_ping();
    void send_pong();
    void send_rst();
    //
    void accept_assign_executor(asio::io_context& new_executor);
    void restart_status_timer();
    void init_rudp_output();
    void notify_data_bytes_write(std::size_t len);
    void request_update_rudp_status();
    void active_request_update_rudp_status();
    void comsume_remote_frames();
    void update_active_time();
    //
    void perform_read();
    void process_read_data(const std::uint8_t* data, std::size_t read_size);
    void process_rudp_data_frame(const std::uint8_t* data, std::size_t read_size);
    void process_control_frame(const std::uint8_t* data);
    std::uint8_t supported_commands() const noexcept;
    void handle_syn();
    void handle_syn_ack();
    void handle_syn_ack2();
    void handle_ping();
    void handle_pong();
    void handle_rst();
    //
    void write_data(const void* data, std::size_t len);
    void write_data(std::vector<std::uint8_t>&& buf, bool is_control_frame = false);
    void flush_data();
    void health_check();
    //
    rudp_socket* const socket_ref;
    std::unique_ptr<ikcpcb, release_kcp_wrapper> kcp_object;
    std::uint32_t command_ts      = 0;
    rudp_connection_status status = RUDP_INIT_STATUS;
    rudp_id_t remote_id           = control_frame_data::kControlMagicNum;
    std::function<void(Fundamental::error_code)> closed_cb;
    std::function<void(Fundamental::error_code)> connected_cb;
    std::vector<std::uint8_t> socket_read_cache;
    std::size_t read_token = 0;

    std::list<std::vector<std::uint8_t>> pending_control_frames;
    std::list<std::vector<std::uint8_t>> pending_data_frames;
    std::tuple<std::vector<std::uint8_t>, std::size_t> pending_remote_data_frame;
    std::vector<std::uint8_t> pending_remote_data_frame_swap_cache;

    std::list<rudp_read_session> pending_rudp_read_requests;
    std::list<rudp_write_session> pending_rudp_write_requests;
    std::int64_t last_rudp_update_time_point = std::numeric_limits<std::int64_t>::max();
    control_frame_data recv_control_frame;
    asio::steady_timer update_timer;
    std::int64_t last_active_time_ms                = 0;
    rudp_force_update_status force_rudp_update_flag = rudp_force_update_status::RUDP_FORCE_UPDATE_NONE;
    // health status check
    asio::steady_timer health_check_timer;
    // timer for status check
    asio::steady_timer status_timer;
    std::uint8_t status_check_command        = 0;
    std::size_t status_check_command_try_cnt = 0;
};

struct rudp_server_context_imp {
    struct rudp_accept_session {
        asio::io_context& ios;
        std::function<void(rudp_handle_t, Fundamental::error_code)> complete_func;
    };
    rudp_server_context_imp(rudp_socket* parent, std::size_t max_pending_connections);
    rudp_server_context_imp(rudp_socket* parent, const std::function<void(Fundamental::error_code)>& complete_func);
    ~rudp_server_context_imp() {

    };
    // external interface
    [[nodiscard]] Fundamental::error_code listen();
    void async_rudp_accept(asio::io_context& ios,
                           const std::function<void(rudp_handle_t handle, Fundamental::error_code)>& complete_func);
    void async_rudp_wait_connect(std::size_t max_wait_ms);
    // internal interface
    void process_wait_connect();
    void peform();
    bool is_listening() const;
    bool filter_data(const void* data, std::size_t data_len);
    void handle_request();
    void handle_syn();
    void destroy(Fundamental::error_code ec);
    // internal events
    void on_connection_status_changed(std::shared_ptr<rudp_socket> connection,
                                      rudp_connection_status current_status,
                                      Fundamental::error_code ec);
    void accept_connection();
    //
    rudp_socket* const socket_ref;
    asio::steady_timer timer;
    const std::size_t max_pending_connections;
    std::atomic_bool listen_flag                 = false;
    std::atomic_bool wait_unique_connection_flag = false;
    std::function<void(Fundamental::error_code)> wait_unique_connection_cb;

    std::unordered_map<asio::ip::udp::endpoint /*remote endpoint*/, std::shared_ptr<rudp_socket>> recv_dic;
    std::list<std::shared_ptr<rudp_socket>> connected_list;

    std::list<rudp_accept_session> accept_list;
    std::vector<std::uint8_t> recv_buf;
    asio::ip::udp::endpoint sender_endpoint;
    control_frame_data last_control_frame;
    std::unordered_map<asio::ip::udp::endpoint /*remote endpoint*/, std::weak_ptr<rudp_socket>> filter_table;
};

struct rudp_socket : public std::enable_shared_from_this<rudp_socket> {
    rudp_socket(rudp_id_t id, const asio::any_io_executor& executor) : socket(executor), id_(id) {
        std::error_code ec;
        socket.set_option(asio::ip::udp::socket::receive_buffer_size(rudp_kernel::kMaxMtuSize), ec);
        socket.set_option(asio::ip::udp::socket::send_buffer_size(rudp_kernel::kMaxMtuSize), ec);
    }
    ~rudp_socket() {
    }

    void listen(std::size_t max_pending_connections, Fundamental::error_code& ec) {
        do {
            if (client_context) {
                ec = error::make_error_code(error::rudp_errors::rudp_failed, "rudp client mode can't call listen");
                break;
            }
            if (server_context) {
                ec = error::make_error_code(error::rudp_errors::rudp_failed, "rudp server has already listened");
                break;
            }
            server_context = std::make_unique<rudp_server_context_imp>(this, max_pending_connections);
            ec             = server_context->listen();
        } while (0);
    }

    void rudp_async_connect(const std::string& address,
                            std::uint16_t port,
                            const std::function<void(Fundamental::error_code)>& complete_func) {
        asio::post(socket.get_executor(), [this, ref = weak_from_this(), address, port, complete_func]() {
            auto strong = ref.lock();
            do {
                if (!strong) {
                    if (complete_func)
                        complete_func(
                            error::make_error_code(error::rudp_errors::rudp_failed, "rudp socket has been released"));
                    break;
                }
                if (server_context) {
                    if (complete_func)
                        complete_func(error::make_error_code(error::rudp_errors::rudp_failed,
                                                             "rudp server mode can't call connect"));
                    break;
                }
                if (!client_context) client_context = std::make_unique<rudp_client_context_imp>(this);
                client_context->connect(address, port, complete_func);
            } while (0);
        });
    }

    void async_rudp_wait_connect(const std::function<void(Fundamental::error_code)>& complete_func,
                                 std::size_t max_wait_ms) {
        asio::post(socket.get_executor(), [this, ref = weak_from_this(), max_wait_ms, complete_func]() {
            auto strong = ref.lock();
            do {
                if (!strong) {
                    if (complete_func)
                        complete_func(
                            error::make_error_code(error::rudp_errors::rudp_failed, "rudp socket has been released"));
                    break;
                }
                if (client_context || server_context) {
                    if (complete_func)
                        complete_func(error::make_error_code(error::rudp_errors::rudp_operation_in_progress));
                    break;
                }
                client_context = std::make_unique<rudp_client_context_imp>(this);
                server_context = std::make_unique<rudp_server_context_imp>(this, complete_func);
                server_context->async_rudp_wait_connect(max_wait_ms);
            } while (0);
        });
    }

    void async_rudp_send(const void* buf,
                         std::size_t len,
                         const std::function<void(std::size_t, Fundamental::error_code)>& complete_func) {
        asio::post(socket.get_executor(), [this, ref = weak_from_this(), buf, len, complete_func]() {
            auto strong = ref.lock();
            do {
                if (!strong) {
                    if (complete_func)
                        complete_func(0, error::make_error_code(error::rudp_errors::rudp_failed,
                                                                "rudp socket has been released"));
                    break;
                }
                if (!client_context || !client_context->is_connected()) {
                    if (complete_func)
                        complete_func(0,
                                      error::make_error_code(error::rudp_errors::rudp_failed, "rudp is not connected"));
                    return;
                }
                client_context->rudp_send(buf, len, complete_func);
            } while (0);
        });
    }

    void async_rudp_recv(void* buf,
                         std::size_t len,
                         const std::function<void(std::size_t, Fundamental::error_code)>& complete_func) {
        asio::post(socket.get_executor(), [this, ref = weak_from_this(), buf, len, complete_func]() {
            auto strong = ref.lock();
            do {
                if (!strong) {
                    if (complete_func)
                        complete_func(0, error::make_error_code(error::rudp_errors::rudp_failed,
                                                                "rudp socket has been released"));
                    break;
                }
                if (!client_context || !client_context->is_connected()) {
                    if (complete_func)
                        complete_func(0,
                                      error::make_error_code(error::rudp_errors::rudp_failed, "rudp is not connected"));
                    return;
                }
                client_context->rudp_recv(buf, len, complete_func);
            } while (0);
        });
    }

    void async_rudp_accept(asio::io_context& ios,
                           const std::function<void(rudp_handle_t handle, Fundamental::error_code)>& complete_func) {

        asio::post(socket.get_executor(), [this, ref = weak_from_this(), &ios, complete_func]() {
            auto strong = ref.lock();
            do {
                if (!strong) {
                    if (complete_func)
                        complete_func({}, error::make_error_code(error::rudp_errors::rudp_failed,
                                                                 "rudp socket has been released"));
                    break;
                }
                if (!server_context || !server_context->is_listening()) {
                    if (complete_func)
                        complete_func({}, error::make_error_code(error::rudp_errors::rudp_failed,
                                                                 "rudp server is not listening"));
                    return;
                }
                server_context->async_rudp_accept(ios, complete_func);
            } while (0);
        });
    }
    //
    void preinit_passive_client(Fundamental::error_code& ec, const rudp_socket& server_socket) {
        do {
            if (server_context) {
                ec = error::make_error_code(error::rudp_errors::rudp_failed, "invalid listen server");
                break;
            }
            if (client_context) {
                ec = error::make_error_code(error::rudp_errors::rudp_failed, "client has been inited");
                break;
            }
            client_context = std::make_unique<rudp_client_context_imp>(this);
            copy_config(server_socket);
        } while (0);
    }

    void destroy(Fundamental::error_code ec) {
        if (close_flag.load()) return;
        {
            std::scoped_lock<std::mutex> locker(rudp::rudp_kernel::data_mutex);
            rudp_kernel::storage_.erase(id_);
        }
        asio::post(socket.get_executor(), [this, ref = shared_from_this(), ec]() {
            auto expected_value = false;
            if (close_flag.compare_exchange_strong(expected_value, true, std::memory_order::memory_order_relaxed)) {
                FDEBUG("rudp socket {} closed local:{}:{} remote:{}:{}", id_,
                       socket.local_endpoint().address().to_string(), socket.local_endpoint().port(),
                       remote_endpoint.address().to_string(), remote_endpoint.port());
                // remove reference
                if (client_context) client_context->destroy(ec);
                if (server_context) server_context->destroy(ec);
                std::error_code e;
                socket.close(e);
            }
        });
    }
    bool is_closed() const {
        return close_flag.load();
    }
    void accept_assign_executor(asio::io_context& ios) {
        if (!client_context || !client_context->is_connected()) return;
        client_context->closed_cb    = nullptr;
        client_context->connected_cb = nullptr;
#ifndef _WIN32
        auto native_sock = socket.native_handle();
        auto protocal    = socket.local_endpoint().protocol();
        Fundamental::error_code ec;
        socket.release(ec);
        if (ec) return;
        socket = std::move(asio::ip::udp::socket(ios, protocal, native_sock));
        client_context->accept_assign_executor(ios);
#endif // !_WIN32
    }

    decltype(auto) get_executor() {
        return socket.get_executor();
    }

    void copy_config(const rudp_socket& server_socket) {
        connection_timeout_msec      = server_socket.connection_timeout_msec;
        command_max_try_cnt          = server_socket.command_max_try_cnt;
        send_window_size             = server_socket.send_window_size;
        recv_window_size             = server_socket.recv_window_size;
        mtu_size                     = server_socket.mtu_size;
        enable_no_delay              = server_socket.enable_no_delay;
        update_interval_ms           = server_socket.update_interval_ms;
        resend_skip_cnt              = server_socket.resend_skip_cnt;
        enable_no_congestion_control = server_socket.enable_no_congestion_control;
        stream_mode                  = server_socket.stream_mode;
        auto_keepalive               = server_socket.auto_keepalive;
        max_connection_idle_time     = server_socket.max_connection_idle_time;
    }
    //
    asio::ip::udp::socket socket;
    asio::ip::udp::endpoint remote_endpoint;
    const rudp_id_t id_ = control_frame_data::kControlMagicNum;

    // config
    rudp_config_item connection_timeout_msec      = rudp_kernel::kConnectTimeoutMs;
    rudp_config_item command_max_try_cnt          = rudp_kernel::kCommandMaxTryCnt;
    rudp_config_item send_window_size             = rudp_kernel::kMaxSendWindowSize;
    rudp_config_item recv_window_size             = rudp_kernel::kMaxRecvWindowSize;
    rudp_config_item mtu_size                     = rudp_kernel::kMtuSize;
    rudp_config_item enable_no_delay              = rudp_kernel::kEnableNoDelay;
    rudp_config_item update_interval_ms           = rudp_kernel::kUpdateIntervalMs;
    rudp_config_item resend_skip_cnt              = rudp_kernel::kResendSkipCnt;
    rudp_config_item enable_no_congestion_control = rudp_kernel::kEnbableNoCongestionControl;
    rudp_config_item stream_mode                  = rudp_kernel::kEnbableStreamMode;
    rudp_config_item auto_keepalive               = rudp_kernel::kEnbableAutoKeepAlive;
    rudp_config_item max_connection_idle_time     = rudp_kernel::kMaxConnectionIdleTimeMs;
    // runtime data
    std::atomic_bool close_flag = false;
    std::unique_ptr<rudp_client_context_imp> client_context;
    std::unique_ptr<rudp_server_context_imp> server_context;
};

rudp_handle_t rudp_create(asio::io_context& ios, Fundamental::error_code& ec) {
    return std::make_shared<rudp_handle>(rudp_kernel::create_socket(ios.get_executor(), ec));
}

void rudp_bind(rudp_handle_t handle, std::uint16_t port, std::string address, Fundamental::error_code& ec) {
    do {
        auto actual_handle = handle->get();
        if (!actual_handle || actual_handle->is_closed()) break;
        ec = error::make_error_code(network::protocal_helper::udp_bind_endpoint(actual_handle->socket, port, address),
                                    "rudp bind");
        return;
    } while (0);
    ec = error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor);
}

void rudp_listen(rudp_handle_t handle, std::size_t max_pending_connections, Fundamental::error_code& ec) {
    do {
        auto actual_handle = handle->get();
        if (!actual_handle || actual_handle->is_closed()) break;
        actual_handle->listen(max_pending_connections, ec);
        return;
    } while (0);
    ec = error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor);
}

void async_rudp_connect(rudp_handle_t handle,
                        const std::string& address,
                        std::uint16_t port,
                        const std::function<void(Fundamental::error_code)>& complete_func) {
    do {
        auto actual_handle = handle->get();
        if (!actual_handle || actual_handle->is_closed()) break;
        actual_handle->rudp_async_connect(address, port, complete_func);
        return;
    } while (0);
    if (complete_func) complete_func(error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor));
}

void async_rudp_wait_connect(rudp_handle_t handle,
                             const std::function<void(Fundamental::error_code)>& complete_func,
                             std::size_t max_wait_ms) {
    do {
        auto actual_handle = handle->get();
        if (!actual_handle || actual_handle->is_closed()) break;
        actual_handle->async_rudp_wait_connect(complete_func, max_wait_ms);
        return;
    } while (0);
    if (complete_func) complete_func(error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor));
}

void async_rudp_send(rudp_handle_t handle,
                     const void* buf,
                     std::size_t len,
                     const std::function<void(std::size_t, Fundamental::error_code)>& complete_func) {
    do {
        auto actual_handle = handle->get();
        if (!actual_handle || actual_handle->is_closed()) break;
        actual_handle->async_rudp_send(buf, len, complete_func);
        return;
    } while (0);
    if (complete_func) complete_func(0, error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor));
}

void async_rudp_recv(rudp_handle_t handle,
                     void* buf,
                     std::size_t len,
                     const std::function<void(std::size_t, Fundamental::error_code)>& complete_func) {
    do {
        auto actual_handle = handle->get();
        if (!actual_handle || actual_handle->is_closed()) break;
        actual_handle->async_rudp_recv(buf, len, complete_func);
        return;
    } while (0);
    if (complete_func) complete_func(0, error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor));
}

void async_rudp_accept(rudp_handle_t handle,
                       asio::io_context& ios,
                       const std::function<void(rudp_handle_t handle, Fundamental::error_code)>& complete_func) {
    do {
        auto actual_handle = handle->get();
        if (!actual_handle || actual_handle->is_closed()) break;
        actual_handle->async_rudp_accept(ios, complete_func);
        return;
    } while (0);
    if (complete_func) complete_func({}, error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor));
}

void rudp_config_sys(rudp_config_type type, std::size_t value) {
    switch (type) {
    case rudp_config_type::RUDP_COMMAND_MAX_TRY_CNT: rudp_kernel::kCommandMaxTryCnt.set_value(value); break;
    case rudp_config_type::RUDP_ENABLE_NO_CONGESTION_CONTROL:
        rudp_kernel::kEnbableNoCongestionControl.set_value(value);
        break;
    case rudp_config_type::RUDP_CONNECT_TIMEOUT_MS: rudp_kernel::kConnectTimeoutMs.set_value(value); break;
    case rudp_config_type::RUDP_FASK_RESEND_SKIP_CNT: rudp_kernel::kResendSkipCnt.set_value(value); break;
    case rudp_config_type::RUDP_MAX_RECV_WINDOW_SIZE: rudp_kernel::kMaxRecvWindowSize.set_value(value); break;
    case rudp_config_type::RUDP_MAX_SEND_WINDOW_SIZE: rudp_kernel::kMaxSendWindowSize.set_value(value); break;
    case rudp_config_type::RUDP_MTU_SIZE: rudp_kernel::kMtuSize.set_value(value); break;
    case rudp_config_type::RUDP_ENABLE_NO_DELAY: rudp_kernel::kEnableNoDelay.set_value(value); break;
    case rudp_config_type::RUDP_UPDATE_INTERVAL_MS: rudp_kernel::kUpdateIntervalMs.set_value(value); break;
    case rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE: rudp_kernel::kEnbableAutoKeepAlive.set_value(value); break;
    case rudp_config_type::RUDP_ENABLE_STREAM_MODE: rudp_kernel::kEnbableStreamMode.set_value(value); break;
    case rudp_config_type::RUDP_MAX_IDLE_CONNECTION_TIME_MS:
        rudp_kernel::kMaxConnectionIdleTimeMs.set_value(value);
        break;
    default: break;
    }
}

void rudp_config(rudp_handle_t handle, rudp_config_type type, std::size_t value) {
    auto actual_handle = handle->get();
    if (!actual_handle || actual_handle->is_closed()) return;
    switch (type) {
    case rudp_config_type::RUDP_COMMAND_MAX_TRY_CNT: actual_handle->command_max_try_cnt.set_value(value); break;
    case rudp_config_type::RUDP_ENABLE_NO_CONGESTION_CONTROL:
        actual_handle->enable_no_congestion_control.set_value(value);
        break;
    case rudp_config_type::RUDP_CONNECT_TIMEOUT_MS: actual_handle->connection_timeout_msec.set_value(value); break;
    case rudp_config_type::RUDP_FASK_RESEND_SKIP_CNT: actual_handle->resend_skip_cnt.set_value(value); break;
    case rudp_config_type::RUDP_MAX_RECV_WINDOW_SIZE: actual_handle->recv_window_size.set_value(value); break;
    case rudp_config_type::RUDP_MAX_SEND_WINDOW_SIZE: actual_handle->send_window_size.set_value(value); break;
    case rudp_config_type::RUDP_MTU_SIZE: actual_handle->mtu_size.set_value(value); break;
    case rudp_config_type::RUDP_ENABLE_NO_DELAY: actual_handle->enable_no_delay.set_value(value); break;
    case rudp_config_type::RUDP_UPDATE_INTERVAL_MS: actual_handle->update_interval_ms.set_value(value); break;
    case rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE: actual_handle->auto_keepalive.set_value(value); break;
    case rudp_config_type::RUDP_ENABLE_STREAM_MODE: actual_handle->stream_mode.set_value(value); break;
    case rudp_config_type::RUDP_MAX_IDLE_CONNECTION_TIME_MS:
        actual_handle->max_connection_idle_time.set_value(value);
        break;
    default: break;
    }
}

Fundamental::error_code rudp_server_context_imp::listen() {
    if (max_pending_connections == 0 || max_pending_connections > 4096)
        return error::make_error_code(error::rudp_errors::rudp_failed, "invalid pending connetions cnt");
    if (wait_unique_connection_flag.load(std::memory_order::memory_order_seq_cst)) {
        return error::make_error_code(error::rudp_errors::rudp_failed,
                                      "invalid listen request on a passive waiting socket");
    }
    bool expected_value = false;
    if (listen_flag.compare_exchange_strong(expected_value, true, std::memory_order::memory_order_seq_cst)) {
        asio::post(socket_ref->socket.get_executor(), [this, ref = socket_ref->weak_from_this()](

                                                      ) {
            auto strong = ref.lock();
            if (!strong) return;
            recv_buf.resize(socket_ref->mtu_size.max_value);
            peform();
        });
    }
    return Fundamental::error_code();
}

void rudp_server_context_imp::async_rudp_wait_connect(std::size_t max_wait_ms) {
    timer.expires_after(std::chrono::milliseconds(max_wait_ms));
    timer.async_wait([this, ref = socket_ref->weak_from_this(), max_wait_ms](std::error_code ec) {
        if (ec) return;
        auto strong = ref.lock();
        if (!strong) return;
        socket_ref->destroy(
            error::make_error_code(error::rudp_errors::rudp_timed_out,
                                   Fundamental::StringFormat("wait connection timeout for {} ms", max_wait_ms)));
    });
    recv_buf.resize(control_frame_data::kRudpControlFrameSize);
    process_wait_connect();
}

rudp_server_context_imp::rudp_server_context_imp(rudp_socket* parent, std::size_t max_pending_connections) :
socket_ref(parent), timer(socket_ref->get_executor()), max_pending_connections(max_pending_connections) {
}
rudp_server_context_imp::rudp_server_context_imp(rudp_socket* parent,
                                                 const std::function<void(Fundamental::error_code)>& complete_func) :
socket_ref(parent), timer(socket_ref->get_executor()), max_pending_connections(1), wait_unique_connection_flag(true),
wait_unique_connection_cb(complete_func) {
}

void rudp_server_context_imp::async_rudp_accept(
    asio::io_context& ios,
    const std::function<void(rudp_handle_t handle, Fundamental::error_code)>& complete_func) {
    asio::post(socket_ref->socket.get_executor(), [=, ref = socket_ref->weak_from_this(), p = &ios]() mutable {
        auto strong = ref.lock();
        if (!strong || !is_listening()) {
            if (complete_func)
                complete_func({}, error::make_error_code(error::rudp_errors::rudp_failed, "rudp socket was closed"));
            return;
        }
        // add new acceptor
        accept_list.emplace_back(rudp_accept_session { *p, complete_func });
        accept_connection();
    });
}

void rudp_server_context_imp::process_wait_connect() {
    socket_ref->socket.async_receive_from(
        asio::buffer(recv_buf.data(), recv_buf.size()), sender_endpoint,
        [this, ref = socket_ref->weak_from_this()](asio::error_code ec, std::size_t len) {
            auto strong = ref.lock();
            if (!strong || !wait_unique_connection_flag.load()) return;
            do {
                if (ec) break;
                if (len != control_frame_data::kRudpControlFrameSize) break;
                if (!last_control_frame.decode(recv_buf.data(), len)) break;
                if (last_control_frame.command != rudp_command_t::RUDP_SYN_COMMAND) break;
                timer.cancel();
                auto complete_func = std::move(wait_unique_connection_cb);
                socket_ref->client_context->unique_passive_connect(sender_endpoint, last_control_frame.src_id,
                                                                   last_control_frame.payload, complete_func);
                return;
            } while (0);
            process_wait_connect();
        });
}

void network::rudp::rudp_server_context_imp::peform() {
    socket_ref->socket.async_receive_from(
        asio::buffer(recv_buf.data(), recv_buf.size()), sender_endpoint,
        [this, ref = socket_ref->weak_from_this()](asio::error_code ec, std::size_t len) {
            auto strong = ref.lock();
            if (!strong || !is_listening()) return;
            do {
                if (ec) break;
                if (filter_data(recv_buf.data(), len)) break;
                if (len != control_frame_data::kRudpControlFrameSize) break;
                if (!last_control_frame.decode(recv_buf.data(), len)) break;
                // ignore  request when queue is full
                if (recv_dic.size() + connected_list.size() >= max_pending_connections) {
                    break;
                }
                handle_request();
            } while (0);
            peform();
        });
}

bool rudp_server_context_imp::is_listening() const {
    return listen_flag.load();
}

bool rudp_server_context_imp::filter_data(const void* data, std::size_t data_len) {

    do {
        if (data_len == 0) break;
        auto iter = filter_table.find(sender_endpoint);
        if (iter == filter_table.end()) break;
        auto strong = iter->second.lock();
        if (!strong || strong->is_closed()) {
            filter_table.erase(iter);
            break;
        }
        // copy data
        std::vector<std::uint8_t> copy_data;
        copy_data.resize(data_len);
        std::memcpy(copy_data.data(), data, data_len);
        asio::post(strong->get_executor(), [ref = iter->second, new_data = std::move(copy_data)]() {
            auto c = ref.lock();
            if (!c || c->is_closed()) return;
            c->client_context->process_read_data(new_data.data(), new_data.size());
        });
        return true;
    } while (0);
    return false;
}

void rudp_server_context_imp::handle_request() {
    switch (last_control_frame.command) {
    case rudp_command_t::RUDP_SYN_COMMAND: handle_syn(); break;
    default: break;
    }
}

void rudp_server_context_imp::handle_syn() {
    FDEBUG(" rudp server {} recv init syn:{} from  {}->{}:{}", socket_ref->id_, last_control_frame.command_ts,
           last_control_frame.src_id, sender_endpoint.address().to_string(), sender_endpoint.port());
    std::shared_ptr<rudp_socket> session;
    do {
        auto iter = recv_dic.find(sender_endpoint);
        if (iter == recv_dic.end()) { // new session
            Fundamental::error_code ec;
            session = rudp_kernel::create_socket(socket_ref->get_executor(), ec);
            if (ec) {
                break;
            }
            session->preinit_passive_client(ec, *socket_ref);
            if (ec) break;
            Fundamental::ScopeGuard g([&]() {
                if (session) session->destroy(ec);
            });
            // local bind
            ec = protocal_helper::udp_bind_endpoint(session->socket, socket_ref->socket.local_endpoint());
            if (ec) break;
            recv_dic[sender_endpoint]     = session;
            filter_table[sender_endpoint] = session;
            g.dismiss();
            FDEBUG("server {}:{} new pending connection start {} ", socket_ref->socket.local_endpoint().port(),
                   socket_ref->id_, session->id_);
        } else {
            session = iter->second;
            // connection has changed,reject it
            if (session->client_context->remote_id != last_control_frame.src_id) break;
        }
        session->client_context->passive_connect(
            sender_endpoint, last_control_frame.src_id, last_control_frame.payload,
            [this, sender_endpoint_copy = sender_endpoint, ref = socket_ref->weak_from_this(),
             session_ref = session->weak_from_this()](Fundamental::error_code ec, rudp_connection_status staus) {
                auto strong         = ref.lock();
                auto strong_session = session_ref.lock();
                if (!strong || !strong_session) return;
                on_connection_status_changed(strong_session, staus, ec);
            });
        return;
    } while (0);
}

void rudp_server_context_imp::on_connection_status_changed(std::shared_ptr<rudp_socket> _connection,
                                                           rudp_connection_status current_status,
                                                           Fundamental::error_code ec) {
    asio::post(socket_ref->get_executor(), [this, ec, current_status, ref = socket_ref->weak_from_this(),
                                            connection_ref = _connection->weak_from_this()]() {
        auto strong     = ref.lock();
        auto connection = connection_ref.lock();
        // may connection has been removed
        if (!strong || !connection) return;
        if (ec) {
            filter_table.erase(connection->remote_endpoint);
            bool has_found = recv_dic.erase(connection->remote_endpoint) > 0;
            for (auto iter = connected_list.begin(); iter != connected_list.end(); ++iter) {
                if (iter->get() == connection.get()) {
                    has_found = true;
                    connected_list.erase(iter);
                    break;
                }
            }
            if (has_found) {
                FDEBUG("server {}:{} accept list remove {} ", socket_ref->socket.local_endpoint().port(),
                       socket_ref->id_, connection->id_);
            }
            return;
        }
        switch (current_status) {
        case rudp_connection_status::RUDP_CONNECTED_STATUS: {
            FDEBUG("server {}:{} new pending connection completed {} ", socket_ref->socket.local_endpoint().port(),
                   socket_ref->id_, connection->id_);
            recv_dic.erase(connection->remote_endpoint);
            connected_list.push_back(connection);
            accept_connection();
        } break;
        case rudp_connection_status::RUDP_CLOSED_STATUS: break;
        default: break;
        }
    });
}

void rudp_server_context_imp::accept_connection() {
    if (connected_list.empty() || accept_list.empty()) return;
    auto accept_sock = std::move(connected_list.front());
    connected_list.pop_front();
    auto& ios          = accept_list.front().ios;
    auto complete_func = accept_list.front().complete_func;
    accept_list.pop_front();
    FDEBUG("server {}:{} accept {} left [{}/{}]", socket_ref->socket.local_endpoint().port(), socket_ref->id_,
           accept_sock->id_, connected_list.size(), accept_list.size());
    accept_sock->accept_assign_executor(ios);
    accept_sock->client_context->closed_cb = [this, ref = socket_ref->weak_from_this(),
                                              endpoint = accept_sock->remote_endpoint](Fundamental::error_code) {
        auto strong = ref.lock();
        if (!strong) return;
        asio::dispatch(strong->get_executor(), [this, ref, endpoint]() {
            auto strong = ref.lock();
            if (!strong) return;
            filter_table.erase(endpoint);
        });
    };
    asio::post(ios, [func = complete_func, accept_handle = std::make_shared<rudp_handle>(accept_sock)]() {
        if (func) func(accept_handle, {});
    });
}
void rudp_server_context_imp::destroy(Fundamental::error_code ec) {
    listen_flag.exchange(false);
    wait_unique_connection_flag.exchange(false);
    {
        auto tmp = std::move(recv_dic);
        for (auto& item : tmp) {
            FDEBUG("server {}:{} pending list force remove {} ", socket_ref->socket.local_endpoint().port(),
                   socket_ref->id_, item.second->id_);
            item.second->destroy(ec);
        }
    }

    {
        auto tmp = std::move(connected_list);
        for (auto& item : tmp) {
            FDEBUG("server {}:{} connected list force remove {} ", socket_ref->socket.local_endpoint().port(),
                   socket_ref->id_, item->id_);
            item->destroy(ec);
        }
    }
    {
        auto tmp = std::move(accept_list);
        for (auto& item : tmp) {
            if (item.complete_func)
                item.complete_func(
                    {}, error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor,
                                               Fundamental::StringFormat("rudp server was released {}", ec)));
        }
    }
    {
        auto finish_func = std::move(wait_unique_connection_cb);
        if (finish_func) finish_func(ec);
    }
}

bool rudp_client_context_imp::is_connected() const {
    return status == rudp_connection_status::RUDP_CONNECTED_STATUS;
}

bool rudp_client_context_imp::is_active() const {
    return status > rudp_connection_status::RUDP_INIT_STATUS;
}

bool rudp_client_context_imp::is_closed() const {
    return status == rudp_connection_status::RUDP_CLOSED_STATUS;
}

void rudp_client_context_imp::destroy(Fundamental::error_code e) {
    FDEBUG("{} destroy client context", socket_ref->id_);
    Fundamental::error_code ec =
        error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor,
                               Fundamental::StringFormat("rudp descriptor was destroy by reason {}", e));
    set_status(rudp_connection_status::RUDP_CLOSED_STATUS, ec);

    {
        auto tmp = std::move(pending_rudp_read_requests);
        for (auto& item : tmp) {
            item.complete_func(0, ec);
        }
    }
    {
        auto tmp = std::move(pending_rudp_write_requests);
        for (auto& item : tmp) {
            item.complete_func(0, ec);
        }
    }
    pending_control_frames.clear();
    pending_data_frames.clear();
    update_timer.cancel();
    status_timer.cancel();
    health_check_timer.cancel();
}

void rudp_client_context_imp::set_status(rudp_connection_status dst_status, Fundamental::error_code ec) {
    if (status == dst_status) return;
    auto old_status = status;
    status          = dst_status;
    switch (status) {
    case rudp_connection_status::RUDP_CONNECTED_STATUS: {
        if (old_status == rudp_connection_status::RUDP_SYN_SENT_STATUS ||
            old_status == rudp_connection_status::RUDP_SYN_RECV_STATUS) { // new connection setup
            FDEBUG("new rudp connection   local[{}]->{}:{} remote[{}]->{}:{}", socket_ref->id_,
                   socket_ref->socket.local_endpoint().address().to_string(),
                   socket_ref->socket.local_endpoint().port(),

                   remote_id, socket_ref->remote_endpoint.address().to_string(), socket_ref->remote_endpoint.port());
            send_ping();
            auto call_cb = std::move(connected_cb);
            if (call_cb) call_cb({});
            update_active_time();
            health_check();
            request_update_rudp_status();
        }
    } break;
    case rudp_connection_status::RUDP_CLOSED_STATUS: {
        if (!ec) {
            ec = error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor, "rudp descriptor was destroy");
        }

        send_rst();
        FDEBUG(" rudp connection disconnected  local[{}]->{}:{} remote[{}]->{}:{}", socket_ref->id_,
               socket_ref->socket.local_endpoint().address().to_string(), socket_ref->socket.local_endpoint().port(),
               remote_id, socket_ref->remote_endpoint.address().to_string(), socket_ref->remote_endpoint.port());
        {
            auto call_cb = std::move(connected_cb);
            if (call_cb) call_cb(ec);
        }
        {
            auto call_cb = std::move(closed_cb);
            if (call_cb) call_cb(ec);
        }
    } break;
    default: break;
    }
}

void rudp_client_context_imp::passive_connect(
    asio::ip::udp::endpoint sender_endpoint,
    rudp_id_t src_id,
    std::uint32_t payload,
    const std::function<void(Fundamental::error_code, rudp_connection_status)>& complete_func) {
    if (is_closed()) {
        if (complete_func)
            complete_func(
                error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor, "rudp server was released"),
                status);
        return;
    }
    if (status == rudp_connection_status::RUDP_INIT_STATUS) { // init
        std::size_t remote_mtu_size = payload & 0xffffff;
        std::size_t stream_mode     = (payload >> 24) & 0x1;
        socket_ref->remote_endpoint = sender_endpoint;
        remote_id                   = src_id;

        std::error_code ec;
        // remote bind
        socket_ref->socket.connect(sender_endpoint, ec);
        if (ec) {
            if (complete_func) complete_func(ec, status);
            return;
        }
        // remote request stream mode but server is not supported
        if (stream_mode && !socket_ref->stream_mode.current_value) {
            send_rst();
            if (complete_func)
                complete_func(error::make_error_code(error::rudp_errors::rudp_invalid_argument,
                                                     "rudp server not supported stream mode"),
                              status);
            return;
        }
        set_status(rudp_connection_status::RUDP_SYN_RECV_STATUS);

        connected_cb = std::bind(complete_func, std::placeholders::_1, rudp_connection_status::RUDP_CONNECTED_STATUS);
        closed_cb    = std::bind(complete_func, std::placeholders::_1, rudp_connection_status::RUDP_CLOSED_STATUS);
        // use the smallest mtu for communication
        if (socket_ref->mtu_size.current_value > remote_mtu_size) {
            socket_ref->mtu_size.set_value(remote_mtu_size);
        }

        socket_read_cache.resize(socket_ref->mtu_size + 32);
        perform_read();
    }
    asio::post(socket_ref->get_executor(), [this, ref = socket_ref->weak_from_this()]() {
        auto strong = ref.lock();
        if (!strong) return;
        send_ack();
    });
}

void rudp_client_context_imp::unique_passive_connect(
    asio::ip::udp::endpoint sender_endpoint,
    rudp_id_t src_id,
    std::uint32_t payload,
    const std::function<void(Fundamental::error_code)>& complete_func) {
    if (is_closed()) {
        if (complete_func)
            complete_func(
                error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor, "rudp server was released"));
        return;
    }
    if (status == rudp_connection_status::RUDP_INIT_STATUS) { // init
        std::size_t remote_mtu_size = payload & 0xffffff;
        std::size_t stream_mode     = (payload >> 24) & 0x1;
        socket_ref->remote_endpoint = sender_endpoint;
        remote_id                   = src_id;

        std::error_code ec;
        // remote bind
        socket_ref->socket.connect(sender_endpoint, ec);
        if (ec) {
            if (complete_func) complete_func(ec);
            return;
        }
        // remote request stream mode but server is not supported
        if (stream_mode && !socket_ref->stream_mode.current_value) {
            send_rst();
            if (complete_func)
                complete_func(error::make_error_code(error::rudp_errors::rudp_invalid_argument,
                                                     "rudp server not supported stream mode"));
            return;
        }
        set_status(rudp_connection_status::RUDP_SYN_RECV_STATUS);

        connected_cb = complete_func;
        // use the smallest mtu for communication
        if (socket_ref->mtu_size.current_value > remote_mtu_size) {
            socket_ref->mtu_size.set_value(remote_mtu_size);
        }
        socket_read_cache.resize(socket_ref->mtu_size + 32);
        perform_read();
    }
    asio::post(socket_ref->get_executor(), [this, ref = socket_ref->weak_from_this()]() {
        auto strong = ref.lock();
        if (!strong) return;
        send_ack();
    });
}

void rudp_client_context_imp::send_ack() {
    if (is_connected()) return;
    control_frame_data ack_frame;
    ack_frame.src_id     = socket_ref->id_;
    ack_frame.dst_id     = remote_id;
    ack_frame.command    = rudp_command_t::RUDP_SYN_ACK_COMMAND;
    ack_frame.command_ts = command_ts++;
    ack_frame.payload    = static_cast<decltype(ack_frame.payload)>(socket_ref->mtu_size.current_value);
    auto send_data       = ack_frame.encode();
    write_data(std::move(send_data), true);
    status_check_command = rudp_command_t::RUDP_SYN_ACK2_COMMAND;
    restart_status_timer();
}

void rudp_client_context_imp::send_syn() {
    if (status != rudp_connection_status::RUDP_SYN_SENT_STATUS) return;
    control_frame_data frame;
    frame.src_id     = socket_ref->id_;
    frame.dst_id     = remote_id;
    frame.command    = rudp_command_t::RUDP_SYN_COMMAND;
    frame.command_ts = command_ts++;
    frame.payload    = (static_cast<decltype(frame.payload)>(socket_ref->mtu_size.current_value) & 0xffffff) |
                    (static_cast<decltype(frame.payload)>(socket_ref->stream_mode.current_value) << 24);
    auto send_data = frame.encode();
    write_data(std::move(send_data), true);
    status_check_command = rudp_command_t::RUDP_SYN_ACK_COMMAND;
    restart_status_timer();
}

void rudp_client_context_imp::send_ack2() {
    control_frame_data frame;
    frame.src_id     = socket_ref->id_;
    frame.dst_id     = remote_id;
    frame.command    = rudp_command_t::RUDP_SYN_ACK2_COMMAND;
    frame.command_ts = command_ts++;
    auto send_data   = frame.encode();
    write_data(std::move(send_data), true);
}

void rudp_client_context_imp::send_ping() {
    if (!is_connected()) return;
    if (!socket_ref->auto_keepalive.current_value) return;
    control_frame_data frame;
    frame.src_id     = socket_ref->id_;
    frame.dst_id     = remote_id;
    frame.command    = rudp_command_t::RUDP_PING_COMMAND;
    frame.command_ts = command_ts++;
    auto send_data   = frame.encode();
    write_data(std::move(send_data), true);
    status_check_command = rudp_command_t::RUDP_PONG_COMMAND;
    restart_status_timer();
}

void rudp_client_context_imp::send_pong() {
    control_frame_data frame;
    frame.src_id     = socket_ref->id_;
    frame.dst_id     = remote_id;
    frame.command    = rudp_command_t::RUDP_PONG_COMMAND;
    frame.command_ts = command_ts++;
    auto send_data   = frame.encode();
    write_data(std::move(send_data), true);
}

void rudp_client_context_imp::send_rst() {
    if (remote_id == control_frame_data::kControlMagicNum) return;
    control_frame_data frame;
    frame.src_id     = socket_ref->id_;
    frame.dst_id     = remote_id;
    frame.command    = rudp_command_t::RUDP_RST_COMMAND;
    frame.command_ts = command_ts++;
    auto send_data   = frame.encode();
    asio::const_buffer send_buf { send_data.data(), send_data.size() };
    socket_ref->socket.async_send(std::move(send_buf),
                                  [write_data = std::move(send_data)](std::error_code, std::size_t) {});
}

void rudp_client_context_imp::accept_assign_executor(asio::io_context& new_executor) {
    if (!is_connected()) return;
    update_timer       = std::move(asio::steady_timer(new_executor));
    status_timer       = std::move(asio::steady_timer(new_executor));
    health_check_timer = std::move(asio::steady_timer(new_executor));
    // update token to avoid call perform_read twice
    ++read_token;
    asio::post(socket_ref->socket.get_executor(), [this, ref = socket_ref->weak_from_this()] {
        perform_read();
        health_check();
        restart_status_timer();
        request_update_rudp_status();
    });
}

void rudp_client_context_imp::restart_status_timer() {
    status_timer.cancel();
    if (status_check_command == 0) return;
    status_timer.expires_after(std::chrono::milliseconds(socket_ref->connection_timeout_msec));
    status_timer.async_wait([this, ref = socket_ref->weak_from_this()](const asio::error_code& ec) {
        if (ec) return;
        auto strong = ref.lock();
        if (!strong || strong->is_closed()) return;
        if (status_check_command == 0) return;
        ++status_check_command_try_cnt;
        if (status_check_command_try_cnt > socket_ref->command_max_try_cnt) {
            socket_ref->destroy(error::make_error_code(error::rudp_errors::rudp_network_unreachable,
                                                       "resend command for too many times"));
        } else {
            // resend request
            switch (status_check_command) {
            case rudp_command_t::RUDP_SYN_ACK_COMMAND: send_syn(); break;
            case rudp_command_t::RUDP_SYN_ACK2_COMMAND: send_ack(); break;
            case rudp_command_t::RUDP_PONG_COMMAND: send_ping(); break;
            default: break;
            }
        }
    });
}

static std::int32_t __rudp_output__(const char* buf, int len, struct IKCPCB*, void* user) {
    // just pending rudp data
    rudp_client_context_imp* imp = static_cast<rudp_client_context_imp*>(user);
    imp->write_data(buf, len);
    return 0;
}

static void __rudp_on_frame_sent__(int len, void* user) {
    // just pending rudp data
    rudp_client_context_imp* imp = static_cast<rudp_client_context_imp*>(user);
    imp->notify_data_bytes_write(static_cast<std::size_t>(len));
}

[[maybe_unused]] static void __rudp_log__(const char* log, struct IKCPCB* kcp, void* user) {
    // just pending rudp data
    rudp_client_context_imp* imp = static_cast<rudp_client_context_imp*>(user);
    FINFO("rudp {} log:{}", imp->socket_ref->id_, log);
}

void rudp_client_context_imp::init_rudp_output() {
    FASSERT(kcp_object, "your should init kcp object first");
    ikcp_setoutput(kcp_object.get(), __rudp_output__);
    kcp_object.get()->notify_write = __rudp_on_frame_sent__;
    // set stream mode
    kcp_object.get()->stream = socket_ref->stream_mode ? 1 : 0;
#if RUDP_RAW_DEBUG
    kcp_object.get()->writelog = __rudp_log__;
    kcp_object.get()->logmask  = 0xffffffff;
#endif
    ikcp_setmtu(kcp_object.get(), static_cast<std::int32_t>(socket_ref->mtu_size));
    ikcp_wndsize(kcp_object.get(), static_cast<std::int32_t>(socket_ref->send_window_size),
                 static_cast<std::int32_t>(socket_ref->recv_window_size));
    ikcp_nodelay(kcp_object.get(), static_cast<std::int32_t>(socket_ref->enable_no_delay),
                 static_cast<std::int32_t>(socket_ref->update_interval_ms),
                 static_cast<std::int32_t>(socket_ref->resend_skip_cnt),
                 static_cast<std::int32_t>(socket_ref->enable_no_congestion_control));
}

void rudp_client_context_imp::notify_data_bytes_write(std::size_t len) {

    while (len > 0) {
        FASSERT(!pending_rudp_write_requests.empty());
        auto& front = pending_rudp_write_requests.front();
        front.write_offset += len;
        if (front.write_offset >= front.data_len) {
            len = front.write_offset - front.data_len;
            if (front.complete_func) front.complete_func(front.data_len, {});
            pending_rudp_write_requests.pop_front();
        } else {
            // partitial write
            break;
        }
    }
}

void rudp_client_context_imp::request_update_rudp_status() {
    if (!is_connected()) return;
    // check active time for idle connections
    auto now        = get_current_time();
    auto current_ts = static_cast<std::uint32_t>(now & 0xffffffff);
    auto update_ts  = ikcp_check(kcp_object.get(), current_ts);
    // check time loopback
    auto diff_time_ms = update_ts >= current_ts ? update_ts - current_ts
                                                : (std::numeric_limits<std::uint32_t>::max() - current_ts + update_ts);
    // send/recv will process this step
    if (force_rudp_update_flag == rudp_force_update_status::RUDP_FORCE_UPDATE_REQUESTING) {
        constexpr std::uint32_t kActiveRequestInterval = 1;
        if (diff_time_ms > kActiveRequestInterval) {
            diff_time_ms = kActiveRequestInterval;
            update_ts    = current_ts + diff_time_ms;
        }
        force_rudp_update_flag = rudp_force_update_status::RUDP_FORCE_UPDATE_PROCESSING;
    } else {
        // no active request
        diff_time_ms = kcp_object->interval;
        update_ts    = current_ts + diff_time_ms;
    }
    update_timer.cancel();
    update_timer.expires_after(std::chrono::milliseconds(diff_time_ms));
    update_timer.async_wait([this, ref = socket_ref->weak_from_this(), update_ts](std::error_code ec) {
        if (ec) return;
        // maybe canceled
        auto strong = ref.lock();
        if (!strong) return;
        ikcp_update(kcp_object.get(), update_ts);
        if (force_rudp_update_flag == rudp_force_update_status::RUDP_FORCE_UPDATE_PROCESSING) {
            ikcp_flush(kcp_object.get());
        }
        force_rudp_update_flag   = rudp_force_update_status::RUDP_FORCE_UPDATE_NONE;
        auto protocal_cache_size = ikcp_waitsnd(kcp_object.get());
        if (static_cast<std::size_t>(protocal_cache_size) > rudp_kernel::kMaxRudpProtocalCacheNums) {
            FWARN("send cache size:{}  is overflow {},your should control send interval or check the network "
                  "status,active disconnect",
                  protocal_cache_size, rudp_kernel::kMaxRudpProtocalCacheNums);
            socket_ref->destroy(error::make_error_code(
                error::rudp_errors::rudp_failed,
                "rudp protocal cache overflow,maybe network is too poor for your data transmission efficiency"));
        }
        request_update_rudp_status();
    });
}

void rudp_client_context_imp::active_request_update_rudp_status() {
    ikcp_update(kcp_object.get(), get_clock_t());
    if (force_rudp_update_flag == rudp_force_update_status::RUDP_FORCE_UPDATE_NONE) {
        force_rudp_update_flag = rudp_force_update_status::RUDP_FORCE_UPDATE_REQUESTING;
        request_update_rudp_status();
    }
}

void rudp_client_context_imp::comsume_remote_frames() {
    while (!pending_rudp_read_requests.empty()) {

        auto& frame       = std::get<0>(pending_remote_data_frame);
        auto& read_offset = std::get<1>(pending_remote_data_frame);
        auto& request     = pending_rudp_read_requests.front();
        auto need_size    = request.buf_len - request.read_offset;
        while (frame.size() - read_offset < need_size) {
            // need more data
            auto new_frame_size = ikcp_peeksize(kcp_object.get());
            if (new_frame_size <= 0) break;
            std::size_t old_size = frame.size();
            frame.resize(static_cast<std::size_t>(new_frame_size) + old_size);
            auto recv_ret =
                ikcp_recv(kcp_object.get(), reinterpret_cast<char*>(frame.data()) + old_size, new_frame_size);
            FASSERT(recv_ret == new_frame_size, "should never reach this case,maybe internal error");
            if (!socket_ref->stream_mode.current_value) break;
        }
        auto current_data_size = frame.size() - read_offset;
        if (current_data_size == 0) break;
        auto copy_size = current_data_size > need_size ? need_size : current_data_size;
        std::memcpy(static_cast<std::uint8_t*>(request.buf) + request.read_offset, frame.data() + read_offset,
                    copy_size);
        request.read_offset += copy_size;
        read_offset += copy_size;
        auto left_bytes = frame.size() - read_offset;
        // none stream mode
        if (!socket_ref->stream_mode.current_value) {

            request.complete_func(
                request.read_offset,
                left_bytes == 0
                    ? Fundamental::error_code {}
                    : error::make_error_code(
                          error::rudp_errors::rudp_no_buffer_space,
                          Fundamental::StringFormat("discard {} bytes,you should increase buffer or use stream mode",
                                                    left_bytes)));
            pending_rudp_read_requests.pop_front();
            // force clear
            left_bytes = 0;
        } else { // stream mode
            if (request.read_offset == request.buf_len) {
                request.complete_func(request.buf_len, {});
                pending_rudp_read_requests.pop_front();
            }
        }
        if (left_bytes == 0) {
            frame.clear();
        } else { // copy rest bytes
            pending_remote_data_frame_swap_cache.resize(left_bytes);
            std::memcpy(pending_remote_data_frame_swap_cache.data(), frame.data() + read_offset, left_bytes);
            std::swap(pending_remote_data_frame_swap_cache, frame);
        }
        read_offset = 0;
    }
}

void rudp_client_context_imp::update_active_time() {
    last_active_time_ms = get_current_time();
}

void rudp_client_context_imp::perform_read() {
    if (!is_active()) return;
    socket_ref->socket.async_receive(
        asio::buffer(socket_read_cache.data(), socket_read_cache.size()),
        [this, ref = socket_ref->weak_from_this(), token = read_token](std::error_code ec, std::size_t len) {
            auto strong = ref.lock();
            if (!strong || !is_active()) return;
            if (!socket_ref->socket.is_open()) return;

            if (ec && ec.value() != static_cast<std::int32_t>(std::errc::operation_canceled)) {
                FDEBUG("read size:[{}/{}] from {}:{} failed for {}:{}", len, socket_read_cache.size(),
                       strong->remote_endpoint.address().to_string(), strong->remote_endpoint.port(), ec.value(),
                       ec.message());
            }
            Fundamental::ScopeGuard g([&]() {
                // token has been changed
                if (token != read_token) return;
                perform_read();
            });
            process_read_data(socket_read_cache.data(), len);
        });
}

void rudp_client_context_imp::process_read_data(const std::uint8_t* data, std::size_t read_size) {
    if (read_size == control_frame_data::kRudpControlFrameSize) {
        process_control_frame(data);

    } else if (read_size >= 24) { // process data frame
        process_rudp_data_frame(data, read_size);
    } else {
#ifdef DEBUG_RUDP
        if (read_size > 0) {
            // invalid payload will be ignored
            FDEBUG("rudp:{} truncate {} bytes", socket_ref->id_, read_size);
        }
#endif
    }
}

void rudp_client_context_imp::process_rudp_data_frame(const std::uint8_t* data, std::size_t read_size) {
#ifdef DEBUG_RUDP
    FDEBUG(" rudp connection {} recv payload size:{} from  {}:{}", socket_ref->id_, read_size,
           socket_ref->remote_endpoint.address().to_string(), socket_ref->remote_endpoint.port());
#endif
    if (!is_connected()) return;
    auto ret = ikcp_input(kcp_object.get(), reinterpret_cast<const char*>(data), static_cast<long>(read_size));
    // ignore invalid data
    if (ret != 0) return;
    update_active_time();
    comsume_remote_frames();
    active_request_update_rudp_status();
}

void rudp_client_context_imp::process_control_frame(const std::uint8_t* data) {
    // ignored invalid frame
    if (!recv_control_frame.decode(data, control_frame_data::kRudpControlFrameSize)) return;

    std::uint8_t supported_mask = supported_commands();

    if ((recv_control_frame.command & supported_mask) == 0) return;
    // verify src
    if ((recv_control_frame.command & rudp_command_t::RUDP_VERIFY_SRC_MASK) != 0 &&
        (recv_control_frame.src_id != remote_id || recv_control_frame.dst_id != socket_ref->id_))
        return;
    switch (recv_control_frame.command) {
    case rudp_command_t::RUDP_SYN_COMMAND: handle_syn(); break;
    case rudp_command_t::RUDP_SYN_ACK_COMMAND: handle_syn_ack(); break;
    case rudp_command_t::RUDP_SYN_ACK2_COMMAND: handle_syn_ack2(); break;
    case rudp_command_t::RUDP_PING_COMMAND: handle_ping(); break;
    case rudp_command_t::RUDP_PONG_COMMAND: handle_pong(); break;
    case rudp_command_t::RUDP_RST_COMMAND: handle_rst(); break;
    default: break;
    }
}

std::uint8_t rudp_client_context_imp::supported_commands() const noexcept {
    switch (status) {
    case rudp_connection_status::RUDP_CONNECTED_STATUS: return rudp_command_t::RUDP_CONNECTED_MASK;
    case rudp_connection_status::RUDP_SYN_RECV_STATUS: return rudp_command_t::RUDP_SERVER_INIT_MASK;
    case rudp_connection_status::RUDP_SYN_SENT_STATUS: return rudp_command_t::RUDP_CLIENT_INIT_MASK;
    default: return rudp_command_t::RUDP_UNKNOWN_COMMAND;
    }
}

void rudp_client_context_imp::handle_syn() {
    // may remote endpoint resent syn
    // just send syn ack
    send_ack();
}

void rudp_client_context_imp::handle_syn_ack() {
    // ignore invalid data
    if (recv_control_frame.dst_id != socket_ref->id_) {
        return;
    }
    FDEBUG(" rudp connection {} recv syn ack:{} from  {}->{}:{}", socket_ref->id_, recv_control_frame.command_ts,
           recv_control_frame.src_id, socket_ref->remote_endpoint.address().to_string(),
           socket_ref->remote_endpoint.port());

    if (status == rudp_connection_status::RUDP_SYN_SENT_STATUS) {
        // update local mtu,use the smallest mtu for communication
        if (socket_ref->mtu_size.current_value > recv_control_frame.payload) {
            socket_ref->mtu_size.set_value(recv_control_frame.payload);
        }
        socket_read_cache.resize(socket_ref->mtu_size + 32);
        remote_id = recv_control_frame.src_id;
        if (status_check_command == rudp_command_t::RUDP_SYN_ACK_COMMAND) {
            status_check_command = rudp_command_t::RUDP_UNKNOWN_COMMAND;
            // reset try_cnt
            status_check_command_try_cnt = 0;
        }
        // use remote id as kcp conv_id
        kcp_object = std::unique_ptr<ikcpcb, release_kcp_wrapper>(ikcp_create(remote_id, this));
        init_rudp_output();
        // switch status
        set_status(rudp_connection_status::RUDP_CONNECTED_STATUS);
    }
    send_ack2();
}

void rudp_client_context_imp::handle_syn_ack2() {
    FDEBUG(" rudp connection {} recv syn ack2:{} from  {}->{}:{}", socket_ref->id_, recv_control_frame.command_ts,
           recv_control_frame.src_id, socket_ref->remote_endpoint.address().to_string(),
           socket_ref->remote_endpoint.port());
    if (status == rudp_connection_status::RUDP_SYN_RECV_STATUS) {
        if (status_check_command == rudp_command_t::RUDP_SYN_ACK2_COMMAND) {
            status_check_command = rudp_command_t::RUDP_UNKNOWN_COMMAND;
            // reset try_cnt
            status_check_command_try_cnt = 0;
        }
        ////use local id as kcp conv_id
        kcp_object = std::unique_ptr<ikcpcb, release_kcp_wrapper>(ikcp_create(socket_ref->id_, this));
        init_rudp_output();
        // switch status
        set_status(rudp_connection_status::RUDP_CONNECTED_STATUS);
    }
}

void rudp_client_context_imp::handle_ping() {
    update_active_time();
    send_pong();
}

void rudp_client_context_imp::handle_pong() {
    update_active_time();
    if (status_check_command == rudp_command_t::RUDP_PONG_COMMAND) {
        // reset try_cnt
        status_check_command_try_cnt = 0;
    }
}

void rudp_client_context_imp::handle_rst() {
#ifndef DEBUG_RUDP
    FDEBUG(" rudp connection {} recv rst:{} from  {}->{}:{} to {} current_status:{} remote_id:{}", socket_ref->id_,
           recv_control_frame.command_ts, recv_control_frame.src_id, socket_ref->remote_endpoint.address().to_string(),
           socket_ref->remote_endpoint.port(), recv_control_frame.dst_id, static_cast<std::int32_t>(status), remote_id);
#endif
    if (recv_control_frame.dst_id != socket_ref->id_) return;
    if (remote_id != control_frame_data::kControlMagicNum && recv_control_frame.src_id != remote_id) return;
    socket_ref->destroy(error::make_error_code(error::rudp_errors::rudp_connection_reset, "recv rst"));
}

void rudp_client_context_imp::write_data(const void* data, std::size_t len) {
    if (len == 0) return;
    std::vector<std::uint8_t> copy_data;
    copy_data.resize(len);
    std::memcpy(copy_data.data(), data, len);
    write_data(std::move(copy_data));
}

void rudp_client_context_imp::write_data(std::vector<std::uint8_t>&& buf, bool is_control_frame) {
    if (buf.empty()) return;
    asio::dispatch(socket_ref->get_executor(), [buf = std::move(buf), this, ref = socket_ref->weak_from_this(),
                                                is_control_frame]() mutable {
        auto strong = ref.lock();
        if (!strong || is_closed()) return;
        if (is_control_frame) {
            pending_control_frames.emplace_back(std::move(buf));
        } else {
            pending_data_frames.emplace_back(std::move(buf));
        }
        if (pending_control_frames.size() + pending_data_frames.size() >= rudp_kernel::kMaxRudpSocketCacheBufferNums) {
            FWARN("drop packet for kernel cache window size overflow {}", rudp_kernel::kMaxRudpSocketCacheBufferNums);
            while (pending_control_frames.size() + pending_data_frames.size() >=
                   rudp_kernel::kMaxRudpSocketCacheBufferNums) {
                if (!pending_control_frames.empty()) {
                    pending_control_frames.pop_front();
                    continue;
                }
                if (!pending_data_frames.empty()) {
                    pending_data_frames.pop_front();
                    continue;
                }
            }
        }
        flush_data();
    });
}
void rudp_client_context_imp::flush_data() {
    auto& access_list = pending_control_frames.empty() ? pending_data_frames : pending_control_frames;
    if (access_list.empty()) return;
    auto send_data = std::move(access_list.front());
    access_list.pop_front();
    asio::const_buffer buffer { send_data.data(), send_data.size() };
    socket_ref->socket.async_send(std::move(buffer),
                                  [this, ref = socket_ref->weak_from_this(),
                                   send_data = std::move(send_data)](std::error_code ec, std::size_t len) {
                                      auto strong = ref.lock();
                                      if (!strong || !is_active()) return;
                                      if (!socket_ref->socket.is_open()) return;
                                      if (ec || len != send_data.size()) {
                                          FDEBUG("{} send failed size:[{}/{}] to {}:{} for {}:{}", socket_ref->id_, len,
                                                 send_data.size(), strong->remote_endpoint.address().to_string(),
                                                 strong->remote_endpoint.port(), ec.value(), ec.message());
                                      } else {
#ifdef DEBUG_RUDP
                                          FDEBUG("{} send size:[{}/{}] to {}:{} for {}:{}", socket_ref->id_, len,
                                                 send_data.size(), strong->remote_endpoint.address().to_string(),
                                                 strong->remote_endpoint.port(), ec.value(), ec.message());
#endif
                                      }

                                      flush_data();
                                  });
}

void rudp_client_context_imp::health_check() {
    if (!is_connected()) return;
    // check active time for idle connections
    auto now = get_current_time();
    if (now > last_active_time_ms &&
        static_cast<std::size_t>(now - last_active_time_ms) > socket_ref->max_connection_idle_time) {
        socket_ref->destroy(
            error::make_error_code(error::rudp_errors::rudp_timed_out, "actively broke idle connections "));
        FWARN("disconnect rudp connection[{}:{}] for idle check {} > {} ", socket_ref->id_, remote_id,
              now - last_active_time_ms, socket_ref->max_connection_idle_time.current_value);
        return;
    }
    health_check_timer.cancel();
    health_check_timer.expires_after(std::chrono::milliseconds(socket_ref->max_connection_idle_time));
    health_check_timer.async_wait([this, ref = socket_ref->weak_from_this()](std::error_code ec) {
        if (ec) return;
        // maybe canceled
        auto strong = ref.lock();
        if (!strong) return;
        health_check();
    });
}

rudp_client_context_imp::rudp_client_context_imp(rudp_socket* parent) :
socket_ref(parent), update_timer(parent->get_executor()), health_check_timer(parent->get_executor()),
status_timer(parent->get_executor()) {
    socket_read_cache.resize(control_frame_data::kRudpControlFrameSize);
}

void rudp_client_context_imp::connect(const std::string& address,
                                      std::uint16_t port,
                                      const std::function<void(Fundamental::error_code)>& complete_func) {
    Fundamental::error_code ec;
    do {
        if (is_closed()) {
            ec = error::make_error_code(error::rudp_errors::rudp_bad_file_descriptor, "rudp client was released");
            break;
        }
        if (status == rudp_connection_status::RUDP_INIT_STATUS) { // init
            asio::ip::udp::endpoint endpoint;
            auto connect_address = asio::ip::make_address(address, ec);
            if (ec) break;
            endpoint = asio::ip::udp::endpoint(connect_address, port);
            // remote bind
            socket_ref->socket.connect(endpoint, ec);
            socket_ref->remote_endpoint = endpoint;
            if (ec) {
                break;
            }
            set_status(rudp_connection_status::RUDP_SYN_SENT_STATUS);

            connected_cb = complete_func;
            perform_read();
        } else {
            ec = error::make_error_code(error::rudp_errors::rudp_operation_in_progress);
        }
    } while (0);
    if (ec) {
        if (complete_func) complete_func(ec);
        return;
    } else {
        asio::post(socket_ref->get_executor(), [this, ref = socket_ref->weak_from_this()]() {
            auto strong = ref.lock();
            if (!strong) return;
            send_syn();
        });
    }
}

void rudp_client_context_imp::rudp_send(
    const void* buf,
    std::size_t len,
    const std::function<void(std::size_t, Fundamental::error_code)>& complete_func) {
    if (len == 0) {
        if (complete_func) complete_func(0, {});
        return;
    }
    Fundamental::error_code ec;
    do {
        if (!is_connected()) {
            ec = error::make_error_code(error::rudp_errors::rudp_not_connected);
            break;
        }
        auto ret = ikcp_send(kcp_object.get(), static_cast<const char*>(buf), static_cast<std::int32_t>(len));
        if (0 == ret) break;
        if (ret < 0) {
            // an error occurred
            ec = error::make_error_code(error::rudp_errors::rudp_failed,
                                        Fundamental::StringFormat("rudp send failed for reason {}", ret));
            break;
        }
        // cache send status
        auto& session         = pending_rudp_write_requests.emplace_back();
        session.data_len      = static_cast<std::size_t>(ret);
        session.complete_func = complete_func;
        active_request_update_rudp_status();
        return;
    } while (0);
    if (complete_func) complete_func(0, ec);
}

void rudp_client_context_imp::rudp_recv(
    void* buf,
    std::size_t len,
    const std::function<void(std::size_t, Fundamental::error_code)>& complete_func) {
    FASSERT(complete_func);
    if (len == 0) {
        complete_func(0, {});
        return;
    }
    asio::post(socket_ref->get_executor(), [this, ref = socket_ref->weak_from_this(), complete_func, buf, len]() {
        auto strong = ref.lock();
        if (!strong) return;
        Fundamental::error_code ec;
        do {
            if (!is_connected()) {
                ec = error::make_error_code(error::rudp_errors::rudp_not_connected);
                break;
            }
            // cache recv status
            auto& session         = pending_rudp_read_requests.emplace_back();
            session.buf_len       = len;
            session.buf           = buf;
            session.complete_func = complete_func;
            comsume_remote_frames();
            return;
        } while (0);
        if (ec) {
            complete_func(0, ec);
        }
    });
}

rudp_handle::rudp_handle(std::shared_ptr<rudp_socket> socket) : socket_(socket) {
}

rudp_handle::rudp_handle(rudp_handle&& other) noexcept : socket_(std::move(other.socket_)) {
}

rudp_handle& rudp_handle::operator=(rudp_handle&& other) noexcept {
    destroy();
    socket_ = std::move(other.socket_);
    return *this;
}

rudp_handle::~rudp_handle() {
    destroy();
}

std::shared_ptr<rudp_socket> rudp_handle::get() const {
    return socket_;
}

rudp_handle::operator bool() const {
    return socket_.get() && !socket_.get()->is_closed();
}

void rudp_handle::destroy() {
    if (socket_) {
        socket_->destroy(error::make_error_code(error::rudp_errors::rudp_success, "active close"));
        socket_.reset();
    }
}
} // namespace network::rudp
