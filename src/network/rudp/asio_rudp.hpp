//
// @author lightning1993 <469953258@qq.com> 2025/08
//
#pragma once
#include "asio_rudp_definitions.hpp"
#include "network/use_asio.hpp"

#include <functional>
#include <memory>

namespace network::rudp
{
struct rudp_socket;
using rudp_handle_t = std::weak_ptr<rudp_socket>;

rudp_handle_t rudp_create(asio::io_context& ios, Fundamental::error_code& ec);

void rudp_release(rudp_handle_t handle);

void rudp_bind(rudp_handle_t handle, std::uint16_t port, std::string address, Fundamental::error_code& ec);

void rudp_listen(rudp_handle_t handle, std::size_t max_pending_connections, Fundamental::error_code& ec);

void async_rudp_accept(rudp_handle_t handle,
                       asio::io_context& ios,
                       const std::function<void(rudp_handle_t handle, Fundamental::error_code)>& complete_func);

void async_rudp_connect(rudp_handle_t handle,
                        const std::string& address,
                        std::uint16_t port,
                        const std::function<void(Fundamental::error_code)>& complete_func);

void async_rudp_send(rudp_handle_t handle,
                     const void* buf,
                     std::size_t len,
                     const std::function<void(std::size_t, Fundamental::error_code)>& complete_func);

void async_rudp_recv(rudp_handle_t handle,
                     void* buf,
                     std::size_t len,
                     const std::function<void(std::size_t, Fundamental::error_code)>& complete_func);

void rudp_config_sys(rudp_config_type type, std::size_t value);

void rudp_config(rudp_handle_t handle, rudp_config_type type, std::size_t value);
} // namespace network::rudp