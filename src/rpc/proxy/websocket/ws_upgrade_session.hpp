#pragma once
#include "fundamental/basic/log.h"
#include "fundamental/basic/url_utils.hpp"
#include "network/upgrade_interface.hpp"
#include "ws_common.hpp"
#include <array>

namespace network
{
namespace proxy
{
class ws_upgrade_imp : public network::network_upgrade_interface {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<ws_upgrade_imp>(std::forward<Args>(args)...);
    }
    ws_upgrade_imp(const std::string api, const std::string& remote_host, std::size_t depth = 0) :
    remote_host(remote_host), api(Fundamental::CorrectApiSlash(api)), depth(depth) {
    }
    const char* interface_name() const override {
        return "ws_upgrade";
    }
    void abort_all_operation() override {
        if (abort_cb_) abort_cb_();
    }
    void start() override {
        FASSERT(read_cb_ && write_cb_ && finish_cb_, "call proxy init first");
        // check user info
        if (remote_host.empty() || api.empty()) {
            finish_cb_(std::make_error_code(std::errc::invalid_argument), "invalid ws proxy infomation");
            return;
        }
        greeting();
    }

protected:
    void greeting() {
        network::websocket::http_handler_context context;
        context.head1 = context.kWebsocketMethod;
        context.head2 = api;
        if (depth > 0) {
            context.head2 += std::string("?") + network::websocket::http_handler_context::kWsForwardDepthFlagName +
                             "=" + std::to_string(depth);
        }
        context.head3 = context.kHttpVersion;
        context.headers.emplace(context.kHttpHost, remote_host);
        context.headers.emplace(context.kHttpUpgradeStr, context.kHttpWebsocketStr);
        context.headers.emplace(context.kHttpConnection, context.kHttpUpgradeValueStr);
        context.headers.emplace(context.kWebsocketRequestKey, network::websocket::ws_utils::generateSessionKey(this));
        context.headers.emplace(context.kWebsocketRequestVersion, context.kWebsocketVersion);
        auto request_str = std::make_shared<std::string>(context.encode());
        network::write_buffer_t write_buffers;
        using value_type = network::write_buffer_t::value_type;
        write_buffers.emplace_back(value_type { request_str->data(), request_str->size() });
        write_cb_(std::move(write_buffers),
                  [this, ptr = shared_from_this(), request_str](std::error_code ec, const std::string& msg) {
                      // failed
                      if (ec.value() != kSuccessOpCode) {
                          finish_cb_(ec, msg);
                          return;
                      }
                      handle_greeting_response();
                  });
    }

    void handle_greeting_response() {
        // ver auth_method

        network::read_buffer_t read_buffers;
        using value_type = network::read_buffer_t::value_type;
        read_buffers.emplace_back(value_type { cache_buffer.data(), cache_buffer.size() });
        read_cb_(std::move(read_buffers), [this, ptr = shared_from_this()](std::error_code ec, const std::string& msg) {
            // failed
            if (ec.value() != kSuccessOpCode) {
                finish_cb_(ec, msg);
                return;
            }
            auto [status, read_size] = http_response_context.parse(cache_buffer.data(), cache_buffer.size());
            switch (status) {
            case websocket::http_handler_context::parse_need_more_data: {
                handle_greeting_response();
            } break;
            case websocket::http_handler_context::parse_success: {
                handle_ws_handshake_success();

            } break;
            default: {
                FDEBUG("invalid http response {}", http_response_context.parse_cache);
                finish_cb_(std::make_error_code(std::errc::bad_message), "invalid http reponse");
            } break;
            }
        });
    }
    void handle_ws_handshake_success() {
        FDEBUG("finish http response {}", http_response_context.parse_cache);

        std::error_code ec;
        std::string msg = "success";
        do {
            if (http_response_context.head2 != http_response_context.kWebsocketSuccessCode) {
                ec  = std::make_error_code(std::errc::invalid_argument);
                msg = Fundamental::StringFormat("ws handshake failed {} {}", http_response_context.head2,
                                                http_response_context.head3);
                break;
            }
            auto iter = http_response_context.headers.find(http_response_context.kWebsocketResponseAccept);
            if (iter == http_response_context.headers.end()) {
                ec  = std::make_error_code(std::errc::bad_message);
                msg = Fundamental::StringFormat("invalid http response  miss {} header",
                                                http_response_context.kWebsocketResponseAccept);
                break;
            }
            auto verify_str =
                websocket::ws_utils::generateServerAcceptKey(network::websocket::ws_utils::generateSessionKey(this));
            if (verify_str != iter->second) {
                ec  = std::make_error_code(std::errc::permission_denied);
                msg = Fundamental::StringFormat("ws verify failed recv: {} != need:{}", iter->second, verify_str);
                break;
            }
        } while (0);
        finish_cb_(ec, msg);
    }

private:
    const std::string remote_host;
    const std::string api;
    std::size_t depth = 0;
    websocket::http_handler_context http_response_context;
    std::array<std::uint8_t, 1> cache_buffer;
};
} // namespace proxy
} // namespace network