#include "ws_forward_connection.hpp"
#include "fundamental/basic/string_utils.hpp"
#include "fundamental/basic/utils.hpp"
#include "rpc/connection.h"
#include "rpc/proxy/websocket/ws_upgrade_session.hpp"

namespace network
{
namespace proxy
{
websocket_forward_connection::websocket_forward_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                                           route_query_function query_func,
                                                           std::string pre_read_data) :
rpc_forward_connection(ref_connection, std::move(pre_read_data)), route_query_f(query_func) {
}

void websocket_forward_connection::StartProtocal() {
    StartDnsResolve(proxy_host, proxy_service);
}

void websocket_forward_connection::StartForward() {
    // now every thing is ok
    switch (host_type) {
    case network::proxy::proxy_host_type::ws_forward_proxy: start_ws_proxy_to_next_layer(); break;
    default: break;
    }
}

void websocket_forward_connection::HandleConnectSuccess() {

    switch (host_type) {
    case network::proxy::proxy_host_type::raw_tcp_proxy: do_pipe_proxy(); break;
    default: rpc_forward_connection::HandleConnectSuccess(); break;
    }
}

void websocket_forward_connection::process_protocal() {
    auto read_buffer                = client2server.GetWriteBuffer();
    auto [status_current, peek_len] = parse_context.parse(read_buffer.data(), read_buffer.size());
    do {
        if (status_current == websocket::http_handler_context::parse_status::parse_failed) break;
        client2server.UpdateWriteBuffer(peek_len);
        if (status_current == websocket::http_handler_context::parse_status::parse_success) {
            start_ws_proxy();
        } else {
            read_more_data();
        }
        return;
    } while (0);
    release_obj();
}

void websocket_forward_connection::read_more_data() {
    client2server.PrepareReadCache();
    forward_async_buffers_read_some({ client2server.GetReadBuffer() },
                                    [this, self = shared_from_this()](std::error_code ec, std::size_t bytesRead) {
                                        if (!reference_.is_valid()) {
                                            return;
                                        }
                                        client2server.UpdateReadBuffer(bytesRead);
                                        if (ec) {
                                            release_obj();
                                            return;
                                        }
                                        process_protocal();
                                    });
}

void websocket_forward_connection::start_ws_proxy() {

    auto response_data    = std::make_shared<std::string>();
    bool finished_success = false;
    Fundamental::ScopeGuard response_guard([&]() {
        if (!finished_success) {
            FDEBUG("ws forward:response \n{}", *response_data);
            //
            forward_async_write_buffers({ network_write_buffer_t { response_data->data(), response_data->size() } },
                                        [this, self = shared_from_this(), response_data,
                                         finished_success](std::error_code ec, std::size_t bytesRead) {
                                            if (!reference_.is_valid()) {
                                                return;
                                            }
                                            if (!finished_success) return;
                                        });
        } else {
            StartProtocal();
        }
    });

    do {
        // check head
        if (parse_context.head1 != response_context.kWebsocketMethod ||
            parse_context.head3 != response_context.kHttpVersion) {
            goto HAS_ANY_PROTOCAL_ERROR;
        }
        // check upgrade
        {
            auto iter = parse_context.headers.find(parse_context.kHttpUpgradeStr);
            if (iter == parse_context.headers.end() ||
                (Fundamental::StringToLower(iter->second), iter->second != parse_context.kHttpWebsocketStr)) {
                goto HAS_ANY_PROTOCAL_ERROR;
            }
        }
        // check connection
        {
            auto iter = parse_context.headers.find(parse_context.kHttpConnection);
            if (iter == parse_context.headers.end() ||
                (Fundamental::StringToLower(iter->second), iter->second != parse_context.kHttpUpgradeValueStr)) {
                goto HAS_ANY_PROTOCAL_ERROR;
            }
        }
        // check ws version
        {
            auto iter = parse_context.headers.find(parse_context.kWebsocketRequestVersion);
            if (iter == parse_context.headers.end() || iter->second != parse_context.kWebsocketVersion) {
                goto HAS_ANY_PROTOCAL_ERROR;
            }
        }
        std::string ws_key;
        {
            auto iter = parse_context.headers.find(parse_context.kWebsocketRequestKey);
            if (iter == parse_context.headers.end()) {
                goto HAS_ANY_PROTOCAL_ERROR;
            }
            ws_key = iter->second;
        }
        {

            auto [uri, query_params] = parse_context.parse_query_params(parse_context.head2);
            request_uri              = uri;
            auto iter                = query_params.find(parse_context.kWsForwardDepthFlagName);
            if (iter != query_params.end()) {
                try {
                    forward_depth = std::stoul(iter->second);
                } catch (...) {
                }
            }
        }

        if (route_query_f) {
            auto [has_found, query_host, guard] = route_query_f(request_uri);
            if (has_found) {
                proxy_host    = query_host.host;
                proxy_service = query_host.service;
                host_type     = query_host.host_type;
                release_gurad = std::move(guard);
            }
        }
        if (proxy_host.empty() || proxy_service.empty()) {
            goto HAS_ANY_PROTOCAL_ERROR;
        }

        if (host_type == network::proxy::proxy_host_type::ws_forward_proxy) {
            if (forward_depth >= parse_context.kWsMaxForwardCntLimit) {
                FWARN("ws forward depth {} overflow max limit {},fallback to tcp proxy", forward_depth,
                      parse_context.kWsMaxForwardCntLimit);
                host_type = network::proxy::proxy_host_type::raw_tcp_proxy;
            }
        }
        FINFO("ws_forward {} to {} {} type:{} depth:{}", parse_context.head2, proxy_host, proxy_service, host_type,
              forward_depth);
        response_context.head1 = response_context.kHttpVersion;
        response_context.head2 = response_context.kWebsocketSuccessCode;
        response_context.head3 = response_context.kWebsocketSuccessStr;
        response_context.headers.emplace(response_context.kHttpUpgradeStr, response_context.kHttpWebsocketStr);
        response_context.headers.emplace(response_context.kHttpConnection, response_context.kHttpUpgradeValueStr);
        response_context.headers.emplace(response_context.kWebsocketResponseAccept,
                                         websocket::ws_utils::generateServerAcceptKey(ws_key));
        *response_data   = response_context.encode();
        finished_success = true;
        return;

    } while (0);
HAS_ANY_PROTOCAL_ERROR:
    *response_data = response_context.default_error_response();
}

void websocket_forward_connection::start_ws_proxy_to_next_layer() {

    // send ws request first
    ++forward_depth;
    FINFO("request ws_forward to next layer {} forward_depth:{}", parse_context.head2, forward_depth);
    auto ws_upgrade     = proxy::ws_upgrade_imp::make_shared(request_uri, proxy_host, forward_depth);
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
            FDEBUG("ws forward finish ec:{}({}) msg:{}", ec.value(), ec.message(), msg);
            if (ec) break;
            do_pipe_proxy();
            return;
        } while (0);
    };
    ws_upgrade->init(read_callback, write_callback, finish_callback, nullptr);
    ws_upgrade->start();
}
void websocket_forward_connection::do_pipe_proxy() {
    auto response_data = std::make_shared<std::string>();
    *response_data     = response_context.encode();
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