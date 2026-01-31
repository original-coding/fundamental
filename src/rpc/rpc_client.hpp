#pragma once
#include "basic/client_util.hpp"
#include "basic/md5.hpp"
#include "basic/meta_util.hpp"
#include "network/network.hpp"
#include "network/upgrade_interface.hpp"

#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "fundamental/basic/buffer.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/events/event_system.h"
namespace network
{
namespace rpc_service
{
class req_result {
public:
    req_result() = default;
    req_result(string_view data) : data_(data.data(), data.length()) {
    }
    bool success() const {
        return !has_error(data_);
    }

    template <typename T>
    T as() {
        if (has_error(data_)) {
            std::string err_msg = data_.empty() ? data_ : get_error_msg(data_);
            throw std::logic_error(err_msg);
        }

        return get_result<T>(data_);
    }

    void as() {
        if (has_error(data_)) {
            std::string err_msg = data_.empty() ? data_ : get_error_msg(data_);
            throw std::logic_error(err_msg);
        }
    }

private:
    std::string data_;
};

struct rpc_request_context {
    rpc_request_context(asio::io_context& ios) : timeout_check_timer(ios) {
    }
    ~rpc_request_context() {
    }
    void finish(const asio::error_code& ec, std::string_view data) {
        bool expected_value = false;
        if (!has_set_value.compare_exchange_strong(expected_value, true)) return;
        try {
            timeout_check_timer.cancel();
        } catch (...) {
        }

        notify_finish.Emit(ec, data);
        if (ec) {
            finish_promise.set_exception(std::make_exception_ptr(std::system_error(ec)));
        } else {
            finish_promise.set_value(req_result { data });
        }
    }
    std::atomic_bool has_set_value = false;
    asio::steady_timer timeout_check_timer;
    std::uint64_t call_id = 0;
    std::promise<req_result> finish_promise;
    Fundamental::Signal<void(const asio::error_code& /*ec*/, std::string_view /*data*/)> notify_finish;
};

template <typename T>
struct result_context {
    uint64_t id;
    std::future<T> future;
    std::weak_ptr<rpc_request_context> context_ref;
    std::function<void()> post_request_func;
    ~result_context() {
        guard_post_request();
    }
    void guard_post_request() {
        if (post_request_func) {
            post_request_func();
            post_request_func = nullptr;
        }
    }

    template <class Rep, class Per>
    std::future_status wait_for(const std::chrono::duration<Rep, Per>& rel_time) {
        return future.wait_for(rel_time);
    }

    T get() {
        return future.get();
    }

    template <typename ResponseType>
    result_context& async_response(const std::function<void(Fundamental::error_code, ResponseType)>& handler) {
        if (handler) {
            auto context = context_ref.lock();
            if (context) {
                context->notify_finish.Connect([handler](const asio::error_code& ec, std::string_view data) {
                    Fundamental::error_code ret_ec = ec;
                    ResponseType response;
                    if (!ret_ec) {
                        try {
                            if (has_error(data)) {
                                std::string err_msg = data.empty() ? "[unknown error]" : get_error_msg(data);
                                ret_ec              = asio::error::make_error_code(asio::error::invalid_argument);
                                ret_ec.assign_details(Fundamental::StringFormat("rpc internal error:{}", err_msg));
                            } else {
                                response = get_result<ResponseType>(data);
                            }
                        } catch (...) {
                            ret_ec = asio::error::make_error_code(asio::error::invalid_argument);
                            ret_ec.assign_details(Fundamental::StringFormat("parse rpc data failed"));
                        }
                    }
                    handler(std::move(ret_ec), std::move(response));
                });
            } else {
                throw std::invalid_argument("invalid rpc request context");
            }
        }
        return *this;
    }
    result_context& async_response(const std::function<void(Fundamental::error_code)>& handler) {
        if (handler) {
            auto context = context_ref.lock();
            if (context) {
                context->notify_finish.Connect([handler](const asio::error_code& ec, std::string_view data) {
                    Fundamental::error_code ret_ec = ec;
                    if (!ret_ec) {
                        try {
                            if (has_error(data)) {
                                std::string err_msg = data.empty() ? "[unknown error]" : get_error_msg(data);
                                ret_ec              = asio::error::make_error_code(asio::error::invalid_argument);
                                ret_ec.assign_details(Fundamental::StringFormat("rpc internal error:{}", err_msg));
                            }
                        } catch (...) {
                            ret_ec = asio::error::make_error_code(asio::error::invalid_argument);
                            ret_ec.assign_details(Fundamental::StringFormat("parse rpc data failed"));
                        }
                    }
                    handler(std::move(ret_ec));
                });
            } else {
                throw std::invalid_argument("invalid rpc request context");
            }
        }
        return *this;
    }
};

enum rpc_client_ssl_level
{
    rpc_client_ssl_level_none,
    rpc_client_ssl_level_optional,
    rpc_client_ssl_level_required
};
class rpc_client;
// call these interface not in io thread
class ClientStreamReadWriter : public std::enable_shared_from_this<ClientStreamReadWriter> {
    friend class rpc_client;

public:
    constexpr static std::size_t kDefaultReadQueueSize  = 64;
    constexpr static std::size_t kDefaultWriteQueueSize = 64;

public:
    Fundamental::Signal<void()> notify_stream_abort;
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<ClientStreamReadWriter>(std::forward<Args>(args)...);
    }
    ClientStreamReadWriter(std::shared_ptr<rpc_client> client,
                           std::size_t read_queue_max_size  = kDefaultReadQueueSize,
                           std::size_t write_queue_max_size = kDefaultWriteQueueSize);
    ~ClientStreamReadWriter();
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
    void EnableAutoHeartBeat(bool enable, std::size_t timeout_msec = 15000);
    void release_obj();
    std::string get_remote_peer_ip() const;
    std::uint16_t get_remote_peer_port() const;

private:
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
    void start() {
        read_head();
        EnableAutoHeartBeat(true, kDefaultPoorNetworkCheckInternalMsec);
    }
    void send_heartbeat() {
        auto& new_item = write_cache_.emplace_back();
        new_item.size  = 0;
        new_item.type  = static_cast<std::uint8_t>(rpc_stream_data_status::rpc_stream_heartbeat);
        new_item.data.clear();
        if (write_cache_.size() == 1) handle_write();
    }
    void reset_timer() {
        if (timeout_msec_ == 0) return;
        deadline_.expires_after(std::chrono::milliseconds(timeout_msec_));
        deadline_.async_wait([this, ptr = weak_from_this()](const asio::error_code& ec) {
            auto strong = ptr.lock();
            if (!strong) return;
            if (!reference_.is_valid()) {
                return;
            }
            if (ec) return;
            if (last_data_status_.load() >= rpc_stream_data_status::rpc_stream_finish) {
                return;
            }
            if (b_wait_any_data) {
                b_wait_any_data.exchange(false);
                FWARN("disconnect rpc stream for timeout {} msec,we has not recv/send any data", timeout_msec_);
                set_status(rpc_stream_data_status::rpc_stream_failed,
                           error::make_error_code(error::rpc_errors::rpc_timeout));
                return;
            }
            b_wait_any_data.exchange(true);
            send_heartbeat();

            reset_timer();
        });
    }
    void read_head();
    void read_body(std::uint32_t offset = 0);
    void set_status(rpc_stream_data_status status, std::error_code ec);
    void handle_write();
    void check_pause_read();
    void try_resume_read();

private:
    network_data_reference reference_;

    std::error_code last_err_;
    rpc_stream_packet read_packet_buffer;
    std::atomic<rpc_stream_data_status> last_data_status_ = rpc_stream_data_status::rpc_stream_none;

    std::shared_ptr<rpc_client> client_;

