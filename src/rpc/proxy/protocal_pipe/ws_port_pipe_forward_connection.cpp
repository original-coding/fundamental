#include "ws_port_pipe_forward_connection.hpp"
#include "fundamental/basic/log.h"
#include "rpc/proxy/websocket/ws_upgrade_session.hpp"
namespace network
{
namespace proxy
{
void ws_port_pipe_forward_connection::process_pipe_handshake() {
    // send ws request first
    FINFO("try port protocal pipe to {}:{}{}", request_context.dst_host, request_context.dst_service,
          request_context.route_path);
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
                break;
            }
            if (ec) {
                FERR("protocal port protocal pipe to {}:{}{} failed:{}", request_context.dst_host,
                     request_context.dst_service, request_context.route_path, Fundamental::error_code(ec));
                break;
            } else {
                FDEBUG("protocal port protocal pipe to {}:{}{} success", request_context.dst_host,
                       request_context.dst_service, request_context.route_path);
            }
            do_pipe_proxy();
            return;
        } while (0);
    };
    ws_upgrade->init(read_callback, write_callback, finish_callback, nullptr);
    ws_upgrade->start();
}
} // namespace proxy
} // namespace network