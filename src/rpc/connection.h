#pragma once
#ifndef REST_RPC_CONNECTION_H_
    #define REST_RPC_CONNECTION_H_

    #include "basic/const_vars.h"
    #include "basic/router.hpp"

    #include "proxy/proxy_manager.hpp"
    #include "proxy/socks5/common.h"

    #include <any>
    #include <array>
    #include <deque>
    #include <iostream>
    #include <list>
    #include <memory>
    #include <unordered_set>

    #include "fundamental/basic/buffer.hpp"
    #include "fundamental/basic/log.h"
    #include "fundamental/events/event_system.h"
    #include "proxy/rpc_forward_connection.hpp"
using asio::ip::tcp;

namespace network
{
namespace rpc_service
{

class rpc_server;
// call these interface not in io thread
class ServerStreamReadWriter : public std::enable_shared_from_this<ServerStreamReadWriter> {
    friend class connection;

public:
    constexpr static std::size_t kDefaultReadQueueSize  = 32;
    constexpr static std::size_t kDefaultWriteQueueSize = 32;

public:
    Fundamental::Signal<void()> notify_stream_abort;
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<ServerStreamReadWriter>(std::forward<Args>(args)...);
    }
    ~ServerStreamReadWriter();
    // don't call read wite interface in any io thread
    // it will block the io thread
    template <typename T>
    bool Read(T& request, std::size_t max_wait_ms = kDefaultNetworkTimeOutMsec);
    template <typename U>
    bool Write(U&& response, std::size_t max_wait_ms = kDefaultNetworkTimeOutMsec);
    bool WriteEmpty(std::size_t max_wait_ms = kDefaultNetworkTimeOutMsec);
    bool ReadEmpty(std::size_t max_wait_ms = kDefaultNetworkTimeOutMsec);
    bool WriteDone();
    std::error_code Finish(std::size_t max_wait_ms = kDefaultNetworkTimeOutMsec);
    std::error_code GetLastError() const;
    void EnableTimeoutCheck(std::size_t timeout_msec = 15000);
    ServerStreamReadWriter(std::shared_ptr<connection> conn,
                           std::size_t read_queue_max_size  = kDefaultReadQueueSize,
                           std::size_t write_queue_max_size = kDefaultWriteQueueSize);
    void release_obj();
    std::string get_remote_peer_ip() const;
    std::uint16_t get_remote_peer_port() const;

private:
    void start() {
        EnableTimeoutCheck(2 * kDefaultPoorNetworkCheckInternalMsec);
        read_head();
    }
    void reponse_heartbeat();
    void read_head();
    void read_body(std::uint32_t offset = 0);
    void set_status(rpc_stream_data_status status, std::error_code ec);
    void handle_write();
    bool wait_write_token(std::size_t max_wait_ms) {
        if (max_wait_ms < 10) max_wait_ms = 10;
        std::unique_lock<std::mutex> locker(write_mutex);
        bool ret = write_cv_.wait_for(locker, std::chrono::milliseconds(max_wait_ms), [&]() -> bool {
            return write_token_nums > 0 || last_data_status_.load() >= rpc_stream_data_status::rpc_stream_finish;
        });
        if (!ret || last_data_status_.load() >= rpc_stream_data_status::rpc_stream_finish) return false;
        --write_token_nums;
        return true;
    }
    void reset_timer() {
        if (timeout_msec_ == 0) {
            return;
        }
        timeout_check_timer_.expires_after(std::chrono::milliseconds(timeout_msec_));
        timeout_check_timer_.async_wait([this, ptr = weak_from_this()](const asio::error_code& ec) {
            auto strong = ptr.lock();
            if (!strong) return;
            if (!reference_.is_valid()) {
                return;
            }
            if (ec) {
                return;
            }
            if (b_waiting_process_any_data) {
                set_status(rpc_stream_data_status::rpc_stream_failed,
                           error::make_error_code(error::rpc_errors::rpc_timeout));
            } else {
                b_waiting_process_any_data.exchange(true);
                reset_timer();
            }
        });
    }