    std::deque<std::vector<std::uint8_t>> response_cache_;
    std::deque<rpc_stream_packet> write_cache_;
    std::list<network_write_buffer_t> write_buffers_;
    std::atomic_bool b_wait_any_data = false;
    asio::steady_timer deadline_;
    std::size_t timeout_msec_ = 0;
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

class rpc_client : private asio::noncopyable, public std::enable_shared_from_this<rpc_client> {
    friend class ClientStreamReadWriter;
    enum rpc_connection_status : std::size_t
    {
        rpc_connection_wait_connecting,
        rpc_connection_dns_resolving,
        rpc_connection_tcp_handshaking,
        rpc_connection_proxy_handshaking,
        rpc_connection_ssl_handshaking,
        rpc_connection_closing,
        rpc_connection_ready,
        rpc_connection_closed,
    };

public:
    inline static std::function<asio::io_context&()> s_io_context_cb = []() -> decltype(auto) {
        return network::io_context_pool::Instance().get_io_context();
    };

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<rpc_client>(std::forward<Args>(args)...);
    }
    rpc_client() : rpc_client("", "") {
    }

    rpc_client(std::string host, const std::string& service) :
    ios_(s_io_context_cb()), resolver_(ios_), socket_(ios_), executor_(socket_.get_executor()), host_(std::move(host)),
    service_name_(service), reconnect_delay_timer_(ios_), deadline_(ios_), body_(INIT_BUF_SIZE) {
#ifdef RPC_VERBOSE
        FDEBUG(" client construct {:p}", (void*)this);
#endif
    }
    ~rpc_client() {
#ifdef RPC_VERBOSE
        FDEBUG(" client deconstruct {:p}", (void*)this);
#endif
        for (auto& i : proxy_interfaces)
            if (i) i->abort_all_operation();
        proxy_interfaces.clear();
    }
    void config_tcp_no_delay(bool flag = true) {
        tcp_no_delay_flag = flag;
    }
    void set_reconnect_delay(size_t milliseconds) {
        reconnect_delay_ms = milliseconds;
    }

    void set_reconnect_count(int reconnect_count) {
        reconnect_cnt_ = reconnect_count;
    }

    bool connect(size_t timeout_msec = kDefaultNetworkTimeOutMsec) {
        if (has_connected()) return true;
        FASSERT(!service_name_.empty());
        do_async_connect();
        return wait_conn(timeout_msec);
    }

    bool connect(const std::string& host,
                 const std::string& service,
                 size_t timeout_msec = kDefaultNetworkTimeOutMsec) {
        if (service_name_.empty()) {
            host_         = host;
            service_name_ = service;
        }

        return connect(timeout_msec);
    }

    void async_connect(const std::string& host, const std::string& service) {
        if (service_name_.empty()) {
            host_         = host;
            service_name_ = service;
        }

        do_async_connect();
    }

    bool wait_conn(size_t timeout_msec) {
        {
            std::unique_lock<std::mutex> locker(status_mutex_);
            status_cv.wait_for(locker, std::chrono::milliseconds(timeout_msec),
                               [&]() { return has_connected() || has_closed(); });
        }
        return has_connected();
    }

    void enable_auto_reconnect(bool enable = true) {
        enable_reconnect_ = enable;
        reconnect_cnt_    = std::numeric_limits<int>::max();
    }

    void enable_timeout_check(bool enable = true, std::size_t timeout_msec = 30000) {
        if (enable) {
            reset_deadline_timer(timeout_msec);
        } else {
            deadline_.cancel();
        }
    }
    void append_proxy(std::shared_ptr<network_upgrade_interface> proxy) {
        if (!proxy) return;
        proxy_interfaces.emplace_back(proxy);
    }
    void set_proxy(std::shared_ptr<network_upgrade_interface> proxy) {
        proxy_interfaces.clear();
        if (!proxy) return;
        proxy_interfaces.emplace_back(proxy);
    }
    void set_proxy(std::vector<std::shared_ptr<network_upgrade_interface>> proxy_vec) {
        proxy_interfaces = proxy_vec;
    }
    void config_addr(const std::string& host, const std::string& service) {
        host_         = host;
        service_name_ = service;
    }

    void set_error_callback(std::function<void(asio::error_code)> f) {
        err_cb_ = std::move(f);
    }

    bool has_connected() const {
        return connection_status.load() == rpc_connection_ready;
    }
    bool has_closed() const {
        return connection_status.load() == rpc_connection_closed;
    }
    // sync call
    template <typename T = void, typename... Args>
    typename std::enable_if<std::is_void<T>::value>::type timeout_call(const std::string& rpc_name,
                                                                       std::size_t timeout_msec,
                                                                       Args&&... args) {
        auto result_context = async_timeout_call(rpc_name, timeout_msec, std::forward<Args>(args)...);
        result_context.guard_post_request();
        auto status = result_context.wait_for(std::chrono::milliseconds(timeout_msec));
        if (status == std::future_status::timeout || status == std::future_status::deferred) {
            throw std::out_of_range(
                Fundamental::StringFormat("rpc {}", status == std::future_status::timeout ? "timeout" : "deferred"));
        }
        result_context.get().as();
    }

    template <typename T, typename... Args>
    typename std::enable_if<!std::is_void<T>::value, T>::type timeout_call(const std::string& rpc_name,
                                                                           std::size_t timeout_msec,
                                                                           Args&&... args) {
        auto result_context = async_timeout_call(rpc_name, timeout_msec, std::forward<Args>(args)...);
        result_context.guard_post_request();
        auto status = result_context.wait_for(std::chrono::milliseconds(timeout_msec));
        if (status == std::future_status::timeout || status == std::future_status::deferred) {
            throw std::out_of_range(
                Fundamental::StringFormat("rpc {}", status == std::future_status::timeout ? "timeout" : "deferred"));
        }

        return result_context.get().template as<T>();
    }

    template <size_t TIMEOUT, typename T = void, typename... Args>
    typename std::enable_if<std::is_void<T>::value>::type call(const std::string& rpc_name, Args&&... args) {
        timeout_call(rpc_name, TIMEOUT, std::forward<Args>(args)...);
    }

    template <typename T = void, typename... Args>
    typename std::enable_if<std::is_void<T>::value>::type call(const std::string& rpc_name, Args&&... args) {
        call<kDefaultNetworkTimeOutMsec, T>(rpc_name, std::forward<Args>(args)...);
    }

