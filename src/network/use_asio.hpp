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
#ifdef _MSC_VER
    #pragma warning(pop)
#endif
#include <string_view>

namespace network {
using tcp_socket  = asio::ip::tcp::socket;
using string_view = std::string_view;
} // namespace network
