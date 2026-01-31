#pragma once
#include "forward_pipe_codec.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/url_utils.hpp"
#include "network/upgrade_interface.hpp"
#include <array>

namespace network
{
namespace proxy
{
class pipe_connection_upgrade : public network::network_upgrade_interface {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<pipe_connection_upgrade>(std::forward<Args>(args)...);
    }
    pipe_connection_upgrade(forward::forward_request_context request) : request(request) {
    }
    const char* interface_name() const override {
        return "pipe_connection_upgrade";
    }
    void abort_all_operation() override {
        if (abort_cb_) abort_cb_();
    }
    void start() override {
        FASSERT(read_cb_ && write_cb_ && finish_cb_, "call proxy init first");
        // check user info
        if (request.dst_host.empty() || request.dst_service.empty() || request.route_path.empty()) {
            finish_cb_(std::make_error_code(std::errc::invalid_argument), "invalid pipe upgrade infomation");
            return;
        }
        greeting();
    }

protected:
    void greeting() {
        auto [b_success, encode_result] = request.encode();
        if (!b_success) {
            finish_cb_(std::make_error_code(std::errc::invalid_argument), "invalid pipe upgrade request");
            return;
        }
        auto request_str = std::make_shared<std::string>(encode_result);
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
        auto [status, len] = response.decode(read_buffer.data(), read_buffer.size());
        switch (status) {
        case forward::forward_parse_status::forward_parse_need_more_data: {
            read_buffer.resize(len);
            network::read_buffer_t read_buffers;
            using value_type = network::read_buffer_t::value_type;
            read_buffers.emplace_back(value_type { read_buffer.data(), read_buffer.size() });
            read_cb_(std::move(read_buffers),
                     [this, ptr = shared_from_this()](std::error_code ec, const std::string& msg) {
                         // failed
                         if (ec.value() != kSuccessOpCode) {
                             finish_cb_(ec, msg);
                             return;
                         }
                         handle_greeting_response();
                     });
        } break;
        case forward::forward_parse_status::forward_parse_success: handshake_success(); break;
        default: {
            finish_cb_(std::make_error_code(std::errc::bad_message), "invalid reponse");
        } break;
        }
    }
    void handshake_success() {
        FDEBUG("finish {}: response {} {}", interface_name(), response.code, response.msg);

        std::error_code ec;
        std::string msg = response.msg;
        do {
            if (response.code != response.kSuccessCode) {
                ec  = std::make_error_code(std::errc::invalid_argument);
                msg = Fundamental::StringFormat("{} handshake failed {} {}", interface_name(), response.code,
                                                response.msg);
                break;
            }
        } while (0);
        finish_cb_(ec, msg);
    }

private:
    forward::forward_request_context request;
    std::vector<std::uint8_t> read_buffer;
    forward::forward_response_context response;
};
} // namespace proxy
} // namespace network