    template <size_t TIMEOUT, typename T, typename... Args>
    typename std::enable_if<!std::is_void<T>::value, T>::type call(const std::string& rpc_name, Args&&... args) {
        return timeout_call<T>(rpc_name, TIMEOUT, std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    typename std::enable_if<!std::is_void<T>::value, T>::type call(const std::string& rpc_name, Args&&... args) {
        return call<kDefaultNetworkTimeOutMsec, T>(rpc_name, std::forward<Args>(args)...);
    }

    template <typename... Args>
    [[nodiscard]] result_context<req_result> async_call(const std::string& rpc_name, Args&&... args) {
        return async_timeout_call(rpc_name, kDefaultNetworkTimeOutMsec, std::forward<Args>(args)...);
    }

    template <typename... Args>
    [[nodiscard]] result_context<req_result> async_timeout_call(const std::string& rpc_name,
                                                                std::size_t timeout_msec,
                                                                Args&&... args) {
        if (has_upgrade) throw std::runtime_error("client has already upgrade");
        auto new_call                  = EmplaceNewCall(timeout_msec);
        std::future<req_result> future = new_call->finish_promise.get_future();

        rpc_service::msgpack_codec codec;
        auto ret = codec.pack(std::forward<Args>(args)...);

        auto delay_post_func = [this, ref = weak_from_this(), call_id = new_call->call_id, ret = std::move(ret),
                                func_id = md5_hasher::MD5Hash32(rpc_name.data())]() mutable {
            auto lock = ref.lock();
            if (!lock) return;
            write(call_id, request_type::rpc_req, std::move(ret), func_id);
        };
        return result_context<req_result> { new_call->call_id, std::move(future), new_call,
                                            std::move(delay_post_func) };
    }

    void stop() {
        release_obj();
    }
    void release_obj() {
        reference_.release();

        asio::post(ios_, [this, ref = shared_from_this()] {
            for (auto& i : proxy_interfaces)
                if (i) i->abort_all_operation();
            proxy_interfaces.clear();
            proxy_interfaces.shrink_to_fit();
            close(true);

            resolver_.cancel();
            reconnect_delay_timer_.cancel();
            deadline_.cancel();
        });
    }
    template <typename Func>
    result_context<req_result> subscribe(std::string key, Func f) {
        if (has_upgrade) throw std::runtime_error("client has already upgrade");
        {
            std::unique_lock<std::mutex> lock(sub_mtx_);
            auto it = sub_map_.find(key);
            if (it != sub_map_.end()) {
                FASSERT(false && "duplicated subscribe");
                return {};
            }
            sub_map_.emplace(key, std::move(f));
        }

        return post_future_call(request_type::rpc_subscribe, key);
    }

    result_context<req_result> unsubscribe(std::string key) {
        if (has_upgrade) throw std::runtime_error("client has already upgrade");
        {
            std::unique_lock<std::mutex> lock(sub_mtx_);
            auto it = sub_map_.find(key);
            if (it == sub_map_.end()) {
                FASSERT(false && "subscribe key not found");
                return {};
            }
            sub_map_.erase(key);
        }

        return post_future_call(request_type::rpc_subscribe, key);
    }

    template <typename... Args>
    result_context<req_result> publish(std::string key, Args&&... args) {
        // convert to format string data
        rpc_service::msgpack_codec codec;
        auto ret = codec.pack(std::forward<Args>(args)...);
        return post_future_call(request_type::rpc_publish, key, std::string(ret.data(), ret.data() + ret.size()));
    }

    void enable_ssl(network_client_ssl_config client_ssl_config,
                    rpc_client_ssl_level enable_ssl_level = rpc_client_ssl_level::rpc_client_ssl_level_required) {
#ifndef NETWORK_DISABLE_SSL
        ssl_config_ = client_ssl_config;
        ssl_level   = enable_ssl_level;
#endif
    }

    std::shared_ptr<ClientStreamReadWriter> upgrade_to_stream(
        std::string_view rpc_name,
        std::size_t timeout_msec     = kDefaultNetworkTimeOutMsec,
        std::size_t read_queue_size  = ClientStreamReadWriter::kDefaultReadQueueSize,
        std::size_t write_queue_size = ClientStreamReadWriter::kDefaultWriteQueueSize) {
        if (has_upgrade) throw std::runtime_error("client has already upgrade");
        auto new_call                  = EmplaceNewCall(timeout_msec);
        std::future<req_result> future = new_call->finish_promise.get_future();

        write(new_call->call_id, request_type::rpc_stream, {}, md5_hasher::MD5Hash32(rpc_name.data()));
        std::shared_ptr<ClientStreamReadWriter> ret;
        try {
            if (timeout_msec > 0)
                future.wait_for(std::chrono::milliseconds(timeout_msec));
            else {
                future.wait();
            }
            ret = ClientStreamReadWriter::make_shared(shared_from_this(), read_queue_size, write_queue_size);
            // release stream write when client was released
            auto release_handle = reference_.notify_release.Connect([con = ret->weak_from_this()]() {
                auto ptr = con.lock();
                if (ptr) ptr->release_obj();
            });
            // unbind
            ret->reference_.notify_release.Connect([release_handle, s = weak_from_this(), this]() {
                auto ptr = s.lock();
                if (ptr) reference_.notify_release.DisConnect(release_handle);
            });

            ret->start();
        } catch (const std::exception& e) {
#ifdef DEBUG
            std::cout << "upgrade_to_stream: failed for " << e.what();
#endif
        }
        return ret;
    }
    auto get_remote_peer_ip() const {
        return remote_ip;
    }
    auto get_remote_peer_port() const {
        return remote_port;
    }
    std::size_t pending_cache_data_size() const {
        return pending_size.load();
    }

private:
    void close(bool forced_close = false) {

        if (!has_connected() && !forced_close) return;
        change_connection_status(rpc_connection_closed);
        auto final_clear_function = [this, ptr = shared_from_this()]() {
            asio::error_code ec;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
            clear_cache();
        };

#ifndef NETWORK_DISABLE_SSL
        if (ssl_stream_) {
            ::asio::dispatch(ssl_stream_->get_executor(), final_clear_function);
            return;
        }
#endif
        final_clear_function();
    }

    std::shared_ptr<rpc_request_context> EmplaceNewCall(std::size_t timeout_msec = 15000) {
        auto p = std::make_shared<rpc_request_context>(ios_);
        if (timeout_msec == 0) timeout_msec = 15000;
        p->timeout_check_timer.expires_after(std::chrono::milliseconds(timeout_msec));
        p->timeout_check_timer.async_wait(
            [p = std::weak_ptr<rpc_request_context>(p), this, ptr = weak_from_this()](const asio::error_code& ec) {
                auto strong = ptr.lock();
                if (!strong) return;
                if (!reference_.is_valid()) {
                    return;
                }
                if (ec) return;
                auto p_instance = p.lock();
                if (p_instance) {
                    p_instance->finish(error::make_error_code(error::rpc_errors::rpc_timeout), "");
                    std::unique_lock<std::mutex> lock(rpc_calls_mtx_);
                    FDEBUG("{:p} remove call {} because of timeout", (void*)this, p_instance->call_id);
                    rpc_calls_map_.erase(p_instance->call_id);
                }
            });
        {
            std::unique_lock<std::mutex> lock(rpc_calls_mtx_);
            next_call_id_++;
            p->call_id = next_call_id_;
            rpc_calls_map_.emplace(p->call_id, p);
        }

        return p;
    }
    template <typename... Args>
    result_context<req_result> post_future_call(request_type call_type, Args&&... args) {

        auto new_call                  = EmplaceNewCall();
        std::future<req_result> future = new_call->finish_promise.get_future();

        rpc_service::msgpack_codec codec;
        auto ret = codec.pack(std::forward<Args>(args)...);
        write(new_call->call_id, call_type, std::move(ret), 0);
        return result_context<req_result> { new_call->call_id, std::move(future), new_call };
    }

    bool is_ssl() const {
#ifndef NETWORK_DISABLE_SSL
        return !ssl_config_.disable_ssl && ssl_level != rpc_client_ssl_level_none;
#else
        return false;
#endif
    }
    void do_async_connect() {
        auto error_handle_func = [this](const asio::error_code& ec) -> bool {
            if (has_connected() || !reference_.is_valid()) {
                return false;
            }
            if (!ec) return true;
            change_connection_status(rpc_connection_closing);
            if (reconnect_cnt_ <= 0) {
                change_connection_status(rpc_connection_closed);
                return false;
            }

            if (reconnect_cnt_ > 0) {
                reconnect_cnt_--;
            }
            async_reconnect();
            return false;
        };
        change_connection_status(rpc_connection_dns_resolving);
        resolver_.async_resolve(
            host_, service_name_,
            [this, error_handle_func, ptr = shared_from_this()](const std::error_code& ec,
                                                                const decltype(resolver_)::results_type& endpoints) {
                if (!reference_.is_valid()) {
                    return;
                }
                if (!error_handle_func(ec)) return;
                // connect endpoint using resolver results
                change_connection_status(rpc_connection_tcp_handshaking);
                asio::async_connect(socket_, endpoints,
                                    [this, error_handle_func, ptr = shared_from_this()](
                                        const asio::error_code& ec, const asio::ip::tcp::endpoint& endpoint) {
                                        if (!reference_.is_valid()) {
                                            return;
                                        }
                                        if (!error_handle_func(ec)) return;
                                        enable_tcp_keep_alive(socket_);
                                        remote_ip   = socket_.remote_endpoint().address().to_string();
                                        remote_port = socket_.remote_endpoint().port();
#ifdef RPC_VERBOSE
                                        FDEBUG("{:p} rpc connect to {}:{}", (void*)this, endpoint.address().to_string(),
                                               endpoint.port());
#endif
                                        handle_connect_success();
                                    });
            });
    }
    void proxy_write(write_buffer_t write_buffers, const network_upgrade_interface::operation_cb& finish_cb) {
        if (!reference_.is_valid()) {
            finish_cb(std::make_error_code(std::errc::operation_canceled), "aborted");
            return;
        }
        std::vector<network_write_buffer_t> buffers;
        for (auto& buffer : write_buffers) {
            buffers.emplace_back(network_write_buffer_t { buffer.data, buffer.len });
        }
        asio::async_write(socket_, std::move(buffers),
                          [this, ptr = shared_from_this(), finish_cb](const asio::error_code& ec, size_t) {
                              if (!reference_.is_valid()) {
                                  finish_cb(std::make_error_code(std::errc::operation_canceled), "aborted");
                                  return;
                              }
                              finish_cb(ec, "");
                          });
    }
    void proxy_read(read_buffer_t read_buffers, const network_upgrade_interface::operation_cb& finish_cb) {
        if (!reference_.is_valid()) {
            finish_cb(std::make_error_code(std::errc::operation_canceled), "aborted");
            return;
        }
        std::vector<network_read_buffer_t> buffers;
        for (auto& buffer : read_buffers) {
            buffers.emplace_back(network_read_buffer_t { buffer.data, buffer.len });
        }
        asio::async_read(socket_, std::move(buffers),
                         [this, ptr = shared_from_this(), finish_cb](const asio::error_code& ec, size_t) {
                             if (!reference_.is_valid()) {
                                 finish_cb(std::make_error_code(std::errc::operation_canceled), "aborted");
                                 return;
                             }
                             finish_cb(ec, "");
                         });
    }
    void perform_proxy(std::size_t layer) {
        change_connection_status(rpc_connection_proxy_handshaking);
        auto write_callback =
            [this, ptr = shared_from_this()](write_buffer_t write_buffers,
                                             const network_upgrade_interface::operation_cb& finish_cb) mutable {
                proxy_write(std::move(write_buffers), finish_cb);
            };
        auto read_callback =
            [this, ptr = shared_from_this()](read_buffer_t read_buffers,
                                             const network_upgrade_interface::operation_cb& finish_cb) mutable {
                proxy_read(std::move(read_buffers), finish_cb);
            };
        ;
        auto abort_callback = [this, ptr = shared_from_this()]() mutable {
            if (!reference_.is_valid()) {
                return;
            }
            asio::post(ios_, [this, ref = shared_from_this()] { close(true); });
        };
        auto finish_callback = [this, ptr = shared_from_this(), layer](std::error_code ec, const std::string& msg) {
            do {
                if (!reference_.is_valid()) {
                    if (!ec) ec = std::make_error_code(std::errc::bad_file_descriptor);
                    break;
                }
                FDEBUG("{} ec:{}({}) msg:{}", proxy_interfaces[layer]->interface_name(), ec.value(), ec.message(), msg);
                if (ec) break;
                auto next_layer = layer + 1;
                if (next_layer == proxy_interfaces.size()) {
                    handle_transfer_ready();
                } else {
                    perform_proxy(next_layer);
                }
                return;
            } while (0);

            change_connection_status(rpc_connection_closing);
            Fundamental::ScopeGuard g([&]() {
                if (connection_status.load() == rpc_connection_closing) change_connection_status(rpc_connection_closed);
            });
            error_callback(ec);
        };
        proxy_interfaces[layer]->init(read_callback, write_callback, finish_callback, abort_callback);
        proxy_interfaces[layer]->start();
    }
    void handle_connect_success() {
        if (!proxy_interfaces.empty())
            perform_proxy(0);
        else {
            handle_transfer_ready();
        }
    }
    void handle_transfer_ready() {
        if (is_ssl()) {
            ssl_handshake();
        } else {
            rpc_protocal_ready();
        }
    }
    void rpc_protocal_ready() {
        change_connection_status(rpc_connection_ready);
        socket_.set_option(asio::ip::tcp::no_delay(tcp_no_delay_flag));
        do_read();
        resend_subscribe();
        // process write cache
        do_write();
    }
    void async_reconnect() {
        if (!reference_.is_valid()) {
            return;
        }
        reset_socket();
        change_connection_status(rpc_connection_wait_connecting);
        reconnect_delay_timer_.expires_after(std::chrono::milliseconds(reconnect_delay_ms));
        reconnect_delay_timer_.async_wait([this, ptr = shared_from_this()](const asio::error_code& ec) {
            if (!reference_.is_valid()) {
                return;
            }
            if (!ec) {
                do_async_connect();
                return;
            }

            error_callback(ec);
        });
    }

    void reset_deadline_timer(size_t timeout_msec) {
        if (!reference_.is_valid()) {
            return;
        }

        deadline_.expires_after(std::chrono::milliseconds(timeout_msec));
        deadline_.async_wait([this, timeout_msec, ptr = weak_from_this()](const asio::error_code& ec) {
            auto strong = ptr.lock();
            if (!strong) return;
            if (!reference_.is_valid()) {
                return;
            }
            if (has_upgrade) return;
            if (!ec) {
                if (has_connected()) {
                    if (b_wait_any_data) {
                        b_wait_any_data.exchange(false);
                        FWARN("disconnect for timeout {} msec,we has not recv/send any data", timeout_msec);
                        close();
                    } else {
                        b_wait_any_data.exchange(true);
                        write(0, request_type::rpc_heartbeat, rpc_service::rpc_buffer_type(0), 0);
                    }

                } else {
                    b_wait_any_data.exchange(false);
                }
            }

            reset_deadline_timer(timeout_msec);
        });
    }

    void write(std::uint64_t req_id, request_type type, rpc_service::rpc_buffer_type&& message, uint32_t func_id) {
        FASSERT(message.size() < MAX_BUF_LEN);
        asio::post(executor_,
                   [this, req_id, type, func_id, message = std::move(message), ptr = shared_from_this()]() mutable {
                       if (!reference_.is_valid()) {
                           return;
                       }
                       client_message_type msg { req_id, type, std::move(message), func_id };
                       outbox_.emplace_back(std::move(msg));
                       pending_size.fetch_add(outbox_.back().content.size());
                       if (outbox_.size() > 1) {
                           // outstanding async_write
                           return;
                       }
                       do_write();
                   });
    }

    void do_write() {
        if (outbox_.empty() || !has_connected()) return;
        if (write_buffers_.empty()) {
            auto& msg       = outbox_.front();
            auto write_size = (uint32_t)msg.content.size();
            rpc_header { RPC_MAGIC_NUM, msg.req_type, write_size, msg.req_id, msg.func_id }.Serialize(
                write_head_.data(), kRpcHeadLen);
            write_buffers_.emplace_back(network_read_buffer_t(write_head_.data(), kRpcHeadLen));
            if (write_size > 0)
                write_buffers_.emplace_back(network_read_buffer_t((char*)msg.content.data(), write_size));
        }
        async_buffers_write_some(
            std::vector<network_write_buffer_t>(write_buffers_.begin(), write_buffers_.end()),
            [this, ptr = shared_from_this()](const asio::error_code& ec, size_t length) {
                if (!reference_.is_valid()) {
                    return;
                }
                if (ec) {
                    close();
                    error_callback(ec);

                    return;
                }
                b_wait_any_data.exchange(false);
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                FINFO("client {:p} write some size:{}", (void*)this, length);
#endif
                if (outbox_.empty()) {
                    return;
                }

                while (length != 0) {
                    if (write_buffers_.empty()) break;
                    auto current_size = write_buffers_.front().size();
                    if (length >= current_size) {
                        length -= current_size;
                        write_buffers_.pop_front();
                        continue;
                    }
                    write_buffers_.front() = network_write_buffer_t(
                        (std::uint8_t*)write_buffers_.front().data() + length, current_size - length);
                    break;
                }
                if (!write_buffers_.empty()) { // write rest data
                    do_write();
                    return;
                }
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                auto& msg = outbox_.front();
                FINFO("client {:p} write header:{} body:{}", (void*)this,
                      Fundamental::Utils::BufferToHex(write_head_.data(), kRpcHeadLen),
                      Fundamental::Utils::BufferToHex(msg.content.data(),
                                                      msg.content.size() > 32 ? 32 : msg.content.size(), 16));
#endif
                pending_size.fetch_sub(outbox_.front().content.size());
                outbox_.pop_front();

                if (!outbox_.empty()) {
                    // more messages to send
                    do_write();
                }
            });
    }

    void do_read() {
        async_buffers_read({ network_read_buffer_t(head_.data(), kRpcHeadLen) },
                           [this, ptr = shared_from_this()](const asio::error_code& ec, const size_t length) {
                               if (!reference_.is_valid()) {
                                   return;
                               }
                               if (!socket_.is_open()) {
                                   close();
                                   return;
                               }

                               if (!ec) {
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                                   FINFO("client {:p} read head: {}", (void*)this,
                                         Fundamental::Utils::BufferToHex(head_.data(), kRpcHeadLen));
#endif
                                   rpc_header header;
                                   header.DeSerialize(head_.data(), kRpcHeadLen);
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                                   FINFO("client {:p} read from connection:{}", (void*)this, header.func_id);
#endif
                                   if (header.req_type == request_type::rpc_heartbeat) {
                                       b_wait_any_data.exchange(false);
                                       do_read();
                                       return;
                                   }
                                   const uint32_t body_len = header.body_len;
                                   if (body_len > 0 && body_len < MAX_BUF_LEN) {
                                       if (body_.size() < body_len) {
                                           body_.resize(body_len);
                                       }
                                       read_body(header.req_id, header.req_type, body_len);
                                       return;
                                   }

                                   if (body_len == 0 || body_len > MAX_BUF_LEN) {
                                       close();
                                       error_callback(asio::error::make_error_code(asio::error::message_size));
                                       return;
                                   }
                               } else {
                                   close();
                                   error_callback(ec);
                               }
                           });
    }

    void read_body(std::uint64_t req_id, request_type req_type, size_t body_len, std::size_t read_offset = 0) {
        FASSERT(read_offset < body_len);
        async_buffers_read_some(
            { network_read_buffer_t(body_.data() + read_offset, body_len - read_offset) },
            [this, req_id, req_type, body_len, read_offset, ptr = shared_from_this()](asio::error_code ec,
                                                                                      std::size_t length) {
                if (!reference_.is_valid()) {
                    return;
                }
                if (!socket_.is_open()) {
                    call_back(req_id, asio::error::make_error_code(asio::error::connection_aborted), {});
                    return;
                }

                if (!ec) {
                    b_wait_any_data.exchange(false);
                    auto current_offset = read_offset + length;
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                    FINFO("client {:p}  read some need:{}  current: {} new:{}", (void*)this, body_len, current_offset,
                          length);
#endif
                    if (current_offset < body_len) {
                        read_body(req_id, req_type, body_len, current_offset);
                        return;
                    }
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                    FINFO("client {:p} read body: {}", (void*)this,
                          Fundamental::Utils::BufferToHex(body_.data(), body_len > 32 ? 32 : body_len, 16));
#endif

                    // entier body
                    if (req_type == request_type::rpc_res) {
                        call_back(req_id, ec, { body_.data(), body_len });
                        do_read();
                    } else if (req_type == request_type::rpc_publish) {
                        callback_sub(ec, { body_.data(), body_len });
                        do_read();
                    } else if (req_type == request_type::rpc_stream) {
                        call_back(req_id, ec, { body_.data(), body_len });
                        has_upgrade = true;
                    } else {
                        FWARN("invalid req_type failed {}", (std::int32_t)req_type);
                        close();
                        error_callback(asio::error::make_error_code(asio::error::invalid_argument));
                        return;
                    }
                } else {
                    FWARN("read failed {}", ec.message());
                    call_back(req_id, ec, {});
                    close();
                    error_callback(ec);
                }
            });
    }

    void resend_subscribe() {
        if (sub_map_.empty()) return;
        std::vector<std::string> sub_keys;
        {
            std::unique_lock<std::mutex> lock(sub_mtx_);
            for (auto& item : sub_map_) {
                sub_keys.push_back(item.first);
            }
        }
        for (auto& key : sub_keys) {
            post_future_call(request_type::rpc_subscribe, key);
        }
    }

    void call_back(uint64_t req_id, const asio::error_code& ec, string_view data) {
        decltype(rpc_calls_map_)::mapped_type req_p = nullptr;
        {
            std::unique_lock<std::mutex> lock(rpc_calls_mtx_);
            auto iter = rpc_calls_map_.find(req_id);
            if (iter == rpc_calls_map_.end()) {
                FDEBUG("{:p} ignore invalid req_id {}", (void*)this, req_id);
                return;
            }
            req_p = iter->second;
            FASSERT(req_p != nullptr, "internal error");
            rpc_calls_map_.erase(iter);
        }
        req_p->finish(ec, data);
    }

    void callback_sub(const asio::error_code& ec, string_view result) {
        rpc_service::msgpack_codec codec;
        try {
            auto tp = codec.unpack_tuple<std::tuple<int, std::string, std::string>>(result.data(), result.size());
            [[maybe_unused]] auto code = std::get<0>(tp);
            auto& key                  = std::get<1>(tp);
            auto& data                 = std::get<2>(tp);
            decltype(sub_map_)::iterator::value_type::second_type cb;
            {
                std::unique_lock<std::mutex> lock(sub_mtx_);
                auto it = sub_map_.find(key);
                if (it == sub_map_.end()) {
                    return;
                }
                cb = it->second;
            }
            cb(data);
        } catch (const std::exception& /*ex*/) {
            error_callback(asio::error::make_error_code(asio::error::invalid_argument));
        }
    }

    void clear_cache() {
        {
            while (!outbox_.empty()) {
                outbox_.pop_front();
            }
            pending_size.exchange(0);
            write_buffers_.clear();
        }
        decltype(rpc_calls_map_) tmp;

        {
            std::unique_lock<std::mutex> lock(rpc_calls_mtx_);
            std::swap(rpc_calls_map_, tmp);
        }
        {
            for (auto& item : tmp) {
                item.second->finish(network::rpc_service::error::make_error_code(
                                        network::rpc_service::error::rpc_errors::rpc_broken_pipe),
                                    "");
            }
            tmp.clear();
        }
    }

    void reset_socket() {
        do {

#ifndef NETWORK_DISABLE_SSL
            if (ssl_stream_) {
                ::asio::dispatch(ssl_stream_->get_executor(),
                                 [ssl_stream = std::move(ssl_stream_), sock = std::move(socket_)]() mutable {
                                     asio::error_code ignored_ec;
                                     ssl_stream->lowest_layer().close(ignored_ec);
                                 });
                ssl_stream_ = nullptr;
                break;
            }
#endif
            asio::error_code igored_ec;
            socket_.close(igored_ec);
        } while (0);

        socket_ = decltype(socket_)(ios_);
    }

    void error_callback(const asio::error_code& ec) {
        if (err_cb_) {
            err_cb_(ec);
        }

        if (enable_reconnect_) {
            async_reconnect();
        }
    }

    bool verify_certificate(bool preverified, asio::ssl::verify_context& ctx) {
        // The verify callback can be used to check whether the certificate that is
        // being presented is valid for the peer. For example, RFC 2818 describes
        // the steps involved in doing this for HTTPS. Consult the OpenSSL
        // documentation for more details. Note that the callback is called once
        // for each certificate in the certificate chain, starting from the root
        // certificate authority.
        do {
            // In this example we will simply print the certificate's subject name.
            X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
            if (!cert) break;
            auto org_name = read_org_name(cert);
            if (!org_name.empty()) {
#ifdef RPC_VERBOSE
                FDEBUG("rpc client {:p} ssl Verifying org:{}", (void*)this, org_name);
#endif
            }
        } while (0);
        return preverified;
    }
#ifndef NETWORK_DISABLE_SSL
    std::string read_org_name(X509* cert) {
        do {
            if (!cert) break;
            X509_NAME* subject_name = X509_get_subject_name(cert);
            if (!subject_name) {
                break;
            }
            char org_name[256];
            auto len = X509_NAME_get_text_by_NID(subject_name, NID_organizationName, org_name, sizeof(org_name));
            if (len > 0) {
                return org_name;
            }
        } while (0);
        return "";
    }
#endif
    void ssl_handshake() {
        change_connection_status(rpc_connection_closing);
        Fundamental::ScopeGuard g([&]() {
            // status not changed
            if (connection_status.load() == rpc_connection_closing) change_connection_status(rpc_connection_closed);
        });
#ifndef NETWORK_DISABLE_SSL
        asio::ssl::context ssl_context(asio::ssl::context::tlsv13);
        auto* actual_context = &ssl_context;
        try {
            if (ssl_config_.load_exception) std::rethrow_exception(ssl_config_.load_exception);
            if (!ssl_config_.ssl_context) {
                if (!ssl_config_.ca_certificate_path.empty()) {
                    ssl_context.load_verify_file(ssl_config_.ca_certificate_path);
                } else {
                    ssl_context.set_default_verify_paths();
                }
                if (!ssl_config_.private_key_path.empty()) {
                    ssl_context.use_private_key_file(ssl_config_.private_key_path, asio::ssl::context::pem);
                }
                if (!ssl_config_.certificate_path.empty()) {
                    ssl_context.use_certificate_chain_file(ssl_config_.certificate_path);
                }
            } else {
                actual_context = ssl_config_.ssl_context.get();
            }

            ssl_level = rpc_client_ssl_level_required;
            FDEBUG("load client ssl config success  ca:{} crt:{} key:{}", ssl_config_.ca_certificate_path,
                   ssl_config_.certificate_path, ssl_config_.private_key_path);
        } catch (const std::exception& e) {
            FERR("load ssl config failed {} :{} {} {}", e.what(), ssl_config_.ca_certificate_path,
                 ssl_config_.certificate_path, ssl_config_.private_key_path);
            if (ssl_level == rpc_client_ssl_level_required) {
                close();
                error_callback(asio::error::make_error_code(asio::error::invalid_argument));
                return;
            }
        }

        ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket_, *actual_context);
        if (ssl_level == rpc_client_ssl_level_required) {
            ssl_stream_->set_verify_mode(asio::ssl::verify_peer);
            ssl_stream_->set_verify_callback(
                std::bind(&rpc_client::verify_certificate, this, std::placeholders::_1, std::placeholders::_2));
        } else {
            ssl_stream_->set_verify_mode(asio::ssl::verify_none);
        }
        SSL_set_tlsext_host_name(ssl_stream_->native_handle(), host_.c_str());
        change_connection_status(rpc_connection_ssl_handshaking);
        ssl_stream_->async_handshake(
            asio::ssl::stream_base::client, [this, ptr = shared_from_this()](const asio::error_code& ec) {
                if (!reference_.is_valid()) {
                    return;
                }
                if (!ec) {
                    rpc_protocal_ready();
                } else {
                    change_connection_status(rpc_connection_closing);
                    Fundamental::ScopeGuard g([&]() {
                        // status not changed
                        if (connection_status.load() == rpc_connection_closing)
                            change_connection_status(rpc_connection_closed);
                    });
                    FDEBUG("perform client {:p} ssl handshake failed {}", (void*)this, ec.message());
                    close();
                    error_callback(ec);
                }
            });
#endif
    }

    void async_buffers_read(network_read_buffers_t buffers, network_io_handler_t handler) {
        if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            asio::async_read(*ssl_stream_, std::move(buffers), std::move(handler));
#endif
        } else {
            asio::async_read(socket_, std::move(buffers), std::move(handler));
        }
    }
    void async_buffers_read_some(network_read_buffers_t buffers, network_io_handler_t handler) {
        if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            ssl_stream_->async_read_some(std::move(buffers), std::move(handler));
#endif
        } else {
            socket_.async_read_some(std::move(buffers), std::move(handler));
        }
    }

