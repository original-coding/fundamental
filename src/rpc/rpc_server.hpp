#ifndef REST_RPC_RPC_SERVER_H_
#define REST_RPC_RPC_SERVER_H_

#include "basic/router.hpp"
#include "connection.h"

#include "fundamental/basic/cxx_config_include.hpp"
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

#include "fundamental/basic/utils.hpp"
#include "fundamental/events/event_system.h"
#include "fundamental/thread_pool/thread_pool.h"

#include "network/network.hpp"

using asio::ip::tcp;

namespace network
{
namespace rpc_service
{
class ServerStreamReadWriter;
using rpc_conn = std::weak_ptr<connection>;

class rpc_server : private asio::noncopyable, public std::enable_shared_from_this<rpc_server> {
    friend class connection;

public:
    Fundamental::Signal<void(asio::error_code, string_view)> on_err;
    Fundamental::Signal<void(std::shared_ptr<connection>, std::string)> on_net_err;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<rpc_server>(std::forward<Args>(args)...);
    }
    template <typename port_type>
    rpc_server(port_type port, size_t timeout_msec = 30000) :
    acceptor_(io_context_pool::Instance().get_io_context()), timeout_msec_(timeout_msec) {
        protocal_helper::init_acceptor(acceptor_, static_cast<std::uint16_t>(port));
    }
    ~rpc_server() {
        FDEBUG("release rpc_server");
    }

    void start() {
        bool expected_value = false;
        if (!has_started_.compare_exchange_strong(expected_value, true)) return;
        FINFO("start rpc server on {}:{}", acceptor_.local_endpoint().address().to_string(),
              acceptor_.local_endpoint().port());
        do_accept();
    }

    void stop() {
        release_obj();
    }
    template <std::int32_t pool_index = Fundamental::ThreadPoolType::LongTimeThreadPool>
    void register_stream_handler(std::string const& name,
                                 const std::function<void(std::shared_ptr<ServerStreamReadWriter>)>& f) {
        auto thread_name = map_thread_name(name, "rpcs_");
        FINFO("register async rpc stream:{} thread_name:{}", name, thread_name);
        auto proxy_func = [f, thread_name](network::rpc_service::rpc_conn conn) {
            auto c = conn.lock();
            if (!c) return;
            auto w = c->InitRpcStream();
            if (!w) return;
            auto& pool     = Fundamental::ThreadPool::Instance<pool_index>();
            auto task_func = [f, thread_name](std::shared_ptr<ServerStreamReadWriter> read_writer) mutable {
                Fundamental::Utils::SetThreadName(thread_name);
                try {
                    f(read_writer);
                } catch (...) {
                    read_writer->release_obj();
                }
            };
            pool.Enqueue(task_func, std::move(w));
        };
        router_.register_handler<false>(name, proxy_func);
    }

    template <typename Function, std::int32_t pool_index = Fundamental::ThreadPoolType::ConsumerThreadPool>
    void register_delay_handler(std::string const& name, Function f) {
        auto thread_name = map_thread_name(name, "rpc_");
        FINFO("register async rpc interface:{} thread_name:{}", name, thread_name);
        auto proxy_func = [f, name, thread_name](std::weak_ptr<connection> conn, std::string_view str,
                                                 std::string& /*result*/) mutable -> void {
            auto c = conn.lock();
            if (!c) return;
            c->set_delay(true);
            auto req_id    = c->request_id();
            auto& pool     = Fundamental::ThreadPool::Instance<pool_index>();
            auto task_func = [name, thread_name](const Function& f, std::weak_ptr<connection> conn, std::string data,
                                                 decltype(req_id) req_id) mutable {
                auto c = conn.lock();
                if (!c) return;
                Fundamental::Utils::SetThreadName(thread_name);
                try {
                    msgpack_codec codec;
                    using args_tuple = typename function_traits<Function>::bare_tuple_type;
                    auto tp          = codec.unpack_tuple<args_tuple>(data.data(), data.size());
                    helper_t<args_tuple, false> { tp }();

                    std::string process_result;
                    router::call(f, conn, process_result, std::move(tp));
                    c->response_interal(req_id, std::move(process_result));

                } catch (std::invalid_argument& e) {
                    auto err_msg = Fundamental::StringFormat("rpc:{} invalid argument exception {}", name, e.what());
                    FERR(err_msg);
                    c->response_errmsg(req_id, network::rpc_service::request_type::rpc_res, err_msg);
                } catch (const std::exception& e) {
                    auto err_msg = Fundamental::StringFormat("rpc:{} has unexpected exception {}", name, e.what());
                    FERR(err_msg);
                    c->response_errmsg(req_id, network::rpc_service::request_type::rpc_res, err_msg);
                } catch (...) {
                    auto err_msg = Fundamental::StringFormat("rpc:{} has unexpected unknown exception", name);
                    FERR(err_msg);
                    c->response_errmsg(req_id, network::rpc_service::request_type::rpc_res, err_msg);
                }
            };
            pool.Enqueue(task_func, f, conn, std::string(str), req_id);
        };
        router_.register_wrapper_handler(name, proxy_func);
    }

