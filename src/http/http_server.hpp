#pragma once

#include "http_connection.h"
#include "http_definitions.hpp"
#include "http_router.hpp"

#include "fundamental/basic/cxx_config_include.hpp"
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

#include "fundamental/events/event_system.h"
#include "network/network.hpp"

using asio::ip::tcp;

namespace network
{
namespace http
{

class http_server : private asio::noncopyable, public std::enable_shared_from_this<http_server> {
    friend class http_connection;

public:
    static const http_handler s_default_file_handler;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<http_server>(std::forward<Args>(args)...);
    }
    http_server(const http_server_config& config) :
    config_(config), acceptor_(io_context_pool::Instance().get_io_context()), router_(config.root_path) {
        protocal_helper::init_acceptor(acceptor_, config_.port);
    }
    ~http_server() {
        FDEBUG("release http_server");
    }

    void start() {
        bool expected_value = false;
        if (!has_started_.compare_exchange_strong(expected_value, true)) return;
        FINFO("start http server on {}:{}", acceptor_.local_endpoint().address().to_string(),
              acceptor_.local_endpoint().port());
        do_accept();
    }

    void stop() {
        release_obj();
    }
    // Regist request pattern with handler function and method mask.
    // Example like: RegistHandler("/createtask",[](std::shared_ptr<http_connection> conn, http_response&
    // response,http_request& request){}, MethodFilter::HttpGet | MethodFilter::HttpPost) when you want handle response
    // asynchronously,you should reserve conn and call response.perform_response() when you finish fill the response
    // normally,we should set header first,then set body size by set_body_size and then append the body
    void register_handler(const std::string& pattern, http_handler handler, std::uint32_t methodMask) {
        router_.update_route_table(pattern, handler, methodMask);
    }

    void enable_default_handler(http_handler handler     = s_default_file_handler,
                                std::uint32_t methodMask = MethodFilter::HttpAll);

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
            throw std::invalid_argument("http_server/ssl need valid certificate and key file");
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
            FDEBUG("load ssl config ca:{} key:{} crt:{} dh:{}", ssl_config_.ca_certificate_path,
                   ssl_config_.private_key_path, ssl_config_.certificate_path, ssl_config_.tmp_dh_path);
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
                    auto new_conn =
                        http_connection::make_shared(std::move(socket), router_, config_.head_case_sensitive,
                                                     config_.timeout_msec, weak_from_this());
                    auto release_handle = reference_.notify_release.Connect([con = new_conn->weak_from_this()]() {
                        auto ptr = con.lock();
                        if (ptr) ptr->release_obj();
                    });
                    // unbind
                    new_conn->reference_.notify_release.Connect([release_handle, s = weak_from_this(), this]() {
                        auto ptr = s.lock();
                        if (ptr) reference_.notify_release.DisConnect(release_handle);
                    });
#ifndef NETWORK_DISABLE_SSL
                    if (ssl_context) {
                        new_conn->enable_ssl(*ssl_context, ssl_config_.enable_no_ssl);
                    }
#endif
                    new_conn->start();
                    FDEBUG("start http_connection {:p}", (void*)(new_conn.get()));
                }

                do_accept();
            });
    }
    http_server_config config_;
    network_data_reference reference_;
    std::atomic_bool has_started_ = false;
    tcp::acceptor acceptor_;

    http_router router_;
#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::context> ssl_context = nullptr;
    network_server_ssl_config ssl_config_;
#endif
};
} // namespace http
  // namespace http
} // namespace network