    void async_buffers_write(network_write_buffers_t buffers, network_io_handler_t handler) {
        if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            asio::async_write(*ssl_stream_, std::move(buffers), std::move(handler));
#endif
        } else {
            asio::async_write(socket_, std::move(buffers), std::move(handler));
        }
    }
    void async_buffers_write_some(network_write_buffers_t buffers, network_io_handler_t handler) {
        if (is_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            ssl_stream_->async_write_some(std::move(buffers), std::move(handler));
#endif
        } else {
            socket_.async_write_some(std::move(buffers), std::move(handler));
        }
    }
    void change_connection_status(rpc_connection_status next_status) {
        auto old_status = connection_status.load();
        while (old_status != next_status) {
            if (connection_status.compare_exchange_strong(old_status, next_status)) {
                std::scoped_lock<std::mutex> locker(status_mutex_);
                status_cv.notify_all();
                break;
            }
        }
    }
    network_data_reference reference_;
    asio::io_context& ios_;
    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::socket socket_;
    const asio::any_io_executor& executor_;

    std::string host_;
    std::string service_name_;
    asio::steady_timer reconnect_delay_timer_;
    size_t reconnect_delay_ms = 1000; // s
    bool tcp_no_delay_flag    = false;
    int reconnect_cnt_        = -1;

    std::mutex status_mutex_;
    std::condition_variable status_cv;
    std::atomic<rpc_connection_status> connection_status = rpc_connection_closed;

    std::atomic_bool b_wait_any_data = false;
    asio::steady_timer deadline_;

    struct client_message_type {
        client_message_type(std::uint64_t req_id,
                            request_type req_type,
                            rpc_service::rpc_buffer_type content,
                            uint32_t func_id) :
        req_id(req_id), req_type(req_type), content(std::move(content)), func_id(func_id) {
        }
        std::uint64_t req_id;
        request_type req_type;
        rpc_service::rpc_buffer_type content;
        uint32_t func_id;
    };
    std::deque<client_message_type> outbox_;
    uint64_t next_call_id_ = 0;
    std::function<void(asio::error_code)> err_cb_;
    bool enable_reconnect_ = false;

    std::unordered_map<std::uint64_t, std::shared_ptr<rpc_request_context>> rpc_calls_map_;
    std::mutex rpc_calls_mtx_;

    std::array<char, kRpcHeadLen> head_;
    std::vector<char> body_;

    std::array<std::uint8_t, kRpcHeadLen> write_head_;
    //
    std::list<network_write_buffer_t> write_buffers_;
    std::mutex sub_mtx_;
    std::unordered_map<std::string, std::function<void(string_view)>> sub_map_;
    //
    std::vector<std::shared_ptr<network_upgrade_interface>> proxy_interfaces;
    rpc_client_ssl_level ssl_level = rpc_client_ssl_level::rpc_client_ssl_level_none;