    void cancel_timer() {
        if (timeout_msec_ == 0) {
            return;
        }
        try {
            timeout_check_timer_.cancel();
        } catch (...) {
        }
    }
    void check_pause_read();
    void try_resume_read();

private:
    network_data_reference reference_;
    std::error_code last_err_;
    rpc_stream_packet read_packet_buffer;
    std::atomic<rpc_stream_data_status> last_data_status_ = rpc_stream_data_status::rpc_stream_none;

    std::shared_ptr<connection> conn_;
    std::deque<std::vector<std::uint8_t>> request_cache_;
    std::deque<rpc_stream_packet> write_cache_;
    asio::steady_timer timeout_check_timer_;
    std::atomic_bool b_waiting_process_any_data = false;
    std::size_t timeout_msec_                   = 0;
    std::list<network_write_buffer_t> write_buffers_;

    std::mutex read_mutex;
    std::condition_variable read_cv_;
    std::atomic_bool is_reading_pausing = false;

    std::mutex write_mutex;
    std::condition_variable write_cv_;
    std::size_t write_token_nums            = 0;
    const std::size_t read_queue_max_size_  = kDefaultReadQueueSize;
    const std::size_t write_queue_max_size_ = kDefaultWriteQueueSize;
    std::atomic_bool is_request_finishing   = false;
};

class connection : public std::enable_shared_from_this<connection>,
                   private asio::noncopyable,
                   public proxy::proxy_upstream_interface {
    friend class rpc_server;
    friend class proxy::rpc_forward_connection;
    friend class ServerStreamReadWriter;
    static constexpr std::size_t kProbeReadSize    = 1;
    static constexpr std::size_t kMaxProbeReadSize = kRpcHeadLen;

public:
    inline static std::size_t sRpcStreamReadQueueLimitSize  = ServerStreamReadWriter::kDefaultReadQueueSize;
    inline static std::size_t sRpcStreamWriteQueueLimitSize = ServerStreamReadWriter::kDefaultWriteQueueSize;

public:
    Fundamental::Signal<void(std::string, std::weak_ptr<connection>)> on_new_subscriber_added;
    Fundamental::Signal<void(const std::unordered_set<std::string>&, std::weak_ptr<connection>)> on_subscribers_removed;
    Fundamental::Signal<void(std::shared_ptr<connection>, std::string)> on_net_err_;
    Fundamental::Signal<void(std::string /*key*/, std::string /*data*/)> on_publish_msg;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<connection>(std::forward<Args>(args)...);
    }
    connection(tcp::socket socket, std::size_t timeout_msec, router& router, std::weak_ptr<rpc_server> server_wref) :
    server_wref_(server_wref), socket_(std::move(socket)), executor_(socket_.get_executor()), body_(INIT_BUF_SIZE),
    timeout_check_timer_(executor_), timeout_msec_(timeout_msec), router_(router) {
        enable_tcp_keep_alive(socket_);
    }
    ~connection() {
    #ifdef RPC_VERBOSE
        FDEBUG("release connection {:p} -> {}", (void*)this, conn_id_);
    #endif
    }

    void start() {
        remote_ip   = socket_.remote_endpoint().address().to_string();
        remote_port = socket_.remote_endpoint().port();
        if (is_ssl()) {
            ssl_handshake();
        } else {
            probe_protocal(0);
        }
    }

    tcp::socket& socket() {
        return socket_;
    }

    uint64_t request_id() const {
        return req_id_;
    }
    template <typename... Args>
    void response(uint64_t req_id, request_type req_type, Args&&... args) {
        auto data   = msgpack_codec::pack(static_cast<int32_t>(result_code::OK), std::forward<Args>(args)...);
        auto s_data = std::string(data.data(), data.data() + data.size());
        response_interal(req_id, std::move(s_data), req_type);
    }

    template <typename... Args>
    void response_errmsg(uint64_t req_id, request_type req_type, Args&&... args) {
        auto data   = msgpack_codec::pack(static_cast<int32_t>(result_code::FAIL), std::forward<Args>(args)...);
        auto s_data = std::string(data.data(), data.data() + data.size());
        response_interal(req_id, std::move(s_data), req_type);
    }

    void set_conn_id(int64_t id) {
        conn_id_ = id;
    }

    int64_t conn_id() const {
        return conn_id_;
    }

    const std::vector<char>& body() const {
        return body_;
    }