    template <bool is_pub = false, typename Function>
    void register_handler(std::string const& name, const Function& f) {
        router_.register_handler<is_pub>(name, f);
    }

    template <bool is_pub = false, typename Function, typename Self>
    void register_handler(std::string const& name, const Function& f, Self* self) {
        router_.register_handler<is_pub>(name, f, self);
    }

    template <typename... Args>
    void publish(std::string key, Args&&... args) {
        {
            std::unique_lock<std::mutex> lock(sub_mtx_);
            if (sub_map_.empty()) return;
        }
        auto ret = rpc_service::msgpack_codec::pack(std::forward<Args>(args)...);
        std::string s_data(ret.data(), ret.data() + ret.size());
        forward_msg(std::move(key), std::move(s_data));
    }

    void release_obj() {
        reference_.release();
        bool expected_value = true;
        if (!has_started_.compare_exchange_strong(expected_value, false)) return;
        asio::post(acceptor_.get_executor(), [this, ref = shared_from_this()] {
            try {
                std::error_code ec;
                acceptor_.close(ec);
            } catch (const std::exception& e) {
            }
        });
    }
    // this function will throw when param is invalid
    void enable_ssl(network_server_ssl_config ssl_config) {
        if (ssl_config.disable_ssl) return;
#ifndef NETWORK_DISABLE_SSL
        if (ssl_config.certificate_path.empty() || ssl_config.private_key_path.empty() ||
            !std_fs::is_regular_file(ssl_config.certificate_path) ||
            !std_fs::is_regular_file(ssl_config.private_key_path)) {
            throw std::invalid_argument("rpc_server/ssl need valid certificate and key file");
        }

        if (!ssl_config.tmp_dh_path.empty() && !std_fs::is_regular_file(ssl_config.tmp_dh_path)) {
            throw std::invalid_argument("tmp_dh_path is not existed");
        }

        std::swap(ssl_config_, ssl_config);
        if (!ssl_config_.passwd_cb) ssl_config_.passwd_cb = [](std::string) -> std::string { return "123456"; };

        unsigned long ssl_options = asio::ssl::context::no_sslv2 | asio::ssl::context::single_dh_use;
        ssl_context               = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv13);
        try {
            ssl_context->set_options(ssl_options);
            ssl_context->set_password_callback(
                [cb = ssl_config_.passwd_cb](std::size_t size, asio::ssl::context_base::password_purpose purpose) {
                    return cb(std::to_string(size) + " " + std::to_string(static_cast<std::size_t>(purpose)));
                });
            auto verify_flag = ::asio::ssl::verify_peer;
            if (!ssl_config_.ca_certificate_path.empty() && std_fs::is_regular_file(ssl_config.ca_certificate_path)) {
                ssl_context->load_verify_file(ssl_config_.ca_certificate_path);
            } else {
                ssl_context->set_default_verify_paths();
            }
            if (ssl_config_.verify_client) verify_flag |= ::asio::ssl::verify_fail_if_no_peer_cert;
            ssl_context->set_verify_mode(verify_flag);

            ssl_context->use_certificate_chain_file(ssl_config_.certificate_path);
            ssl_context->use_private_key_file(ssl_config_.private_key_path, asio::ssl::context::pem);
            if (!ssl_config_.tmp_dh_path.empty()) ssl_context->use_tmp_dh_file(ssl_config_.tmp_dh_path);
    #ifdef RPC_VERBOSE
            FDEBUG("load ssl config ca:{} key:{} crt:{} dh:{}", ssl_config_.ca_certificate_path,
                   ssl_config_.private_key_path, ssl_config_.certificate_path, ssl_config_.tmp_dh_path);
    #endif
        } catch (const std::exception& e) {
            throw std::invalid_argument(std::string("load ssl config failed ") + e.what());
        }

#endif
    }