#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_ = nullptr;
    network_client_ssl_config ssl_config_;
#endif
    // rpc handler
    std::atomic_bool has_upgrade = false;
    // remote endpoint
    std::string remote_ip;
    std::uint16_t remote_port = 0;
    //
    std::atomic<std::size_t> pending_size = 0;
};

inline ClientStreamReadWriter::ClientStreamReadWriter(std::shared_ptr<rpc_client> client,
                                                      std::size_t read_queue_max_size,
                                                      std::size_t write_queue_max_size) :
client_(client), deadline_(client_->executor_), read_queue_max_size_(read_queue_max_size),
write_queue_max_size_(write_queue_max_size) {
    write_token_nums = write_queue_max_size_;
#ifdef RPC_VERBOSE
    FDEBUG("build stream writer {:p} with client:{:p}", (void*)this, (void*)&client_);
#endif
}
inline ClientStreamReadWriter::~ClientStreamReadWriter() {
#ifdef RPC_VERBOSE
    FDEBUG("release stream writer {:p} with client:{:p}", (void*)this, (void*)&client_);
#endif
}
template <typename T>
inline bool ClientStreamReadWriter::Read(T& request, std::size_t max_wait_ms) {
    auto check_func = [this]() -> bool {
        return last_data_status_ >= rpc_stream_data_status::rpc_stream_write_done || !response_cache_.empty();
    };
    std::vector<std::uint8_t> response_data;
    {
        std::unique_lock<std::mutex> locker(read_mutex);
        if (max_wait_ms > 0)
            read_cv_.wait_for(locker, std::chrono::milliseconds(max_wait_ms), check_func);
        else {
            read_cv_.wait(locker, check_func);
        }
        if (response_cache_.empty()) return false;
        response_data = std::move(response_cache_.front());
        response_cache_.pop_front();
        if (last_data_status_ < rpc_stream_data_status::rpc_stream_finish) {
            try_resume_read();
        }
    }
    try {
        request = msgpack_codec::unpack<T>(response_data.data(), response_data.size());
    } catch (const std::exception& e) {
        set_status(rpc_stream_data_status::rpc_stream_failed,
                   error::make_error_code(error::rpc_errors::rpc_unpack_failed));

        return false;
    }

    return true;
}
template <typename U>
inline bool ClientStreamReadWriter::Write(U&& response, std::size_t max_wait_ms) {
    rpc_buffer_type data;
    try {
        data = msgpack_codec::pack(std::forward<U>(response));
    } catch (const std::exception& e) {
        set_status(rpc_stream_data_status::rpc_stream_failed,
                   error::make_error_code(error::rpc_errors::rpc_pack_failed));

        return false;
    }
    if (!wait_write_token(max_wait_ms)) return false;
    asio::post(client_->executor_, [this, data = std::move(data), ref = shared_from_this()]() mutable {
        if (!reference_.is_valid()) return;
        auto& new_item = write_cache_.emplace_back();
        new_item.size  = Fundamental::host_value_convert(static_cast<std::uint32_t>(data.size()));
        new_item.type  = static_cast<std::uint8_t>(rpc_stream_data_status::rpc_stream_data);
        new_item.data  = std::move(data);
        if (write_cache_.size() == 1) handle_write();
    });
    return true;
}

