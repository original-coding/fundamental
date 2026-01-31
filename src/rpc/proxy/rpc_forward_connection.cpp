#include "rpc_forward_connection.hpp"
#include "rpc/connection.h"
namespace network
{
namespace proxy
{
rpc_forward_connection::rpc_forward_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                               std::string pre_read_data) :
upstream(ref_connection), ref_executor_(upstream->get_current_executor()), proxy_socket_(ref_executor_),
resolver(ref_executor_), cachePool(Fundamental::MakePoolMemorySource()), client2server(cachePool),
server2client(cachePool), idle_check_timer(ref_executor_), delay_reconnect_timer(ref_executor_),
traffic_control_timer(ref_executor_) {
#ifdef RPC_VERBOSE
    client2server.tag_ = "client2server";
    server2client.tag_ = "server2client";
#endif
    load_preread_data(std::move(pre_read_data));
#ifndef NETWORK_DISABLE_SSL
    ssl_config_.disable_ssl = true;
#endif
}

void rpc_forward_connection::start() {
    RestartTimeoutIdleCheck();

    process_protocal();
}

rpc_forward_connection::~rpc_forward_connection() {
    FDEBUG("~rpc_forward_connection");
}

void rpc_forward_connection::release_obj() {
    reference_.release();
    asio::post(ref_executor_, [this, ref = shared_from_this()] {
        try {
            HandleDisconnect({}, "release_obj");
        } catch (const std::exception& e) {
        }
    });
}

void rpc_forward_connection::set_forward_speed_limit(std::size_t forward_limit_speed_bytes_per_second) {
    config_control_window_size = forward_limit_speed_bytes_per_second;
}

void rpc_forward_connection::HandleDisconnect(asio::error_code ec,
                                              const std::string& callTag,
                                              std::uint32_t closeMask) {
    // borken pipe when write failed
    if (has_status(closeMask, UpstreamWriting)) {
        closeMask |= DownstreamReading;
    }
    if (has_status(closeMask, DownstreamWriting)) {
        closeMask |= UpstreamReading;
    }
    if (!callTag.empty())
        FDEBUG("disconnect for {} {}-> ec:{}-{}", callTag, closeMask, ec.category().name(), ec.message());
    // enter half connection pipe status
    if (has_any_status(closeMask, UpstreamReading | DownstreamReading)) {
        idle_check_interval_msec = idle_check_interval_msec > kHalfConnectionStatusIdleCheckIntervalMsec
                                       ? kHalfConnectionStatusIdleCheckIntervalMsec
                                       : idle_check_interval_msec;
        // now we should remove traffic control
        traffic_control_timer.cancel();
        // no limit
        traffic_control_window_size = 0;
        resume_traffic_control_read();
        RestartTimeoutIdleCheck();
    }
    if (has_any_status(closeMask, UpstreamReading | UpstreamWriting)) {
        auto origin_status = status;
        if (has_status(closeMask, UpstreamWriting)) {
            status &= (~UpstreamWriting);
        }
        if (has_status(closeMask, UpstreamReading)) {
            status &= (~UpstreamReading);
        }
        // read /write pipe both has been broken
        if (!has_any_status(status, UpstreamReading | UpstreamWriting)) {
            // has been changed
            if (has_any_status(origin_status, UpstreamReading | UpstreamWriting)) {
                if (upstream) upstream->release_obj();
#ifdef RPC_VERBOSE
                if (!callTag.empty()) FDEBUG("{} close proxy remote endpoint", callTag);
#endif
            }
        }
    }
    if (has_status(closeMask, ProxyDnsResolving)) {
        if (status & ProxyDnsResolving) {
            status &= (~ProxyDnsResolving);
            resolver.cancel();
#ifdef RPC_VERBOSE
            if (!callTag.empty()) FDEBUG("{} stop proxy dns resolving", callTag);
#endif
        }
    }
    if (has_any_status(closeMask, DownstreamReading | DownstreamWriting | ServerConnecting)) {
        delay_reconnect_timer.cancel();
        auto origin_status = status;
        if (has_status(closeMask, DownstreamReading)) {
            status &= (~DownstreamReading);
        }
        if (has_status(closeMask, DownstreamWriting)) {
            status &= (~DownstreamWriting);
        }
        if (has_status(closeMask, ServerConnecting)) {
            status &= (~ServerConnecting);
        }

        // read /write connect phase all has been aborted
        if (!has_any_status(status, DownstreamWriting | DownstreamReading | ServerConnecting)) {
            // has been changed
            if (has_any_status(origin_status, DownstreamWriting | DownstreamReading | ServerConnecting)) {
                {
                    auto final_clear_function = [this, ptr = shared_from_this()]() {
                        asio::error_code ec;
                        proxy_socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
                        proxy_socket_.close(ec);
                    };

#ifndef NETWORK_DISABLE_SSL
                    if (ssl_stream_) {
                        ::asio::dispatch(ssl_stream_->get_executor(), final_clear_function);
                        return;
                    }
#endif
                    final_clear_function();
                }
#ifdef RPC_VERBOSE
                if (!callTag.empty()) FDEBUG("{} close proxy local endpoint", callTag);
#endif
            }
        }
    }
    if (!has_any_status(status, DownstreamWriting | DownstreamReading | UpstreamReading | UpstreamWriting)) {
        idle_check_timer.cancel();
    }
    // no longer need reset read operation
    if (!has_any_status(status, UpstreamReading | DownstreamReading)) {
        traffic_control_timer.cancel();
    }
}

void rpc_forward_connection::HandleConnectSuccess() {
    if (proxy_by_ssl()) {
        ssl_handshake();
    } else {
        StartForward();
    }
}

void rpc_forward_connection::StartProtocal() {
    StartDnsResolve(proxy_host, proxy_service);
    StartClientRead();
}

void rpc_forward_connection::StartDnsResolve(const std::string& host, const std::string& service) {
    FDEBUG("start proxy dns resolve {}:{}", host, service);
    status |= ProxyDnsResolving;
    resolver.async_resolve(
        host, service, [ref = shared_from_this(), this](asio::error_code ec, decltype(resolver)::results_type result) {
            if (!reference_.is_valid()) {
                return;
            }
            status ^= ProxyDnsResolving;
            if (ec || result.empty()) {
                HandleDisconnect(ec, "dns resolve");
                return;
            }
            if (has_status(status, UpstreamReading | UpstreamWriting)) StartConnect(std::move(result));
        });
}

void rpc_forward_connection::StartConnect(asio::ip::tcp::resolver::results_type&& result) {
    status |= ServerConnecting;
    DoProxyConnect(std::move(result), 0);
}

void rpc_forward_connection::StartForward() {
    status ^= ServerConnecting;
    status |= DownstreamReading | DownstreamWriting;
    asio::error_code error_code;
    asio::ip::tcp::no_delay option(true);
    proxy_socket_.set_option(option, error_code);
    enable_tcp_keep_alive(proxy_socket_);
    start_forward_traffic_control();
    StartServerRead();
    StartClient2ServerWrite();
}

void rpc_forward_connection::enable_ssl(network_client_ssl_config client_ssl_config) {
#ifndef NETWORK_DISABLE_SSL
    ssl_config_ = client_ssl_config;
#endif
}

void rpc_forward_connection::load_preread_data(std::string preread_data) {
    client2server.PrepareReadCache();
    // handle pending data
    std::size_t pending_size   = preread_data.size();
    std::size_t pending_offset = 0;
    while (pending_size > 0) {
        client2server.PrepareReadCache();
        auto buffer     = client2server.GetReadBuffer();
        auto chunk_size = buffer.size() > pending_size ? pending_size : buffer.size();
        std::memcpy(buffer.data(), preread_data.data() + pending_offset, chunk_size);
        client2server.UpdateReadBuffer(chunk_size);
        pending_size -= chunk_size;
        pending_offset += chunk_size;
    }
}

void rpc_forward_connection::StartServer2ClientWrite() {
    if (!(status & UpstreamWriting)) return;
    auto needWrite = server2client.PrepareWriteCache();
    // recheck server2client buffer cache size
    if (server_delay_read_more) {
        if (server2client.cache_.size() < kMaxForwardCacheNums && !has_reach_downstream_speed_limit()) {
            server_delay_read_more = false;
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FERR("{:p} forward server2client connection  do delay read", (void*)this);
#endif
            StartServerRead();
        }
    }
    if (!needWrite) {
        if (!has_status(status, DownstreamReading)) {
            HandleDisconnect({}, "Server2ClientWrite write finished when down stream read pipe borken",
                             UpstreamWriting);
        }
        return;
    }
    forward_async_write_buffers_some({ server2client.GetWriteBuffer() },
                                     [this, self = shared_from_this()](std::error_code ec, std::size_t bytesWrite) {
                                         if (!reference_.is_valid()) {
                                             return;
                                         }
                                         if (ec) {
                                             HandleDisconnect(ec, "Server2ClientWrite", UpstreamWriting);
                                             return;
                                         }
                                         RestartTimeoutIdleCheck();
                                         server2client.UpdateWriteBuffer(bytesWrite);
                                         StartServer2ClientWrite();
                                     });
}

void rpc_forward_connection::DoProxyConnect(asio::ip::tcp::resolver::results_type result, std::size_t try_cnt) {
    FDEBUG("start connect to {}:{}", proxy_host, proxy_service);
    asio::async_connect(
        proxy_socket_, result,
        [this, self = shared_from_this(), resolve_result = result, try_cnt](std::error_code ec,
                                                                            asio::ip::tcp::endpoint endpoint) {
            if (!reference_.is_valid()) {
                return;
            }

            if (ec) {
                FDEBUG("connect to {}:{} ec:{} error:{} kMaxReconnectCnts:{} {}", proxy_host, proxy_service,
                       static_cast<std::int32_t>(ec.value()), ec.message(), kMaxReconnectCnts);
                if (kMaxReconnectCnts > 0 && ec.category() == asio::system_category()) {
                    auto err_num = static_cast<std::errc>(ec.value());
                    // in this case we should try reconnect
                    if (err_num == std::errc::connection_refused || err_num == std::errc::timed_out ||
                        err_num == std::errc::host_unreachable || err_num == std::errc::network_unreachable) {
                        DoDelayReconnect(resolve_result, try_cnt + 1);
                        return;
                    }
                }

                HandleDisconnect(ec, "connect");
                return;
            }
            FDEBUG("proxy connect cnt:{} to {}:{} success", try_cnt, endpoint.address().to_string(), endpoint.port());
            HandleConnectSuccess();
        });
}

void rpc_forward_connection::StartClientRead() {
    if (!(status & UpstreamReading)) return;
    client2server.PrepareReadCache();
    auto current_read_buffer = client2server.GetReadBuffer();
    auto min_read_size       = current_read_buffer.size();
    if (traffic_control_window_size > 0) {
        if (min_read_size > client_recv_window_size) min_read_size = client_recv_window_size;
    }
    forward_async_buffers_read_some(
        { network_read_buffer_t(current_read_buffer.data(), min_read_size) },
        [this, self = shared_from_this()](std::error_code ec, std::size_t bytesRead) {
            if (!reference_.is_valid()) {
                return;
            }
            RestartTimeoutIdleCheck();
            client2server.UpdateReadBuffer(bytesRead);
            client_recv_window_size -= client_recv_window_size > bytesRead ? bytesRead : client_recv_window_size;
            Fundamental::ScopeGuard guard([&]() { StartClient2ServerWrite(); });

            if (ec) {
                HandleDisconnect(ec, "ClientRead", UpstreamReading);
                return;
            }
            if (client2server.cache_.size() >= kMaxForwardCacheNums || has_reach_upstream_speed_limit()) {
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                FERR("{:p} forward client2server connection  request delay read", (void*)this);
#endif
                client_delay_read_more = true;
            } else {
                StartClientRead();
            }
        });
}

void rpc_forward_connection::StartClient2ServerWrite() {
    if (!(status & DownstreamWriting)) return;
    auto needWrite = client2server.PrepareWriteCache();
    // recheck client2server buffer size
    if (client_delay_read_more) {
        if (client2server.cache_.size() < kMaxForwardCacheNums && !has_reach_upstream_speed_limit()) {
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FERR("{:p} forward client2server connection  do delay read", (void*)this);
#endif
            StartClientRead();
            client_delay_read_more = false;
        }
    }
    if (!needWrite) {
        if (!has_status(status, UpstreamReading)) {
            HandleDisconnect({}, "Client2ServerWrite write finished when up stream read pipe borken",
                             DownstreamWriting);
        }
        return;
    }
    downstream_async_write_buffers_some({ client2server.GetWriteBuffer() },
                                        [this, self = shared_from_this()](std::error_code ec, std::size_t bytesWrite) {
                                            if (!reference_.is_valid()) {
                                                return;
                                            }
                                            if (ec) {
                                                HandleDisconnect(ec, "Client2ServerWrite", DownstreamWriting);
                                                return;
                                            }
                                            RestartTimeoutIdleCheck();
                                            client2server.UpdateWriteBuffer(bytesWrite);
                                            StartClient2ServerWrite();
                                        });
}

void rpc_forward_connection::StartServerRead() {
    if (!(status & DownstreamReading)) return;
    server2client.PrepareReadCache();
    auto current_read_buffer = server2client.GetReadBuffer();
    auto min_read_size       = current_read_buffer.size();
    if (traffic_control_window_size > 0) {
        if (min_read_size > server_recv_window_size) min_read_size = server_recv_window_size;
    }
    downstream_async_buffer_read_some(
        { network_read_buffer_t(current_read_buffer.data(), min_read_size) },
        [this, self = shared_from_this()](std::error_code ec, std::size_t bytesRead) {
            if (!reference_.is_valid()) {
                return;
            }
            RestartTimeoutIdleCheck();
            server2client.UpdateReadBuffer(bytesRead);
            server_recv_window_size -= server_recv_window_size > bytesRead ? bytesRead : server_recv_window_size;
            Fundamental::ScopeGuard guard([&]() { StartServer2ClientWrite(); });
            if (ec) {
                HandleDisconnect(ec, "ServerRead", DownstreamReading);
                return;
            }
            if (server2client.cache_.size() >= kMaxForwardCacheNums || has_reach_downstream_speed_limit()) {
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                FERR("{:p}  forward server2client connection request delay read", (void*)this);
#endif
                server_delay_read_more = true;
            } else {
                StartServerRead();
            }
        });
}

void rpc_forward_connection::FallBackProtocal() {
    auto fall_back_stream = std::move(upstream);
    release_obj();
    FDEBUG("fallback to raw rpc protocal");
    fall_back_stream->probe_protocal();
}

void rpc_forward_connection::RestartTimeoutIdleCheck() {
    idle_check_timer.cancel();
    idle_check_timer.expires_after(std::chrono::milliseconds(idle_check_interval_msec));
    idle_check_timer.async_wait([this, self = weak_from_this()](const asio::error_code& ec) {
        auto strong = self.lock();
        if (!strong) return;
        if (!reference_.is_valid()) {
            return;
        }
        if (ec) {
            return;
        }
        try {
            HandleDisconnect(std::make_error_code(std::errc::timed_out),
                             Fundamental::StringFormat("[idle check] for  interval {} msec", idle_check_interval_msec));
        } catch (...) {
        }
    });
}

void rpc_forward_connection::RestartTrafficControlUpdate() {
    traffic_control_timer.expires_after(std::chrono::milliseconds(1000));
    // this timer should maintain lifetime to restart read operation when window is reset by next sec
    traffic_control_timer.async_wait([this, self = shared_from_this()](const asio::error_code& ec) {
        if (!reference_.is_valid()) {
            return;
        }
        if (ec) {
            return;
        }
        if (!has_any_status(status, UpstreamReading | DownstreamReading)) {
            return;
        }
#if defined(RPC_VERBOSE)
        FINFO("{:p} proxy to {}:{} [up]{}byte/s [down]{}byte/s ", (void*)this, proxy_host, proxy_service,
              (traffic_control_window_size - client_recv_window_size),
              (traffic_control_window_size - server_recv_window_size));
#endif
        client_recv_window_size = traffic_control_window_size;
        server_recv_window_size = traffic_control_window_size;
        resume_traffic_control_read();
        RestartTrafficControlUpdate();
    });
}

bool rpc_forward_connection::has_reach_downstream_speed_limit() const {
    return traffic_control_window_size > 0 && server_recv_window_size == 0;
}

bool rpc_forward_connection::has_reach_upstream_speed_limit() const {
    return traffic_control_window_size > 0 && client_recv_window_size == 0;
}

bool rpc_forward_connection::has_status(std::uint32_t current_status, std::uint32_t status_mask) {
    return (current_status & status_mask) == status_mask;
}

bool rpc_forward_connection::has_any_status(std::uint32_t current_status, std::uint32_t status_mask) {
    return (current_status & status_mask) != 0;
}

void rpc_forward_connection::start_forward_traffic_control() {
    traffic_control_timer.cancel();
    // 0 means no limit
    traffic_control_window_size = config_control_window_size;
    client_recv_window_size     = traffic_control_window_size;
    server_recv_window_size     = traffic_control_window_size;
    if (traffic_control_window_size > 0) {
        RestartTrafficControlUpdate();
    }
}

void rpc_forward_connection::resume_traffic_control_read() {
    if (client_delay_read_more) {
        if (client2server.cache_.size() < kMaxForwardCacheNums && !has_reach_upstream_speed_limit()) {
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FERR("{:p} forward client2server connection  do delay read by limit", (void*)this);
#endif
            StartClientRead();
            client_delay_read_more = false;
        }
    }
    if (server_delay_read_more) {
        if (server2client.cache_.size() < kMaxForwardCacheNums && !has_reach_downstream_speed_limit()) {
            server_delay_read_more = false;
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FERR("{:p} forward server2client connection  do delay read by limit", (void*)this);
#endif
            StartServerRead();
        }
    }
}

void rpc_forward_connection::DoDelayReconnect(asio::ip::tcp::resolver::results_type result, std::size_t try_cnt) {
    if (try_cnt >= kMaxReconnectCnts) {
        HandleDisconnect(std::make_error_code(std::errc::timed_out),
                         Fundamental::StringFormat("connect timeout for max try {} in interval {} msec",
                                                   kMaxReconnectCnts, kReconnectRetryIntervalMsec));
        return;
    }
    FDEBUG("proxy reconnect to {}:{} cnt:{}", proxy_host, proxy_service, try_cnt);
    delay_reconnect_timer.cancel();
    delay_reconnect_timer.expires_after(std::chrono::milliseconds(kReconnectRetryIntervalMsec));
    delay_reconnect_timer.async_wait(
        [this, self = shared_from_this(), resolve_result = result, try_cnt](const asio::error_code& ec) {
            if (!reference_.is_valid()) {
                return;
            }
            // cancelled
            if (ec) {
                return;
            }
            try {
                DoProxyConnect(resolve_result, try_cnt);
            } catch (...) {
            }
        });
}

void rpc_forward_connection::ssl_handshake() {
#ifndef NETWORK_DISABLE_SSL
    asio::ssl::context ssl_context(asio::ssl::context::tlsv13);
    auto* actual_context = &ssl_context;
    try {
        if (ssl_config_.load_exception) std::rethrow_exception(ssl_config_.load_exception);
        if (!ssl_config_.ssl_context) {
            if (!ssl_config_.ca_certificate_path.empty()) {
                ssl_context.load_verify_file(ssl_config_.ca_certificate_path);
            } else {
                ssl_context.set_default_verify_paths();
            }
            if (!ssl_config_.private_key_path.empty()) {
                ssl_context.use_private_key_file(ssl_config_.private_key_path, asio::ssl::context::pem);
            }
            if (!ssl_config_.certificate_path.empty()) {
                ssl_context.use_certificate_chain_file(ssl_config_.certificate_path);
            }
        } else {
            actual_context = ssl_config_.ssl_context.get();
        }
        FDEBUG("load client ssl config success  ca:{} crt:{} key:{}", ssl_config_.ca_certificate_path,
               ssl_config_.certificate_path, ssl_config_.private_key_path);
    } catch (const std::exception& e) {
        FERR("load ssl context failed {}", e.what());
        return;
    }

    ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(proxy_socket_, *actual_context);
    ssl_stream_->set_verify_mode(asio::ssl::verify_peer);
    SSL_set_tlsext_host_name(ssl_stream_->native_handle(), proxy_host.c_str());
    ssl_stream_->async_handshake(asio::ssl::stream_base::client,
                                 [this, ptr = shared_from_this()](const asio::error_code& ec) {
                                     if (!reference_.is_valid()) {
                                         return;
                                     }
                                     if (ec) return;
                                     StartForward();
                                 });
#endif
}

bool rpc_forward_connection::proxy_by_ssl() {
#ifndef NETWORK_DISABLE_SSL
    return !ssl_config_.disable_ssl;
#else
    return false;
#endif
}
} // namespace proxy
} // namespace network