    std::string remote_address() const {
        if (!reference_.is_valid()) {
            return "";
        }

        asio::error_code ec;
        auto endpoint = socket_.remote_endpoint(ec);
        if (ec) {
            return "";
        }
        return endpoint.address().to_string();
    }

    void set_delay(bool delay) {
        delay_ = delay;
    }

    #ifndef NETWORK_DISABLE_SSL
    void enable_ssl(asio::ssl::context& ssl_context, bool enable_no_ssl) {
        ssl_context_ref = &ssl_context;
        enable_no_ssl_  = enable_no_ssl;
    }
    #endif
    void release_obj() override {
        reference_.release();
        asio::post(executor_, [this, ref = shared_from_this()] { close(); });
    }
    std::shared_ptr<ServerStreamReadWriter> InitRpcStream();

    void config_proxy_manager(network::proxy::ProxyManager* manager) {
        proxy_manager_ = manager;
    }
    void config_socks5_handler(std::shared_ptr<SocksV5::Sock5Handler> handler) {
        socks5_handler = handler;
    }
    void set_external_config(rpc_server_external_config config) {
        external_config = config;
    }
    void set_tcp_no_delay(bool flag = true) {
        try {
            socket_.set_option(asio::ip::tcp::no_delay(flag));
        } catch (...) {
        }
    }
    bool has_closed() const {
        return !reference_.is_valid();
    }
    auto get_remote_peer_ip() const {
        return remote_ip;
    }
    auto get_remote_peer_port() const {
        return remote_port;
    }

private:
    void publish(const std::string& key, const std::string& data) {
        response(0, request_type::rpc_publish, key, data);
    }

    void do_ssl_handshake(const char* preread_data, std::size_t read_len) {
        // handle ssl
    #ifndef NETWORK_DISABLE_SSL
        auto self   = shared_from_this();
        ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket_, *ssl_context_ref);

