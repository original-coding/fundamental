#include "ws_forward_connection.hpp"
#include "fundamental/basic/string_utils.hpp"
#include "fundamental/basic/utils.hpp"
#include "rpc/connection.h"
namespace network
{
namespace proxy
{
websocket_forward_connection::websocket_forward_connection(std::shared_ptr<rpc_service::connection> ref_connection,
                                                           route_query_function query_func,
                                                           std::string pre_read_data) :
rpc_forward_connection(ref_connection, std::move(pre_read_data)), route_query_f(query_func) {
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
    forward_async_buffer_read_some(client2server.GetReadBuffer(),
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
    websocket::http_handler_context response_context;
    auto response_data    = std::make_shared<std::string>();
    bool finished_success = false;
    Fundamental::ScopeGuard response_guard([&]() {
        FDEBUG("ws forward:response \n{}", *response_data);
        //
        forward_async_write_buffers(asio::const_buffer { response_data->data(), response_data->size() },
                                    [this, self = shared_from_this(), response_data,
                                     finished_success](std::error_code ec, std::size_t bytesRead) {
                                        if (!reference_.is_valid()) {
                                            return;
                                        }
                                        if (!finished_success) return;
                                        StartProtocal();
                                    });
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
        if (route_query_f) {
            auto [has_found, query_host, query_service, guard] = route_query_f(parse_context.head2);
            if (has_found) {
                proxy_host    = query_host;
                proxy_service = query_service;
                release_gurad = std::move(guard);
            }
        }
        if (proxy_host.empty() || proxy_service.empty()) {
            goto HAS_ANY_PROTOCAL_ERROR;
        }
        FINFO("ws_forward {} to {} {}", parse_context.head2, proxy_host, proxy_service);
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
} // namespace proxy
} // namespace network