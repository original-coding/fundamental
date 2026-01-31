#pragma once
#include "http_router.hpp"

#include <any>
#include <array>
#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <unordered_set>

#include <network/network.hpp>

#include "fundamental/basic/log.h"
#include "fundamental/events/event_system.h"
#include "http_request.hpp"
#include "http_response.hpp"

using asio::ip::tcp;

namespace network
{
namespace http
{

class http_server;

class http_connection : public std::enable_shared_from_this<http_connection>, private asio::noncopyable {
    friend class http_server;
    friend class http_response;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<http_connection>(std::forward<Args>(args)...);
    }
    http_connection(tcp::socket socket,
                    http_router& router,
                    bool header_case_sensitive,
                    std::size_t timeout_msec,
                    std::weak_ptr<http_server> server_wref) :
    router_(router), server_wref_(server_wref), socket_(std::move(socket)),
    timeout_check_timer_(socket_.get_executor()), timeout_msec_(timeout_msec), request_(header_case_sensitive),
    response_(*this) {
    }
    ~http_connection() {
        FDEBUG("release http_connection {:p}", (void*)this);
    }

    void start() {
        request_.ip_   = socket_.remote_endpoint().address().to_string();
        request_.port_ = socket_.remote_endpoint().port();
        if (is_ssl()) {
            ssl_handshake();
        } else {
            handle_read();
        }
    }

#ifndef NETWORK_DISABLE_SSL
    void enable_ssl(asio::ssl::context& ssl_context, bool enable_no_ssl) {
        ssl_context_ref = &ssl_context;
        enable_no_ssl_  = enable_no_ssl;
    }
#endif
    void release_obj() {
        reference_.release();
        asio::post(socket_.get_executor(), [this, ref = shared_from_this()] { close(); });
    }

    bool has_closed() const {
        return !reference_.is_valid();
    }

    decltype(auto) get_root_path() const {
        return router_.root_path();
    }
    http_request& get_request() {
        return request_;
    }
    http_response& get_response() {
        return response_;
    }

private:
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
                                             FDEBUG("client perform ssl handshake failed {}", error.message());
                                             release_obj();
                                             return;
                                         }

                                         handle_read();
                                     });
#endif
    }
    void ssl_handshake() {
        auto self(shared_from_this());
        asio::async_read(socket_, network_read_buffer_t(request_.buffer_.data(), kSslPreReadSize),
                         [this, self](asio::error_code ec, std::size_t length) {
                             if (!reference_.is_valid()) {
                                 return;
                             }
                             if (!socket_.is_open()) {
                                 return;
                             }
                             do {
                                 if (ec) {
                                     FDEBUG("ssl handshake read failed {}", ec.message());
                                     break;
                                 }
                                 const std::uint8_t* p_data = (const std::uint8_t*)request_.buffer_.data();
                                 if (p_data[0] == 0x16) // ssl Handshake
                                 {
                                     if (p_data[1] != 0x03 || p_data[2] > 0x03) {
                                         break;
                                     }
                                     do_ssl_handshake(request_.buffer_.data(), kSslPreReadSize);
                                 } else {
                                     if (!enable_no_ssl_) {
                                     }
                                     FDEBUG("[http] WARNNING!!! falling  down to no ssl socket");
                                     ssl_context_ref = nullptr;
                                     handle_read(kSslPreReadSize);
                                 }
                                 return;
                             } while (0);
                             release_obj();
                         });
    }

    bool is_ssl() const {
#ifndef NETWORK_DISABLE_SSL
        return ssl_context_ref != nullptr;
#else
        return false;
#endif
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
    void reset_timer() {
        if (timeout_msec_ == 0) {
            return;
        }

        auto self(shared_from_this());
        timeout_check_timer_.expires_after(std::chrono::milliseconds(timeout_msec_));
        timeout_check_timer_.async_wait([this, self](const asio::error_code& ec) {
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
    void process_http_request() {
        auto method = request_.get_method();
        auto& h     = router_.get_table(request_.get_pattern());
        if (!h.handler) {
            response_.stock_response(http_response::not_found);
        } else if (!(h.method_mask & method)) {
            response_.stock_response(http_response::not_implemented);

        } else {
            response_.prepare();
            h.handler(shared_from_this(), response_, request_);
        }
        response_.perform_response();
    }
    void process_bad_request() {
        response_.stock_response(http_response::bad_request);
        response_.perform_response();
    }
    void handle_read(std::size_t offset = 0) {
        auto self(shared_from_this());
        async_buffers_read_some(
            { network_read_buffer_t(request_.buffer_.data() + offset, request_.buffer_.size() - offset) },
            [this, offset, self](asio::error_code ec, std::size_t length) {
                if (!reference_.is_valid()) {
                    return;
                }
                if (ec) {
                    release_obj();
                    return;
                }
                // reset timeout
                b_waiting_process_any_data.exchange(false);
                std::optional<bool> result;
                std::tie(result, std::ignore) =
                    request_.Parse(request_.buffer_.data(), request_.buffer_.data() + length + offset, length + offset);

                auto resultHasVal = result.has_value();
                // all data finished
                if (resultHasVal && result.value()) {
                    process_http_request();
                } else if (resultHasVal && !result.value()) {
                    process_bad_request();
                } else {
                    // read more data
                    handle_read();
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
        cancel_timer();
#ifndef NETWORK_DISABLE_SSL
        if (ssl_stream_) {
            ::asio::dispatch(ssl_stream_->get_executor(), [this, ref = shared_from_this()]() {
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

    http_router& router_;
    network_data_reference reference_;
    std::weak_ptr<http_server> server_wref_;
    tcp::socket socket_;

    std::atomic_bool b_waiting_process_any_data = false;
    asio::steady_timer timeout_check_timer_;
    std::size_t timeout_msec_;

    bool delay_ = false;
#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_ = nullptr;
    asio::ssl::context* ssl_context_ref                                    = nullptr;
    bool enable_no_ssl_                                                    = true;
#endif
    // http data
    http_request request_;
    http_response response_;
};

} // namespace http
} // namespace network
