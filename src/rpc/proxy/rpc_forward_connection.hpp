#pragma once
#include "proxy_buffer.hpp"
#include "rpc/basic/const_vars.h"

#include <functional>
#include <memory>

namespace network
{

namespace proxy
{

struct proxy_upstream_interface {
    virtual void async_buffers_read(network_read_buffers_t buffers, network_io_handler_t handler)        = 0;
    virtual void async_buffers_read_some(network_read_buffers_t buffers, network_io_handler_t handler)   = 0;
    virtual void async_buffers_write(network_write_buffers_t buffers, network_io_handler_t handler)      = 0;
    virtual void async_buffers_write_some(network_write_buffers_t buffers, network_io_handler_t handler) = 0;
    virtual const asio::any_io_executor& get_current_executor()                                          = 0;
    virtual void release_obj()                                                                           = 0;
    virtual void probe_protocal(std::size_t offset = 0, std::size_t target_probe_size = 0) {};
};

class rpc_forward_connection : public std::enable_shared_from_this<rpc_forward_connection> {
    enum TrafficProxyStatusMask : std::uint32_t
    {
        UpstreamReading      = (1 << 0),
        UpstreamWriting      = (1 << 1),
        ProxyDnsResolving    = (1 << 2),
        ServerConnecting     = (1 << 3),
        DownstreamReading    = (1 << 4),
        DownstreamWriting    = (1 << 5),
        TrafficProxyCloseAll = static_cast<std::uint32_t>(~0),
    };

public:
    // default value
    constexpr static std::size_t kDefaultMaxForwardCacheNums                       = 8;
    constexpr static std::size_t kDefaultReconnectRetryIntervalMsec                = 1000;
    constexpr static std::size_t kDefaultMaxReconnectCnts                          = 5;
    constexpr static std::size_t kDefaultIdleCheckIntervalMsec                     = 30000;
    constexpr static std::size_t kDefaultHalfConnectionStatusIdleCheckIntervalMsec = 500;
    // 1000Mbps
    constexpr static std::size_t kDefaultForwardSpeedLimitRateBytesPerSec = 125 * 1024 * 1024;
    // you can change this value to change the performance

    inline static std::size_t kMaxForwardCacheNums        = kDefaultMaxForwardCacheNums;
    inline static std::size_t kReconnectRetryIntervalMsec = kDefaultReconnectRetryIntervalMsec;
    inline static std::size_t kMaxReconnectCnts           = kDefaultMaxReconnectCnts;
    inline static std::size_t kIdleCheckIntervalMsec      = kDefaultIdleCheckIntervalMsec;
    inline static std::size_t kHalfConnectionStatusIdleCheckIntervalMsec =
        kDefaultHalfConnectionStatusIdleCheckIntervalMsec;
    // we usually not limit proxy speed
    inline static std::size_t kForwardSpeedLimitRateBytesPerSec = 0;

public:
    void start();
    virtual ~rpc_forward_connection();
    explicit rpc_forward_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                    std::string pre_read_data = "");
    virtual void release_obj();
    // 0 means disable traffix control
    void set_forward_speed_limit(std::size_t forward_limit_speed_bytes_per_second);
    network::network_data_reference& get_reference_obj() {
        return reference_;
    }

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
    void load_preread_data(std::string preread_data);

protected:
    void StartDnsResolve(const std::string& host, const std::string& service);
    void StartConnect(asio::ip::tcp::resolver::results_type&& result);
    void StartClientRead();
    void FallBackProtocal();
    void RestartTimeoutIdleCheck();
    void RestartTrafficControlUpdate();
    bool has_reach_downstream_speed_limit() const;
    bool has_reach_upstream_speed_limit() const;

private:
    void StartServerRead();
    void StartClient2ServerWrite();
    void StartServer2ClientWrite();
    void DoProxyConnect(asio::ip::tcp::resolver::results_type result, std::size_t try_cnt);
    void DoDelayReconnect(asio::ip::tcp::resolver::results_type result, std::size_t try_cnt);
    static bool has_status(std::uint32_t current_status, std::uint32_t status_mask);
    static bool has_any_status(std::uint32_t current_status, std::uint32_t status_mask);
    void start_forward_traffic_control();
    void resume_traffic_control_read();

protected:
    void ssl_handshake();
    bool proxy_by_ssl();

protected:
    // forward imp
    inline void forward_async_buffers_read(network_read_buffers_t buffers, network_io_handler_t handler) {
        upstream->async_buffers_read(std::move(buffers), handler);
    }

    inline void forward_async_buffers_read_some(network_read_buffers_t buffers, network_io_handler_t handler) {
        upstream->async_buffers_read_some(std::move(buffers), handler);
    }

    inline void forward_async_write_buffers(network_write_buffers_t buffers, network_io_handler_t handler) {
        upstream->async_buffers_write(std::move(buffers), handler);
    }
    inline void forward_async_write_buffers_some(network_write_buffers_t buffers, network_io_handler_t handler) {
        upstream->async_buffers_write_some(std::move(buffers), handler);
    }

    void downstream_async_buffer_read(network_read_buffers_t buffers, network_io_handler_t handler) {
        if (proxy_by_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            asio::async_read(*ssl_stream_, std::move(buffers), std::move(handler));
#endif
        } else {
            asio::async_read(proxy_socket_, std::move(buffers), std::move(handler));
        }
    }
    void downstream_async_buffer_read_some(network_read_buffers_t buffers, network_io_handler_t handler) {
        if (proxy_by_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            ssl_stream_->async_read_some(std::move(buffers), std::move(handler));
#endif
        } else {
            proxy_socket_.async_read_some(std::move(buffers), std::move(handler));
        }
    }

    void downstream_async_write_buffers(network_write_buffers_t buffers, network_io_handler_t handler) {
        if (proxy_by_ssl()) {
#ifndef NETWORK_DISABLE_SSL
            asio::async_write(*ssl_stream_, std::move(buffers), std::move(handler));
#endif
        } else {
            asio::async_write(proxy_socket_, std::move(buffers), std::move(handler));
        }
    }
    void downstream_async_write_buffers_some(network_write_buffers_t buffers, network_io_handler_t handler) {
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
    std::shared_ptr<proxy_upstream_interface> upstream;
    const asio::any_io_executor& ref_executor_;
    //
    asio::ip::tcp::socket proxy_socket_;
#ifndef NETWORK_DISABLE_SSL
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> ssl_stream_ = nullptr;
    network_client_ssl_config ssl_config_;
#endif
    asio::ip::tcp::resolver resolver;
    std::uint32_t status = UpstreamReading | UpstreamWriting;
    //
    decltype(Fundamental::MakePoolMemorySource()) cachePool;
    EndponitCacheStatus client2server;
    EndponitCacheStatus server2client;
    // idle check
    std::size_t idle_check_interval_msec = kDefaultIdleCheckIntervalMsec;
    // raw client socket
    bool client_delay_read_more = false;
    // remote server socket
    bool server_delay_read_more = false;
    // traffic control recv window size
    std::size_t config_control_window_size  = kForwardSpeedLimitRateBytesPerSec;
    std::size_t traffic_control_window_size = 0;
    std::size_t client_recv_window_size     = kDefaultForwardSpeedLimitRateBytesPerSec;
    std::size_t server_recv_window_size     = kDefaultForwardSpeedLimitRateBytesPerSec;
    // timer
    asio::steady_timer idle_check_timer;
    asio::steady_timer delay_reconnect_timer;
    asio::steady_timer traffic_control_timer;
};

} // namespace proxy
} // namespace network