        ssl_stream_->async_handshake(asio::ssl::stream_base::server, network_write_buffer_t(preread_data, read_len),
                                     [this, self](const asio::error_code& error, std::size_t) {
                                         if (!reference_.is_valid()) {
                                             return;
                                         }
                                         if (error) {
                                             FDEBUG("perform ssl handshake failed {}", error.message());
                                             on_net_err_(self, error.message());
                                             release_obj();
                                             return;
                                         }

                                         probe_protocal(0);
                                     });
    #endif
    }
    void ssl_handshake();

    bool is_ssl() const {
    #ifndef NETWORK_DISABLE_SSL
        return ssl_context_ref != nullptr;
    #else
        return false;
    #endif
    }
    void process_rpc_request();
    void process_transparent_proxy(std::size_t preread_len = 0);
    void process_ws_request(std::size_t preread_len = 0);
    void process_pipe_connection(std::size_t preread_len = 0);
    void process_socks5_proxy(const void* preread_data, std::size_t len);
    void probe_protocal(std::size_t offset = 0, std::size_t target_probe_size = kProbeReadSize);
    void read_rpc_head(std::size_t offset = 0);

    void read_body(uint32_t func_id, std::size_t size, std::size_t start_offset = 0);
    void handle_none_param_request(uint32_t func_id) {
        read_rpc_head();
        process_request(func_id, nullptr, 0);
    }
    void process_request(uint32_t func_id, const char* data, std::size_t size) {
        route_result_t ret = router_.route<connection>(func_id, std::string_view { data, size }, shared_from_this());
        if (delay_) {
            delay_ = false;
        } else {
            response_interal(req_id_, std::move(ret.result));
        }
    }

    void process_rpc_stream(uint32_t func_id) {
        cancel_timer();
        // allocate rpc stream
        rpc_stream_rw_ = ServerStreamReadWriter::make_shared(shared_from_this(), sRpcStreamReadQueueLimitSize,
                                                             sRpcStreamWriteQueueLimitSize);

        route_result_t ret = router_.route<connection>(func_id, std::string_view { nullptr, 0 }, shared_from_this());
        // force clear rpc stream cache
        rpc_stream_rw_ = nullptr;
        response_interal(req_id_, std::move(ret.result), request_type::rpc_stream);
    }

    void response_interal(uint64_t req_id, std::string data, request_type req_type = request_type::rpc_res) {

        assert(data.size() < MAX_BUF_LEN);
        asio::post(executor_, [this, data = std::move(data), req_id, req_type, ref = shared_from_this()]() mutable {
            if (!reference_.is_valid()) {
                return;
            }
            write_queue_.emplace_back(message_type { req_id, req_type, std::move(data) });
            if (write_queue_.size() > 1) {
                return;
            }
            write();
        });
    }

    void write();

    void async_buffers_read(network_read_buffers_t buffers, network_io_handler_t handler) override {
        if (is_ssl()) {
    #ifndef NETWORK_DISABLE_SSL
            asio::async_read(*ssl_stream_, std::move(buffers), std::move(handler));
    #endif
        } else {
            asio::async_read(socket_, std::move(buffers), std::move(handler));
        }
    }

    void async_buffers_read_some(network_read_buffers_t buffers, network_io_handler_t handler) override {
        if (is_ssl()) {
    #ifndef NETWORK_DISABLE_SSL
            ssl_stream_->async_read_some(std::move(buffers), std::move(handler));
    #endif
        } else {
            socket_.async_read_some(std::move(buffers), std::move(handler));
        }
    }

    void async_buffers_write(network_write_buffers_t buffers, network_io_handler_t handler) override {
        if (is_ssl()) {
    #ifndef NETWORK_DISABLE_SSL
            asio::async_write(*ssl_stream_, std::move(buffers), std::move(handler));
    #endif
        } else {
            asio::async_write(socket_, std::move(buffers), std::move(handler));
        }
    }
    void async_buffers_write_some(network_write_buffers_t buffers, network_io_handler_t handler) override {
        if (is_ssl()) {
    #ifndef NETWORK_DISABLE_SSL
            ssl_stream_->async_write_some(std::move(buffers), std::move(handler));
    #endif
        } else {
            socket_.async_write_some(std::move(buffers), std::move(handler));
        }
    }
    const asio::any_io_executor& get_current_executor() override {
        return executor_;
    }
    void reset_timer() {
        if (timeout_msec_ == 0) {
            return;
        }

        timeout_check_timer_.expires_after(std::chrono::milliseconds(timeout_msec_));
        timeout_check_timer_.async_wait([this, self = weak_from_this()](const asio::error_code& ec) {
            auto strong = self.lock();
            if (!strong) return;
            if (!reference_.is_valid()) {
                return;
            }

            if (ec) {
                return;
            }
            if (b_waiting_process_any_data) {
                release_obj();
            } else {
                b_waiting_process_any_data.exchange(true);
                reset_timer();
            }
        });
    }

    void cancel_timer() {
        if (timeout_msec_ == 0) {
            return;
        }
        try {
            timeout_check_timer_.cancel();
        } catch (...) {
        }
    }

    void close() {
        {
            std::unordered_set<std::string> tmp;
            std::swap(subscribers_, tmp);
            on_subscribers_removed(tmp, weak_from_this());
        }
        cancel_timer();
    #ifndef NETWORK_DISABLE_SSL
        if (ssl_stream_) {
            asio::dispatch(ssl_stream_->get_executor(), [this, ref = shared_from_this()]() {
                asio::error_code ignored_ec;
                ssl_stream_->lowest_layer().close(ignored_ec);
            });
            return;
        }
    #endif

        asio::error_code ignored_ec;
        socket_.shutdown(tcp::socket::shutdown_both, ignored_ec);
        socket_.close(ignored_ec);
    }
    network_data_reference reference_;
    std::weak_ptr<rpc_server> server_wref_;
    tcp::socket socket_;
    const asio::any_io_executor& executor_;
    char head_[kRpcHeadLen];
    std::vector<char> body_;
    std::uint64_t req_id_;
    request_type req_type_;

    std::uint8_t write_head_buffer[kRpcHeadLen];

    uint32_t write_size_ = 0;

    std::atomic_bool b_waiting_process_any_data = false;
    asio::steady_timer timeout_check_timer_;
    std::size_t timeout_msec_;
    int64_t conn_id_ = 0;
    std::unordered_set<std::string> subscribers_;

    std::deque<message_type> write_queue_;
    std::list<network_write_buffer_t> write_buffers_;
    std::shared_ptr<ServerStreamReadWriter> rpc_stream_rw_;

    router& router_;
    bool delay_ = false;
    #ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_ = nullptr;
    asio::ssl::context* ssl_context_ref                                    = nullptr;
    bool enable_no_ssl_                                                    = true;
    #endif
    // proxy
    network::proxy::ProxyManager* proxy_manager_ = nullptr;
    rpc_server_external_config external_config;
    std::shared_ptr<SocksV5::Sock5Handler> socks5_handler;
    // remote endpoint
    std::string remote_ip;
    std::uint16_t remote_port = 0;
};

