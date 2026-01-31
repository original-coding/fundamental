#include "protocal_pipe_connection.hpp"
#include "fundamental/basic/log.h"
#include "rpc/proxy/socks5/socks5_proxy_session.hpp"
#include "rpc/proxy/websocket/ws_upgrade_session.hpp"
namespace network
{
namespace proxy
{
protocal_pipe_connection::protocal_pipe_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                                   rpc_client_forward_config forward_config,
                                                   std::string pre_read_data) :
rpc_forward_connection(ref_connection, ""), forward_config_(forward_config) {

    if (pre_read_data.size() > 0) {
        read_cache.resize(pre_read_data.size());
        std::memcpy(read_cache.data(), pre_read_data.data(), pre_read_data.size());
    }
}
void protocal_pipe_connection::process_protocal() {
    auto [current_status, ret_len] = request_context.decode(read_cache.data(), read_cache.size());
    do {
        if (current_status == forward::forward_parse_failed) break;
        if (current_status == forward::forward_parse_success) {
            if (ret_len != read_cache.size()) break;
            start_pipe_proxy_handler();
        } else {
            read_cache.resize(ret_len);
            read_more_data();
        }
        return;
    } while (0);
    release_obj();
}

void protocal_pipe_connection::HandleConnectSuccess() {
    if (need_socks5_proxy) {
        process_socks5_proxy();
    } else {
        handle_tls();
    }
}

void protocal_pipe_connection::StartProtocal() {
    StartDnsResolve(proxy_host, proxy_service);
}

void protocal_pipe_connection::StartForward() {
    process_pipe_handshake();
}

void protocal_pipe_connection::StartRawForward() {
    rpc_forward_connection::StartForward();
}

void protocal_pipe_connection::read_more_data() {
    forward_async_buffers_read({ network_read_buffer_t { read_cache.data(), read_cache.size() } },
                               [this, self = shared_from_this()](std::error_code ec, std::size_t) {
                                   if (!reference_.is_valid()) {
                                       return;
                                   }
                                   if (ec) {
                                       release_obj();
                                       return;
                                   }
                                   process_protocal();
                               });
}

void protocal_pipe_connection::start_pipe_proxy_handler() {
    forward::forward_response_context response_context;
    response_context.code = 1;
    response_context.msg  = "failed";

    Fundamental::ScopeGuard response_guard([&]() {
        if (response_context.code != response_context.kSuccessCode) {
            FWARN("fail to setup pipe {}", response_context.msg);
        }
        //
        auto [enc_success, enc_ret] = response_context.encode();
        if (!enc_success) {
            FDEBUG("enc response failed");
            return;
        }
        auto need_more_phase = response_context.code == response_context.kSuccessCode;
        // The response indicating a successful proxy will not be sent until after the connection is successfully
        // established.
        if (!need_forward_phase() || !need_more_phase) {
            auto response_data = std::make_shared<std::string>(enc_ret);
            forward_async_write_buffers({ network_write_buffer_t { response_data->data(), response_data->size() } },
                                        [this, self = shared_from_this(), response_data,
                                         need_more_phase](std::error_code ec, std::size_t bytesRead) {
                                            if (!reference_.is_valid() || !need_more_phase || ec) {
                                                return;
                                            }
                                            if (need_fallback()) {
                                                handle_fallback();
                                            }
                                        });
        } else {
            StartProtocal();
        }
    });

    do {
        if (request_context.dst_host.empty() || request_context.dst_service.empty()) {
            response_context.msg = "invalid forward host information";
            break;
        }
        if (!verify_protocal_request_param(response_context)) {
            break;
        }
        if (!handle_none_forward_phase_protocal(response_context)) {
            break;
        }
        proxy_host    = request_context.dst_host;
        proxy_service = request_context.dst_service;
        FDEBUG("pipe proxy to {} {}", proxy_host, proxy_service);
        if (request_context.socks5_option == forward::forward_option::forward_required_option &&
            (forward_config_.socks5_proxy_host.empty() || forward_config_.socks5_proxy_port.empty())) {
            response_context.msg = "no socks5 proxy set";
            break;
        }
        need_socks5_proxy = (request_context.socks5_option != forward::forward_option::forward_disable_option) &&
                            !forward_config_.socks5_proxy_host.empty() && !forward_config_.socks5_proxy_port.empty();
        if (need_socks5_proxy) {
            proxy_host    = forward_config_.socks5_proxy_host;
            proxy_service = forward_config_.socks5_proxy_port;
        }
        if (request_context.ssl_option == forward::forward_option::forward_required_option &&
            forward_config_.ssl_config.disable_ssl) {
            response_context.msg = "no ssl proxy enabled";
            break;
        }

        response_context.code = response_context.kSuccessCode;
        response_context.msg  = "success";
    } while (0);
}
bool protocal_pipe_connection::verify_protocal_request_param(forward::forward_response_context& response_context) {
    switch (request_context.forward_protocal) {
    case forward::forward_websocket: {
        if (request_context.route_path.empty()) {
            response_context.msg = "websocker forward need a valid route path";
            return false;
        }
        break;
    }
    case forward::forward_add_server: {
        if (request_context.route_path.empty()) {
            response_context.msg = "add server need a valid route path";
            return false;
        }
        break;
    }
    default: break;
    }
    return true;
}

bool protocal_pipe_connection::handle_none_forward_phase_protocal(forward::forward_response_context& response_context) {
    switch (request_context.forward_protocal) {
    case forward::forward_add_server: {
        if (!add_server_handler) {
            response_context.msg = "no add server handlers has been set";
            return false;
        }
        auto [ret, reason] =
            add_server_handler(request_context.route_path, request_context.dst_host, request_context.dst_service);
        if (!ret) {
            response_context.msg = std::string("add server failed for reason:") + reason;
            return false;
        }
        FINFO("add server success {} -> {}:{}", request_context.route_path, request_context.dst_host,
              request_context.dst_service);
        break;
    }
    default: break;
    }
    return true;
}

bool protocal_pipe_connection::need_forward_phase() const {
    switch (request_context.forward_protocal) {
    case forward::forward_protocal_num:
    case forward::forward_add_server: {
        return false;
    }
    default: return true;
    }
}
bool protocal_pipe_connection::need_fallback() const {
    switch (request_context.forward_protocal) {
    case forward::forward_add_server: {
        return true;
    }
    default: return false;
    }
}

void protocal_pipe_connection::handle_fallback() {
    FallBackProtocal();
}

void protocal_pipe_connection::process_socks5_proxy() {
    std::uint16_t port = 0;
    try {
        port = static_cast<std::uint16_t>(std::stoul(request_context.dst_service));
    } catch (...) {
        FERR("invalud socks5 dst service {}", request_context.dst_service);
        return;
    }

    auto socks5_proxy = SocksV5::socks5_proxy_imp::make_shared(
        request_context.dst_host, port, forward_config_.socks5_username, forward_config_.socks5_proxy_port);
    auto write_callback = [this,
                           ptr = shared_from_this()](write_buffer_t write_buffers,
                                                     const network_upgrade_interface::operation_cb& finish_cb) mutable {
        std::vector<network_write_buffer_t> buffers;
        for (auto& buffer : write_buffers) {
            buffers.emplace_back(network_write_buffer_t { buffer.data, buffer.len });
        }
        downstream_async_write_buffers(
            std::move(buffers), [this, ptr = shared_from_this(), finish_cb](std::error_code ec, std::size_t) {
                if (!reference_.is_valid()) {
                    finish_cb(std::make_error_code(std::errc::operation_canceled), "aborted");
                    return;
                }
                finish_cb(ec, "");
            });
    };
    auto read_callback = [this,
                          ptr = shared_from_this()](read_buffer_t read_buffers,
                                                    const network_upgrade_interface::operation_cb& finish_cb) mutable {
        std::vector<network_read_buffer_t> buffers;
        for (auto& buffer : read_buffers) {
            buffers.emplace_back(network_read_buffer_t { buffer.data, buffer.len });
        }
        downstream_async_buffer_read(std::move(buffers),
                                     [this, ptr = shared_from_this(), finish_cb](const asio::error_code& ec, size_t) {
                                         if (!reference_.is_valid()) {
                                             finish_cb(std::make_error_code(std::errc::operation_canceled), "aborted");
                                             return;
                                         }
                                         finish_cb(ec, "");
                                     });
    };
    auto finish_callback = [this, ptr = shared_from_this()](std::error_code ec, const std::string& msg) {
        do {
            if (!reference_.is_valid()) {
                if (!ec) ec = std::make_error_code(std::errc::bad_file_descriptor);
                break;
            }
            FDEBUG("pipe socks5 proxy ec:{}({}) msg:{}", ec.value(), ec.message(), msg);
            if (ec) break;
            handle_tls();
            return;
        } while (0);
    };
    socks5_proxy->init(read_callback, write_callback, finish_callback, nullptr);
    socks5_proxy->start();
}
void protocal_pipe_connection::handle_tls() {
    if (request_context.ssl_option != forward::forward_disable_option) {
        enable_ssl(forward_config_.ssl_config);
    }
    if (proxy_by_ssl()) {
        ssl_handshake();
    } else {
        StartForward();
    }
}

void protocal_pipe_connection::process_pipe_handshake() {
    switch (request_context.forward_protocal) {
    case forward::forward_websocket: {
        process_ws_proxy();
    } break;
    case forward::forward_raw: {
        do_pipe_proxy();
    } break;
    default: {
        FERR("pipe proxy protocal {} is coming soon",
             forward::forward_context_interface::kForwardProtocalArray[request_context.forward_protocal]);
    } break;
    }
}

void protocal_pipe_connection::process_ws_proxy() {
    // send ws request first
    FINFO("protpcal pipe client request ws_forward {}", request_context.route_path);
    auto ws_upgrade     = proxy::ws_upgrade_imp::make_shared(request_context.route_path, request_context.dst_host);
    auto write_callback = [this,
                           ptr = shared_from_this()](write_buffer_t write_buffers,
                                                     const network_upgrade_interface::operation_cb& finish_cb) mutable {
        std::vector<network_write_buffer_t> buffers;
        for (auto& buffer : write_buffers) {
            buffers.emplace_back(network_write_buffer_t { buffer.data, buffer.len });
        }
        downstream_async_write_buffers(
            std::move(buffers), [this, ptr = shared_from_this(), finish_cb](std::error_code ec, std::size_t) {
                if (!reference_.is_valid()) {
                    finish_cb(std::make_error_code(std::errc::operation_canceled), "aborted");
                    return;
                }
                finish_cb(ec, "write failed");
            });
    };
    auto read_callback = [this,
                          ptr = shared_from_this()](read_buffer_t read_buffers,
                                                    const network_upgrade_interface::operation_cb& finish_cb) mutable {
        std::vector<network_read_buffer_t> buffers;
        for (auto& buffer : read_buffers) {
            buffers.emplace_back(network_read_buffer_t { buffer.data, buffer.len });
        }
        downstream_async_buffer_read(std::move(buffers),
                                     [this, ptr = shared_from_this(), finish_cb](const asio::error_code& ec, size_t) {
                                         if (!reference_.is_valid()) {
                                             finish_cb(std::make_error_code(std::errc::operation_canceled), "aborted");
                                             return;
                                         }
                                         finish_cb(ec, "read failed");
                                     });
    };
    auto finish_callback = [this, ptr = shared_from_this()](std::error_code ec, const std::string& msg) {
        do {
            if (!reference_.is_valid()) {
                if (!ec) ec = std::make_error_code(std::errc::bad_file_descriptor);
                break;
            }
            FDEBUG("pipe ws proxy finish ec:{}({}) msg:{}", ec.value(), ec.message(), msg);
            if (ec) break;
            do_pipe_proxy();
            return;
        } while (0);
    };
    ws_upgrade->init(read_callback, write_callback, finish_callback, nullptr);
    ws_upgrade->start();
}

void protocal_pipe_connection::do_pipe_proxy() {
    forward::forward_response_context response_context;
    response_context.code = response_context.kSuccessCode;
    response_context.msg  = "start pipe proxy";

    auto [enc_success, enc_ret] = response_context.encode();
    if (!enc_success) {
        FDEBUG("enc response failed");
        return;
    }
    auto response_data = std::make_shared<std::string>(enc_ret);
    forward_async_write_buffers(
        { network_write_buffer_t { response_data->data(), response_data->size() } },
        [this, self = shared_from_this(), response_data](std::error_code ec, std::size_t bytesRead) {
            if (!reference_.is_valid() || ec) {
                FWARN("broken pipe connection");
                return;
            }
            StartClientRead();
            rpc_forward_connection::StartForward();
        });
}
} // namespace proxy
} // namespace network