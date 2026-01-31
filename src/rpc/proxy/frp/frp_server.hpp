#pragma once

#include "frp_server_connection.hpp"
#include "rpc/proxy/rpc_forward_connection.hpp"

namespace network
{
namespace proxy
{
using ::asio::ip::tcp;
class frp_signaling_server : private asio::noncopyable, public std::enable_shared_from_this<frp_signaling_server> {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_signaling_server>(std::forward<Args>(args)...);
    }
    template <typename port_type>
    frp_signaling_server(port_type port, std::set<std::uint16_t> enabled_ports) :
    acceptor_(io_context_pool::Instance().get_io_context()), enabled_ports(enabled_ports) {
        protocal_helper::init_acceptor(acceptor_, static_cast<std::uint16_t>(port));
    }
    ~frp_signaling_server() {
    }

    void start() {
        bool expected_value = false;
        if (!has_started_.compare_exchange_strong(expected_value, true)) return;
        FINFO("start frp signaling server on {}:{}", acceptor_.local_endpoint().address().to_string(),
              acceptor_.local_endpoint().port());
        do_accept();
    }

    void stop() {
        release_obj();
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
            throw std::invalid_argument("frp_signaling_server/ssl need valid certificate and key file");
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
        } catch (const std::exception& e) {
            throw std::invalid_argument(std::string("load ssl config failed ") + e.what());
        }

#endif
    }

private:
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
                    auto new_conn = frp_basic_connection::make_shared(
                        std::move(socket), [this, ref = weak_from_this()](std::uint16_t port) -> bool {
                            auto strong = ref.lock();
                            if (!strong) return false;
                            return enabled_ports.count(port) > 0;
                        });

#ifndef NETWORK_DISABLE_SSL
                    if (ssl_context) {
                        new_conn->enable_ssl(*ssl_context);
                    }
#endif
                    new_conn->notify_connection_ready.Connect(
                        shared_from_this(), [this](std::shared_ptr<frp_basic_connection> new_session_connection) {
                            if (!new_session_connection) return;
                            auto port = new_session_connection->get_proxy_port();
                            if (enabled_ports.count(port) == 0) return;
                            std::scoped_lock<std::mutex> locker(frp_proxy_server_mutex);
                            auto& session         = proxy_servers[port];
                            auto session_instance = session.lock();
                            if (!session_instance) {
                                try {
                                    session_instance =
                                        frp_port_proxy_session::make_shared(port, new_session_connection->get_frp_id());
                                    session = session_instance;
                                } catch (const std::exception& e) {
                                    FERR("init port listener:{} failed e:{}", port, e.what());
                                    return;
                                } catch (...) {
                                    return;
                                }
                            } else {
                                auto id1 = session_instance->get_frp_id();
                                auto id2 = new_session_connection->get_frp_id();
                                if (id1 != id2) {
                                    FERR("ignore conflicted frp request id1:{} id2:{} on port:{}", id1, id2, port);
                                    return;
                                }
                            }
                            session_instance->push_new_session(std::move(new_session_connection));
                        });
                    new_conn->start();
                }

                do_accept();
            });
    }

    network_data_reference reference_;
    std::atomic_bool has_started_ = false;
    tcp::acceptor acceptor_;
#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::context> ssl_context = nullptr;
    network_server_ssl_config ssl_config_;
#endif
    const std::set<std::uint16_t> enabled_ports;
    std::mutex frp_proxy_server_mutex;
    std::map<std::uint16_t, std::weak_ptr<frp_port_proxy_session>> proxy_servers;
};
} // namespace proxy
} // namespace network