inline ServerStreamReadWriter::ServerStreamReadWriter(std::shared_ptr<connection> conn,
                                                      std::size_t read_queue_max_size,
                                                      std::size_t write_queue_max_size) :
conn_(conn), timeout_check_timer_(conn_->executor_), read_queue_max_size_(read_queue_max_size),
write_queue_max_size_(write_queue_max_size) {
    #ifdef RPC_VERBOSE
    FDEBUG("build stream writer {:p} with connection:{:p}", (void*)this, (void*)conn_.get());
    #endif
    write_token_nums = write_queue_max_size_;
}
inline void ServerStreamReadWriter::release_obj() {
    reference_.release();
    asio::post(conn_->executor_, [this, ref = shared_from_this()] {
        cancel_timer();
        if (last_data_status_ < rpc_stream_data_status::rpc_stream_finish) {
            set_status(rpc_stream_data_status::rpc_stream_finish,
                       error::make_error_code(error::rpc_errors::rpc_internal_error));
        }
        conn_->release_obj();
    });
}
inline std::string ServerStreamReadWriter::get_remote_peer_ip() const {
    return conn_->get_remote_peer_ip();
}
inline std::uint16_t ServerStreamReadWriter::get_remote_peer_port() const {
    return conn_->get_remote_peer_port();
}
inline ServerStreamReadWriter::~ServerStreamReadWriter() {
    #ifdef RPC_VERBOSE
    FDEBUG("release stream writer {:p} with connection:{:p}", (void*)this, (void*)conn_.get());
    #endif
}
template <typename T>
inline bool ServerStreamReadWriter::Read(T& request, std::size_t max_wait_ms) {
    auto check_func = [this]() -> bool {
        return last_data_status_ >= rpc_stream_data_status::rpc_stream_write_done || !request_cache_.empty();
    };
    std::vector<std::uint8_t> request_data;
    {
        std::unique_lock<std::mutex> locker(read_mutex);
        if (max_wait_ms > 0)
            read_cv_.wait_for(locker, std::chrono::milliseconds(max_wait_ms), check_func);
        else {
            read_cv_.wait(locker, check_func);
        }
        if (request_cache_.empty()) return false;
        request_data = std::move(request_cache_.front());
        request_cache_.pop_front();
        if (last_data_status_ < rpc_stream_data_status::rpc_stream_finish) {
            try_resume_read();
        }
    }
    try {
        request = msgpack_codec::unpack<T>(request_data.data(), request_data.size());
    } catch (const std::exception& e) {
        set_status(rpc_stream_data_status::rpc_stream_failed,
                   error::make_error_code(error::rpc_errors::rpc_unpack_failed));

        return false;
    }

    return true;
}
template <typename U>
inline bool ServerStreamReadWriter::Write(U&& response, std::size_t max_wait_ms) {
    rpc_buffer_type data;
    try {
        data = msgpack_codec::pack(std::forward<U>(response));
    } catch (const std::exception& e) {
        set_status(rpc_stream_data_status::rpc_stream_failed,
                   error::make_error_code(error::rpc_errors::rpc_pack_failed));

        return false;
    }
    if (!wait_write_token(max_wait_ms)) return false;
    asio::post(conn_->executor_, [this, data = std::move(data), ref = shared_from_this()]() mutable {
        if (!reference_.is_valid()) return;
        auto& new_item = write_cache_.emplace_back();
        new_item.size  = Fundamental::host_value_convert(static_cast<std::uint32_t>(data.size()));
        new_item.type  = static_cast<std::uint8_t>(rpc_stream_data_status::rpc_stream_data);
        new_item.data  = std::move(data);
        if (write_cache_.size() == 1) handle_write();
    });
    return true;
}