inline bool ClientStreamReadWriter::WriteEmpty(std::size_t max_wait_ms) {
    if (!wait_write_token(max_wait_ms)) return false;
    asio::post(client_->executor_, [this, ref = shared_from_this()]() mutable {
        if (!reference_.is_valid()) return;
        auto& new_item = write_cache_.emplace_back();
        new_item.size  = 0;
        new_item.type  = static_cast<std::uint8_t>(rpc_stream_data_status::rpc_stream_data);
        new_item.data.resize(0);
        if (write_cache_.size() == 1) handle_write();
    });
    return true;
}

inline bool ClientStreamReadWriter::ReadEmpty(std::size_t max_wait_ms) {
    auto check_func = [this]() -> bool {
        return last_data_status_ >= rpc_stream_data_status::rpc_stream_write_done || !response_cache_.empty();
    };
    {
        std::unique_lock<std::mutex> locker(read_mutex);
        if (max_wait_ms > 0)
            read_cv_.wait_for(locker, std::chrono::milliseconds(max_wait_ms), check_func);
        else {
            read_cv_.wait(locker, check_func);
        }
        if (response_cache_.empty()) return false;
        response_cache_.pop_front();
        if (last_data_status_ < rpc_stream_data_status::rpc_stream_finish) {
            try_resume_read();
        }
    }
    return true;
}

