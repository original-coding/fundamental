#pragma once
#include "proxy_buffer.hpp"
#include "rpc/basic/const_vars.h"
#include <memory>

#include "rpc/connection.h"
namespace network
{
namespace rpc_service
{
class connection;
}
namespace proxy
{
using rpc_service::connection;
class rpc_forward_connection : public std::enable_shared_from_this<rpc_forward_connection> {
    friend class connection;
    enum TrafficProxyStatusMask : std::uint32_t
    {
        ClientProxying                        = (1 << 0),
        ProxyDnsResolving                     = (1 << 2),
        ServerProxying                        = (1 << 3),
        ServerConnecting                      = (1 << 4),
        TrafficProxyCloseExceptClientProxying = static_cast<std::uint32_t>(~ClientProxying),
        TrafficProxyCloseExceptServerProxying = static_cast<std::uint32_t>(~ServerProxying),
        TrafficProxyCloseAll                  = static_cast<std::uint32_t>(~0),
    };

public:
    void start();
    virtual ~rpc_forward_connection();
    explicit rpc_forward_connection(std::shared_ptr<rpc_service::connection> ref_connection,
                                    std::string pre_read_data = "");
    void release_obj();

protected:
    virtual void process_protocal() = 0;
    void HandleDisconnect(asio::error_code ec,
                          const std::string& callTag = "",
                          std::uint32_t closeMask    = TrafficProxyCloseAll);
    virtual void HandleConnectSuccess();
    // This function is a protocol handling example, and its default implementation initiates a connection to the peer
    // and enables read operations on the raw connection.
    virtual void StartProtocal();
    // This function will be called after a successful proxy connection or SSL handshake.
    virtual void StartForward();
    void enable_ssl(network_client_ssl_config client_ssl_config);
    
protected:
    void StartDnsResolve(const std::string& host, const std::string& service);
    void StartConnect(asio::ip::tcp::resolver::results_type&& result);
    void StartServer2ClientWrite();
    void StartClientRead();
    void StartClient2ServerWrite();
    void StartServerRead();
    void FallBackProtocal();
protected:
    void ssl_handshake();
    bool proxy_by_ssl();

protected:
    // forward imp
    template <typename Handler, typename BufferType>
    inline void forward_async_buffer_read(BufferType buffers, Handler handler) {
        upstream->async_buffer_read(std::move(buffers), handler);
    }

    template <typename Handler, typename BufferType>
    inline void forward_async_buffer_read_some(BufferType buffers, Handler handler) {
        upstream->async_buffer_read_some(std::move(buffers), handler);
    }

    template <typename BufferType, typename Handler>
    inline void forward_async_write_buffers(BufferType buffers, Handler handler) {
        upstream->async_write_buffers(std::move(buffers), handler);
    }
    template <typename BufferType, typename Handler>
    inline void forward_async_write_buffers_some(BufferType&& buffers, Handler handler) {
        upstream->async_write_buffers_some(std::move(buffers), handler);
    }

    template <typename BufferType, typename Handler>
    void downstream_async_buffer_read(BufferType buffers, Handler handler) {
        if (proxy_by_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            asio::async_read(*ssl_stream_, std::move(buffers), std::move(handler));
#endif
        } else {
            asio::async_read(proxy_socket_, std::move(buffers), std::move(handler));
        }
    }
    template <typename BufferType, typename Handler>
    void downstream_async_buffer_read_some(BufferType buffers, Handler handler) {
        if (proxy_by_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            ssl_stream_->async_read_some(std::move(buffers), std::move(handler));
#endif
        } else {
            proxy_socket_.async_read_some(std::move(buffers), std::move(handler));
        }
    }

    template <typename BufferType, typename Handler>
    void downstream_async_write_buffers(BufferType&& buffers, Handler handler) {
        if (proxy_by_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            asio::async_write(*ssl_stream_, std::move(buffers), std::move(handler));
#endif
        } else {
            asio::async_write(proxy_socket_, std::move(buffers), std::move(handler));
        }
    }
    template <typename BufferType, typename Handler>
    void downstream_async_write_buffers_some(BufferType&& buffers, Handler handler) {
        if (proxy_by_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            ssl_stream_->async_write_some(std::move(buffers), std::move(handler));
#endif
        } else {
            proxy_socket_.async_write_some(std::move(buffers), std::move(handler));
        }
    }

protected:
    network::network_data_reference reference_;
    std::string proxy_host;
    std::string proxy_service;
    /// Socket for the connection.
    std::shared_ptr<rpc_service::connection> upstream;
    const asio::any_io_executor& ref_executor_;
    //
    asio::ip::tcp::socket proxy_socket_;
#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_ = nullptr;
    network_client_ssl_config ssl_config_;
#endif
    asio::ip::tcp::resolver resolver;
    std::int32_t status = ClientProxying;
    //
    decltype(Fundamental::MakePoolMemorySource()) cachePool;
    EndponitCacheStatus client2server;
    EndponitCacheStatus server2client;
    bool upstream_delay_close   = false;
    bool downstream_delay_close = false;
};

} // namespace proxy
} // namespace network