inline bool ServerStreamReadWriter::WriteEmpty(std::size_t max_wait_ms) {
    if (!wait_write_token(max_wait_ms)) return false;
    rpc_buffer_type data;

    asio::post(conn_->executor_, [this, ref = shared_from_this()]() mutable {
        if (!reference_.is_valid()) return;
        auto& new_item = write_cache_.emplace_back();
        new_item.size  = 0;
        new_item.type  = static_cast<std::uint8_t>(rpc_stream_data_status::rpc_stream_data);
        new_item.data.resize(0);
        if (write_cache_.size() == 1) handle_write();
    });
    return true;
}

inline bool ServerStreamReadWriter::ReadEmpty(std::size_t max_wait_ms) {
    auto check_func = [this]() -> bool {
        return last_data_status_ >= rpc_stream_data_status::rpc_stream_write_done || !request_cache_.empty();
    };
    {
        std::unique_lock<std::mutex> locker(read_mutex);
        if (max_wait_ms > 0)
            read_cv_.wait_for(locker, std::chrono::milliseconds(max_wait_ms), check_func);
        else {
            read_cv_.wait(locker, check_func);
        }
        if (request_cache_.empty()) return false;
        request_cache_.pop_front();
        if (last_data_status_ < rpc_stream_data_status::rpc_stream_finish) {
            try_resume_read();
        }
    }
    return true;
}

inline bool ServerStreamReadWriter::WriteDone() {
    if (last_data_status_ >= rpc_stream_data_status::rpc_stream_finish) return false;
    asio::post(conn_->executor_, [this, ref = shared_from_this()]() mutable {
        if (!reference_.is_valid()) return;
        auto& new_item = write_cache_.emplace_back();
        new_item.size  = 0;
        new_item.type  = static_cast<std::uint8_t>(rpc_stream_data_status::rpc_stream_write_done);
        new_item.data.clear();
        if (write_cache_.size() == 1) handle_write();
    });
    return true;
}

inline std::error_code ServerStreamReadWriter::Finish(std::size_t max_wait_ms) {
    {
        auto expected_value = false;
        if (!is_request_finishing.compare_exchange_strong(expected_value, true)) {
            FASSERT(false, "call ClientStreamReadWriter twice");
            return last_err_;
        }
    }
    auto check_func = [this]() -> bool { return last_data_status_ >= rpc_stream_data_status::rpc_stream_finish; };
    try_resume_read();
    std::unique_lock<std::mutex> locker(read_mutex);
    if (max_wait_ms > 0)
        read_cv_.wait_for(locker, std::chrono::milliseconds(max_wait_ms), check_func);
    else {
        read_cv_.wait(locker, check_func);
    }
    return last_err_;
}