inline bool ClientStreamReadWriter::WriteDone() {
    if (last_data_status_ >= rpc_stream_data_status::rpc_stream_finish) return false;
    asio::post(client_->executor_, [this, ref = shared_from_this()]() mutable {
        if (!reference_.is_valid()) return;
        auto& new_item = write_cache_.emplace_back();
        new_item.size  = 0;
        new_item.type  = static_cast<std::uint8_t>(rpc_stream_data_status::rpc_stream_write_done);
        new_item.data.clear();
        if (write_cache_.size() == 1) handle_write();
    });
    return true;
}

inline std::error_code ClientStreamReadWriter::Finish(std::size_t max_wait_ms) {
    {
        auto expected_value = false;
        if (!is_request_finishing.compare_exchange_strong(expected_value, true)) {
            FASSERT(false, "call ClientStreamReadWriter twice");
            return last_err_;
        }
    }
    do {
        if (last_data_status_ >= rpc_stream_data_status::rpc_stream_finish) break;
        asio::post(client_->executor_, [this, ref = shared_from_this()]() mutable {
            if (!reference_.is_valid()) return;
            auto& new_item = write_cache_.emplace_back();
            new_item.size  = 0;
            new_item.type  = static_cast<std::uint8_t>(rpc_stream_data_status::rpc_stream_finish);
            new_item.data.clear();
            if (write_cache_.size() == 1) handle_write();
        });
        auto check_func = [this]() -> bool { return last_data_status_ >= rpc_stream_data_status::rpc_stream_finish; };
        try_resume_read();
        std::unique_lock<std::mutex> locker(read_mutex);
        if (max_wait_ms > 0)
            read_cv_.wait_for(locker, std::chrono::milliseconds(max_wait_ms), check_func);
        else {
            read_cv_.wait(locker, check_func);
        }
    } while (0);

    return last_err_;
}

