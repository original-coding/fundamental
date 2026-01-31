#pragma once

#ifndef ASIO_USE_WOLFSSL
    #undef SHA256
#endif
#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 26800 6255 6387 6031 6258 6001 26439 26495 26819)
#endif
#include <asio.hpp>
#include <asio/detail/noncopyable.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/buffer.hpp>

#ifdef _MSC_VER
    #pragma warning(pop)
#endif
#include <functional>
#include <string_view>
#include <vector>

namespace network
{
using tcp_socket  = asio::ip::tcp::socket;
using string_view = std::string_view;

using network_io_handler_t    = std::function<void(std::error_code, std::size_t)>;
using network_write_buffer_t  = asio::const_buffer;
using network_read_buffer_t   = asio::mutable_buffer;
using network_write_buffers_t = std::vector<network_write_buffer_t>;
using network_read_buffers_t  = std::vector<network_read_buffer_t>;
} // namespace network