inline std::error_code ServerStreamReadWriter::GetLastError() const {
    return last_err_;
}
inline void ServerStreamReadWriter::EnableTimeoutCheck(std::size_t timeout_msec) {
    timeout_msec_ = timeout_msec;
    timeout_check_timer_.cancel();
    reset_timer();
}
inline void ServerStreamReadWriter::reponse_heartbeat() {
    auto& new_item = write_cache_.emplace_back();
    new_item.size  = 0;
    new_item.type  = static_cast<std::uint8_t>(rpc_stream_data_status::rpc_stream_heartbeat);
    new_item.data.clear();
    if (write_cache_.size() == 1) handle_write();
}
inline void ServerStreamReadWriter::read_head() {
    if (is_reading_pausing) return;
    std::vector<network_read_buffer_t> buffers;
    buffers.emplace_back(&read_packet_buffer.size, sizeof(read_packet_buffer.size));
    buffers.emplace_back(network_read_buffer_t(&read_packet_buffer.type, 1));

    conn_->async_buffers_read(std::move(buffers), [this, ptr = shared_from_this()](asio::error_code ec,
                                                                                   std::size_t length) {
        if (!reference_.is_valid()) {
            return;
        }
        if (last_data_status_ >= rpc_stream_data_status::rpc_stream_finish) return;
        if (ec) {
            set_status(rpc_stream_data_status::rpc_stream_failed, std::move(ec));
            notify_stream_abort.Emit();
        } else {
    #if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FINFO("server {:p} stream read head size data:{} type:{}", (void*)this,
                  Fundamental::Utils::BufferToHex(&read_packet_buffer.size, sizeof(read_packet_buffer.size)),
                  static_cast<std::uint32_t>(read_packet_buffer.type));
    #endif
            b_waiting_process_any_data.exchange(false);

            try {
                auto status = static_cast<rpc_stream_data_status>(read_packet_buffer.type);
                if ((status != rpc_stream_data_status::rpc_stream_heartbeat && status < last_data_status_) ||
                    status >= rpc_stream_data_status::rpc_stream_status_max) {
                    FDEBUG("server {:p} rpc_bad_request status:{} last_data_status:{}", (void*)this,
                           static_cast<std::uint32_t>(status), static_cast<std::uint32_t>(last_data_status_.load()));
                    set_status(rpc_stream_data_status::rpc_stream_failed,
                               error::make_error_code(error::rpc_errors::rpc_bad_request));
                    return;
                }
                switch (status) {
                case rpc_stream_data_status::rpc_stream_data: {
                    {
                        std::scoped_lock<std::mutex> locker(read_mutex);
                        last_data_status_ = status;
                    }
                    read_packet_buffer.size = Fundamental::host_value_convert(read_packet_buffer.size);
                    if (read_packet_buffer.size > read_packet_buffer.data.size())
                        read_packet_buffer.data.resize(read_packet_buffer.size);
                    read_body();

                } break;
                case rpc_stream_data_status::rpc_stream_heartbeat: {

                    reponse_heartbeat();
                    read_head();
                } break;
                case rpc_stream_data_status::rpc_stream_write_done: {
                    read_head();
                    set_status(status, error::make_error_code(error::rpc_errors::rpc_success));
                } break;
                case rpc_stream_data_status::rpc_stream_finish: {
                    if (last_data_status_ != rpc_stream_data_status::rpc_stream_write_done) {
                        set_status(rpc_stream_data_status::rpc_stream_failed,
                                   error::make_error_code(error::rpc_errors::rpc_broken_pipe));
                        return;
                    }
                    set_status(status, error::make_error_code(error::rpc_errors::rpc_success));
                } break;
                default: {
                    FDEBUG("server {:p} rpc_bad_request   unsupported status:{} last_data_status:{}", (void*)this,
                           static_cast<std::uint32_t>(status), static_cast<std::uint32_t>(last_data_status_.load()));
                    set_status(rpc_stream_data_status::rpc_stream_failed,
                               error::make_error_code(error::rpc_errors::rpc_bad_request));
                    break;
                }
                }

            } catch (...) {
                set_status(rpc_stream_data_status::rpc_stream_failed,
                           error::make_error_code(error::rpc_errors::rpc_memory_error));
            }
        }
    });
}
inline void ServerStreamReadWriter::read_body(std::uint32_t offset) {
    std::vector<network_read_buffer_t> buffers;
    buffers.emplace_back(
        network_read_buffer_t(read_packet_buffer.data.data() + offset, read_packet_buffer.size - offset));
    conn_->async_buffers_read_some(
        std::move(buffers), [this, offset, ptr = shared_from_this()](asio::error_code ec, std::size_t length) {
            if (!reference_.is_valid()) {
                return;
            }
            if (last_data_status_ >= rpc_stream_data_status::rpc_stream_finish) return;
            if (ec) {
                set_status(rpc_stream_data_status::rpc_stream_failed, std::move(ec));
                notify_stream_abort.Emit();
            } else {
                b_waiting_process_any_data.exchange(false);
                auto current_read_offset = offset + length;
    #if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                FINFO("server {:p} stream read some need:{}  current: {} new:{}", (void*)this, read_packet_buffer.size,
                      current_read_offset, length);
    #endif
                if (current_read_offset < read_packet_buffer.size) {

                    read_body(static_cast<std::uint32_t>(current_read_offset));
                    return;
                }
    #if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                FINFO("server {:p} stream read {}", (void*)this,
                      Fundamental::Utils::BufferToHex(read_packet_buffer.data.data(),
                                                      read_packet_buffer.size > 32 ? 32 : read_packet_buffer.size, 16));
    #endif
                check_pause_read();
                read_head();
                // discard all request frame when finish was called
                if (is_request_finishing) return;
                std::scoped_lock<std::mutex> locker(read_mutex);
                request_cache_.emplace_back(std::move(read_packet_buffer.data));
                read_cv_.notify_one();
            }
        });
}
inline void ServerStreamReadWriter::set_status(rpc_stream_data_status status, std::error_code ec) {
    std::scoped_lock<std::mutex, std::mutex> locker(read_mutex, write_mutex);
    Fundamental::ScopeGuard notify_g([&]() {
        read_cv_.notify_all();
        write_cv_.notify_all();
    });
    if (last_data_status_.load() >= rpc_stream_data_status::rpc_stream_finish) {

        return;
    }
    last_err_         = std::move(ec);
    last_data_status_ = status;

    if (last_data_status_.load() >= rpc_stream_data_status::rpc_stream_finish) {
        FDEBUG("rpc {:p} stream connection finish success:{} {}", (void*)this,
               last_data_status_.load() == rpc_stream_data_status::rpc_stream_finish, last_err_.message());
        release_obj();
    }
}
inline void ServerStreamReadWriter::handle_write() {
    if (last_data_status_.load() >= rpc_stream_data_status::rpc_stream_finish) {
        write_buffers_.clear();
        write_cache_.clear();
    }

    if (write_cache_.empty()) {
        return;
    }
    if (write_buffers_.empty()) {
        auto& item = write_cache_.front();
        write_buffers_.emplace_back(network_write_buffer_t(&item.size, sizeof(item.size)));
        write_buffers_.emplace_back(network_write_buffer_t(&item.type, sizeof(item.type)));
        if (item.data.size() > 0) {
            write_buffers_.emplace_back(network_write_buffer_t(item.data.data(), item.data.size()));
        }
    }

    conn_->async_buffers_write_some(std::vector<network_write_buffer_t>(write_buffers_.begin(), write_buffers_.end()),
                                    [this, ptr = shared_from_this()](asio::error_code ec, std::size_t length) {
                                        if (!reference_.is_valid()) {
                                            return;
                                        }
                                        if (last_data_status_ >= rpc_stream_data_status::rpc_stream_finish) return;
                                        if (ec) {
                                            set_status(rpc_stream_data_status::rpc_stream_failed, ec);
                                            notify_stream_abort.Emit();
                                            return;
                                        }
                                        b_waiting_process_any_data.exchange(false);

    #if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                                        FINFO("server {:p} stream write some size:{}", (void*)this, length);
    #endif
                                        while (length != 0) {
                                            if (write_buffers_.empty()) break;
                                            auto current_size = write_buffers_.front().size();
                                            if (length >= current_size) {
                                                length -= current_size;
                                                write_buffers_.pop_front();
                                                continue;
                                            }
                                            write_buffers_.front() = network_write_buffer_t(
                                                (std::uint8_t*)write_buffers_.front().data() + length,
                                                current_size - length);
                                            break;
                                        }
                                        if (!write_buffers_.empty()) { // write rest data

                                            handle_write();
                                            return;
                                        }
                                        Fundamental::ScopeGuard writable_notify_g([&]() {
                                            {
                                                std::scoped_lock<std::mutex> locker(write_mutex);
                                                if (write_cache_.size() < write_queue_max_size_) {
                                                    write_token_nums = write_queue_max_size_ - write_cache_.size();
                                                } else {
                                                    write_token_nums = 0;
                                                }
                                                if (write_token_nums == 1)
                                                    write_cv_.notify_one();
                                                else {
                                                    write_cv_.notify_all();
                                                }
                                            }
                                        });
                                        write_cache_.pop_front();
                                        handle_write();
                                    });
}

inline void ServerStreamReadWriter::check_pause_read() {
    if (is_request_finishing) return;
    std::scoped_lock<std::mutex> locker(read_mutex);
    if (request_cache_.size() >= read_queue_max_size_) {
    #if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
        FERR("server stream {:p} pause read", (void*)this);
    #endif
        is_reading_pausing.exchange(true);
    }
}

inline void ServerStreamReadWriter::try_resume_read() {
    auto expected_value = true;
    if (is_reading_pausing.compare_exchange_strong(expected_value, false)) {
        asio::post(conn_->executor_, [this, ref = shared_from_this()]() mutable {
            if (!reference_.is_valid()) return;
    #if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FERR("server stream {:p} resume read", (void*)this);
    #endif
            read_head();
        });
    }
}
} // namespace rpc_service
} // namespace network

#endif // REST_RPC_CONNECTION_H_