    void enable_data_proxy(network::proxy::ProxyManager* manager) {
        proxy_manager = manager;
    }
    void set_external_config(rpc_server_external_config config) {
        external_config = config;
    }
    void enable_socks5_proxy(std::shared_ptr<SocksV5::Sock5Handler> socks5_handler) {
        socks5_proxy_handler = socks5_handler;
    }
    rpc_client_forward_config get_client_forward_config() const {
        return external_config.forward_config;
    }

private:
    std::string map_thread_name(const std::string& name, std::string prefix) {
        std::uint32_t hash_value = md5_hasher::MD5Hash32(name.c_str(), static_cast<std::uint32_t>(name.length()));
        return Fundamental::StringFormat("{}{}", prefix.empty() ? "rpc_" : prefix.substr(0, 5), hash_value);
    }
    void do_accept() {

        acceptor_.async_accept(
            io_context_pool::Instance().get_io_context(),
            [this, ptr = shared_from_this()](asio::error_code ec, asio::ip::tcp::socket socket) {
                if (!reference_.is_valid()) {
                    return;
                }
                if (!acceptor_.is_open()) {
                    return;
                }

                if (ec) {
                    // maybe system error... ignored
                } else {
                    auto new_conn =
                        connection::make_shared(std::move(socket), timeout_msec_, router_, weak_from_this());
                    new_conn->on_new_subscriber_added.Connect([this](std::string key, std::weak_ptr<connection> conn) {
                        std::unique_lock<std::mutex> lock(sub_mtx_);
                        sub_map_.emplace(std::move(key), conn);
                    });
                    if (on_net_err) {
                        new_conn->on_net_err_.Connect(on_net_err);
                    }
                    auto release_handle = reference_.notify_release.Connect([con = new_conn->weak_from_this()]() {
                        auto ptr = con.lock();
                        if (ptr) ptr->release_obj();
                    });
                    // unbind
                    new_conn->reference_.notify_release.Connect([release_handle, s = weak_from_this(), this]() {
                        auto ptr = s.lock();
                        if (ptr) reference_.notify_release.DisConnect(release_handle);
                    });
                    new_conn->on_publish_msg.Connect([this, s = weak_from_this()](std::string key, std::string data) {
                        auto ptr = s.lock();
                        if (!ptr) return;
                        forward_msg(std::move(key), std::move(data));
                    });
                    new_conn->on_subscribers_removed.Connect(
                        [this, s = weak_from_this()](const std::unordered_set<std::string>& keys,
                                                     std::weak_ptr<connection> conn) {
                            auto ptr = s.lock();
                            if (!ptr) return;
                            std::unique_lock<std::mutex> lock(sub_mtx_);
                            for (auto& key : keys) {
                                auto range = sub_map_.equal_range(key);
                                for (auto it = range.first; it != range.second;) {
                                    if (it->second.lock() == conn.lock()) {
                                        it = sub_map_.erase(it);
                                        break;
                                    } else {
                                        ++it;
                                    }
                                }
                            }
                        });
#ifndef NETWORK_DISABLE_SSL
                    if (ssl_context) {
                        new_conn->enable_ssl(*ssl_context, ssl_config_.enable_no_ssl);
                    }
#endif
                    std::int64_t id = 0;
                    // increase conn_id_ and return old value
                    while (true) {
                        id = conn_id_.load();
                        if (conn_id_.compare_exchange_strong(id, id + 1)) break;
                    }
                    new_conn->set_conn_id(id);
                    new_conn->config_proxy_manager(proxy_manager);
                    new_conn->set_external_config(external_config);
                    new_conn->config_socks5_handler(socks5_proxy_handler);
                    new_conn->start();
#ifdef RPC_VERBOSE
                    FDEBUG("start connection {:p} -> {}", (void*)(new_conn.get()), id);
#endif
                }

                do_accept();
            });
    }

    void forward_msg(std::string key, std::string data) {
        std::unique_lock<std::mutex> lock(sub_mtx_);
        auto range = sub_map_.equal_range(key);
        if (range.first != range.second) {
            for (auto it = range.first; it != range.second; ++it) {
                auto conn = it->second.lock();
                if (conn == nullptr || conn->has_closed()) {
                    continue;
                }
                conn->publish(key, data);
            }
        } else {
            if (!sub_map_.empty())
                on_err(asio::error::make_error_code(asio::error::invalid_argument),
                       "The subscriber of the key: " + key + " does not exist.");
        }
    }
    network_data_reference reference_;
    std::atomic_bool has_started_ = false;
    tcp::acceptor acceptor_;
    std::size_t timeout_msec_ = 0;

    std::atomic<std::int64_t> conn_id_ = 0;

    std::unordered_multimap<std::string, std::weak_ptr<connection>> sub_map_;
    std::mutex sub_mtx_;

    router router_;
#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::context> ssl_context = nullptr;
    network_server_ssl_config ssl_config_;
#endif
    // proxy
    network::proxy::ProxyManager* proxy_manager = nullptr;
    rpc_server_external_config external_config;
    std::shared_ptr<SocksV5::Sock5Handler> socks5_proxy_handler = nullptr;
};
} // namespace rpc_service
  // namespace rpc_service
} // namespace network

#endif // REST_RPC_RPC_SERVER_H_
