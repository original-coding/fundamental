#include "protocal_pipe_proxy_connection.hpp"
#include "fundamental/basic/log.h"
#include "pipe_connection_upgrade_session.hpp"
namespace network
{
namespace proxy
{
protocal_pipe_proxy_connection::protocal_pipe_proxy_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                                               std::string local_proxy_host,
                                                               std::string local_proxy_port,
                                                               std::string remote_host,
                                                               std::string remote_port,
                                                               std::string remote_api_path) :
ws_port_pipe_forward_connection(ref_connection, remote_host, remote_port, remote_api_path) {
    // set proxy host info
    proxy_host    = local_proxy_host;
    proxy_service = local_proxy_port;
    // set preotocal pipe proxy request
    request_context.forward_protocal = forward::forward_websocket;
    request_context.ssl_option       = forward::forward_optional_option;
    request_context.socks5_option    = forward::forward_optional_option;
}

void protocal_pipe_proxy_connection::process_pipe_handshake() {
    FINFO("try port protocal pipe proxy by {}:{} to {}:{}{}", proxy_host, proxy_service, request_context.dst_host,
          request_context.dst_service, request_context.route_path);
    auto pipe_upgrade   = proxy::pipe_connection_upgrade::make_shared(request_context);
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
                break;
            }
            if (ec) {
                FERR("port protocal pipe proxy by {}:{} to {}:{}{} failed:{}", proxy_host, proxy_service,
                     request_context.dst_host, request_context.dst_service, request_context.route_path,
                     Fundamental::error_code(ec));
                break;
            } else {
                FDEBUG("port protocal pipe proxy by {}:{} to {}:{}{} success", proxy_host, proxy_service,
                       request_context.dst_host, request_context.dst_service, request_context.route_path);
            }
            do_pipe_proxy();
            return;
        } while (0);
    };
    pipe_upgrade->init(read_callback, write_callback, finish_callback, nullptr);
    pipe_upgrade->start();
}

} // namespace proxy
} // namespace network