inline std::error_code ClientStreamReadWriter::GetLastError() const {
    return last_err_;
}
inline void ClientStreamReadWriter::EnableAutoHeartBeat(bool enable, std::size_t timeout_msec) {
    timeout_msec_ = timeout_msec;
    deadline_.cancel();
    if (!enable) {
        timeout_msec_ = 0;
    } else {
        reset_timer();
    }
}
inline void ClientStreamReadWriter::release_obj() {
    reference_.release();
    asio::post(client_->executor_, [this, ref = shared_from_this()] {
        if (last_data_status_ < rpc_stream_data_status::rpc_stream_finish) {
            set_status(rpc_stream_data_status::rpc_stream_failed,
                       error::make_error_code(error::rpc_errors::rpc_internal_error));
        }
        deadline_.cancel();
        client_->close(true);
    });
}
inline std::string ClientStreamReadWriter::get_remote_peer_ip() const {
    return client_->get_remote_peer_ip();
}
inline std::uint16_t ClientStreamReadWriter::get_remote_peer_port() const {
    return client_->get_remote_peer_port();
}
inline void ClientStreamReadWriter::read_head() {
    if (is_reading_pausing) return;
    std::vector<network_read_buffer_t> buffers;
    buffers.emplace_back(network_read_buffer_t(&read_packet_buffer.size, sizeof(read_packet_buffer.size)));
    buffers.emplace_back(network_read_buffer_t(&read_packet_buffer.type, 1));

    client_->async_buffers_read(
        std::move(buffers), [this, ptr = shared_from_this()](asio::error_code ec, std::size_t length) {
            if (!reference_.is_valid()) {
                return;
            }
            if (last_data_status_ >= rpc_stream_data_status::rpc_stream_finish) return;
            if (ec) {
                set_status(rpc_stream_data_status::rpc_stream_failed, std::move(ec));
                notify_stream_abort.Emit();
            } else {
                b_wait_any_data.exchange(false);
#ifdef RPC_VERBOSE
                FDEBUG("client {:p} stream read head : {}", (void*)this,
                       Fundamental::Utils::BufferToHex(&read_packet_buffer.size, sizeof(read_packet_buffer.size)));
#endif
                auto status = static_cast<rpc_stream_data_status>(read_packet_buffer.type);
                if ((status != rpc_stream_data_status::rpc_stream_heartbeat && status < last_data_status_) ||
                    status >= rpc_stream_data_status::rpc_stream_finish) {
                    set_status(rpc_stream_data_status::rpc_stream_failed,
                               error::make_error_code(error::rpc_errors::rpc_bad_request));
                    return;
                }
                switch (status) {
                case rpc_stream_data_status::rpc_stream_data: {
                    {
                        std::scoped_lock<std::mutex> locker(read_mutex);
                        last_data_status_ = rpc_stream_data_status::rpc_stream_data;
                    }
                    read_packet_buffer.size = Fundamental::host_value_convert(read_packet_buffer.size);
                    try {
                        if (read_packet_buffer.size > read_packet_buffer.data.size())
                            read_packet_buffer.data.resize(read_packet_buffer.size);
                        read_body();
                    } catch (...) {
                        set_status(rpc_stream_data_status::rpc_stream_failed,
                                   error::make_error_code(error::rpc_errors::rpc_memory_error));
                    }

                } break;
                case rpc_stream_data_status::rpc_stream_write_done: {
                    set_status(status, error::make_error_code(error::rpc_errors::rpc_success));
                    read_head();
                } break;
                case rpc_stream_data_status::rpc_stream_heartbeat: {
                    read_head();
                } break;
                default: {
                    set_status(rpc_stream_data_status::rpc_stream_failed,
                               error::make_error_code(error::rpc_errors::rpc_bad_request));
                    break;
                }
                }
            }
        });
}
inline void ClientStreamReadWriter::read_body(std::uint32_t offset) {
    std::vector<network_read_buffer_t> buffers;
    buffers.emplace_back(
        network_read_buffer_t(read_packet_buffer.data.data() + offset, read_packet_buffer.size - offset));
    client_->async_buffers_read_some(
        std::move(buffers), [this, offset, ptr = shared_from_this()](asio::error_code ec, std::size_t length) {
            if (!reference_.is_valid()) {
                return;
            }
            if (last_data_status_ >= rpc_stream_data_status::rpc_stream_write_done) return;
            if (ec) {
                set_status(rpc_stream_data_status::rpc_stream_failed, std::move(ec));
                notify_stream_abort();
            } else {
                b_wait_any_data.exchange(false);
                auto current_offset = offset + length;
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                FINFO("client {:p} stream read some need:{}  current: {} new:{}", (void*)this, read_packet_buffer.size,
                      current_offset, length);
#endif
                if (current_offset < read_packet_buffer.size) {
                    read_body(static_cast<std::uint32_t>(current_offset));
                    return;
                }
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                FINFO("client {:p} stream read :{:x} {}", (void*)this, read_packet_buffer.type,
                      Fundamental::Utils::BufferToHex(read_packet_buffer.data.data(),
                                                      read_packet_buffer.size > 32 ? 32 : read_packet_buffer.size, 16));
#endif
                check_pause_read();
                read_head();
                // discard all request frame when finish was called
                if (is_request_finishing) return;
                {
                    std::scoped_lock<std::mutex> locker(read_mutex);
                    response_cache_.emplace_back(std::move(read_packet_buffer.data));
                    read_cv_.notify_one();
                }
            }
        });
}
inline void ClientStreamReadWriter::set_status(rpc_stream_data_status status, std::error_code ec) {
    std::scoped_lock<std::mutex, std::mutex> locker(read_mutex, write_mutex);
    Fundamental::ScopeGuard write_notify_g([&]() {
        read_cv_.notify_all();
        write_cv_.notify_all();
    });
    if (last_data_status_.load() >= rpc_stream_data_status::rpc_stream_finish) {
        return;
    }

    last_err_         = std::move(ec);
    last_data_status_ = status;
    if (last_data_status_.load() >= rpc_stream_data_status::rpc_stream_finish) {
#ifdef RPC_VERBOSE
        FDEBUG("rpc stream client {:p} finish success:{} {}", (void*)this,
               last_data_status_.load() == rpc_stream_data_status::rpc_stream_finish, last_err_.message());
#endif
        release_obj();
    }
}
inline void ClientStreamReadWriter::handle_write() {
    if (last_data_status_.load() >= rpc_stream_data_status::rpc_stream_finish) {
        return;
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
    client_->async_buffers_write_some(
        std::vector<network_write_buffer_t>(write_buffers_.begin(), write_buffers_.end()),
        [this, ptr = shared_from_this()](asio::error_code ec, std::size_t length) {
            if (!reference_.is_valid()) {
                return;
            }
            if (last_data_status_ >= rpc_stream_data_status::rpc_stream_finish) return;
            if (ec) {
                set_status(rpc_stream_data_status::rpc_stream_failed, ec);
                notify_stream_abort();
                return;
            }
            if (write_cache_.empty()) return;
            // write success means connection is active
            b_wait_any_data.exchange(false);
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FINFO("client {:p} stream write some size:{}", (void*)this, length);
#endif
            while (length != 0) {
                if (write_buffers_.empty()) break;
                auto current_size = write_buffers_.front().size();
                if (length >= current_size) {
                    length -= current_size;
                    write_buffers_.pop_front();
                    continue;
                }
                write_buffers_.front() = network_write_buffer_t((std::uint8_t*)write_buffers_.front().data() + length,
                                                                current_size - length);
                break;
            }
            if (!write_buffers_.empty()) { // write rest data
                handle_write();
                return;
            }
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            auto& item = write_cache_.front();
            FINFO("client {:p} stream write size:{} type:{} data:{}", (void*)this,
                  Fundamental::Utils::BufferToHex(&item.size, sizeof(item.size)), static_cast<std::uint32_t>(item.type),
                  Fundamental::Utils::BufferToHex(item.data.data(), item.data.size() > 32 ? 32 : item.data.size(), 16));
#endif
            auto packet_type = write_cache_.front().type;
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
            if (packet_type == static_cast<std::uint8_t>(rpc_stream_data_status::rpc_stream_finish)) {
                write_cache_.clear();
                write_buffers_.clear();
                set_status(rpc_stream_data_status::rpc_stream_finish,
                           error::make_error_code(error::rpc_errors::rpc_success));
            } else {
                handle_write();
            }
        });
}

inline void ClientStreamReadWriter::check_pause_read() {
    if (is_request_finishing) return;
    std::scoped_lock<std::mutex> locker(read_mutex);
    if (response_cache_.size() > read_queue_max_size_) {
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
        FERR("client stream {:p} pause read", (void*)this);
#endif
        is_reading_pausing.exchange(true);
    }
}

inline void ClientStreamReadWriter::try_resume_read() {
    auto expected_value = true;
    if (is_reading_pausing.compare_exchange_strong(expected_value, false)) {
        asio::post(client_->executor_, [this, ref = shared_from_this()]() mutable {
            if (!reference_.is_valid()) return;
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FERR("client stream {:p} resume read", (void*)this);
#endif
            read_head();
        });
    }
}
} // namespace rpc_service
} // namespace network
