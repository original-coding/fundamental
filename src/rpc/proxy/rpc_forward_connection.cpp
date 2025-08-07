#include "rpc_forward_connection.hpp"
#include "rpc/connection.h"
#define RPC_VERBOSE 1
namespace network
{
namespace proxy
{
rpc_forward_connection::rpc_forward_connection(std::shared_ptr<rpc_service::connection> ref_connection,
                                               std::string pre_read_data) :
upstream(ref_connection), ref_executor_(upstream->executor_), proxy_socket_(ref_executor_), resolver(ref_executor_),
cachePool(Fundamental::MakePoolMemorySource()), client2server(cachePool), server2client(cachePool) {
#ifdef RPC_VERBOSE
    client2server.tag_ = "client2server";
    server2client.tag_ = "server2client";
#endif
    client2server.PrepareReadCache();
    // handle pending data
    std::size_t pending_size   = pre_read_data.size();
    std::size_t pending_offset = 0;
    while (pending_size > 0) {
        client2server.PrepareReadCache();
        auto buffer     = client2server.GetReadBuffer();
        auto chunk_size = buffer.size() > pending_size ? pending_size : buffer.size();
        std::memcpy(buffer.data(), pre_read_data.data() + pending_offset, chunk_size);
        client2server.UpdateReadBuffer(chunk_size);
        pending_size -= chunk_size;
        pending_offset += chunk_size;
    }
#ifndef NETWORK_DISABLE_SSL
    ssl_config_.disable_ssl = true;
#endif
}

void rpc_forward_connection::start() {
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

void rpc_forward_connection::HandleDisconnect(asio::error_code ec,
                                              const std::string& callTag,
                                              std::uint32_t closeMask) {
    if (!callTag.empty())
        FDEBUG("disconnect for {} {}-> ec:{}-{}", callTag, closeMask, ec.category().name(), ec.message());
    if (closeMask & ClientProxying) {
        if (status & ClientProxying) {
            status &= (~ClientProxying);
            if (upstream) upstream->release_obj();
#ifdef RPC_VERBOSE
            if (!callTag.empty()) FDEBUG("{} close proxy remote endpoint", callTag);
#endif
        }
    }
    if (closeMask & ProxyDnsResolving) {
        if (status & ProxyDnsResolving) {
            status &= (~ProxyDnsResolving);
            resolver.cancel();
#ifdef RPC_VERBOSE
            if (!callTag.empty()) FDEBUG("{} stop proxy dns resolving", callTag);
#endif
        }
    }
    if ((closeMask & ServerProxying) || (closeMask & ServerConnecting)) {
        if ((status & ServerProxying) || (status & ServerConnecting)) {
            status &= ~(ServerProxying | ServerConnecting);

            {
                auto final_clear_function = [this, ptr = shared_from_this()]() {
                    asio::error_code ec;
                    proxy_socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
                    proxy_socket_.close(ec);
                };

#ifndef NETWORK_DISABLE_SSL
                if (ssl_stream_) {
                    ssl_stream_->async_shutdown([ptr = shared_from_this(), final_clear_function](
                                                    const asio::error_code&) { final_clear_function(); });
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
            if (status & ClientProxying) StartConnect(std::move(result));
        });
}

void rpc_forward_connection::StartConnect(asio::ip::tcp::resolver::results_type&& result) {
    status |= ServerConnecting;
    asio::async_connect(proxy_socket_, result,
                        [this, self = shared_from_this()](std::error_code ec, asio::ip::tcp::endpoint endpoint) {
                            if (!reference_.is_valid()) {
                                return;
                            }
                            FDEBUG("proxy connect to {}:{}", endpoint.address().to_string(), endpoint.port());
                            if (ec) {
                                HandleDisconnect(ec, "connect");
                                return;
                            }
                            HandleConnectSuccess();
                        });
}

void rpc_forward_connection::StartForward() {
    status ^= ServerConnecting;
    status |= ServerProxying;
    if (!(status & ClientProxying)) return;
    asio::error_code error_code;
    asio::ip::tcp::no_delay option(true);
    proxy_socket_.set_option(option, error_code);
    enable_tcp_keep_alive(proxy_socket_);

    StartServerRead();
    StartClient2ServerWrite();
}

void rpc_forward_connection::enable_ssl(network_client_ssl_config client_ssl_config) {
#ifndef NETWORK_DISABLE_SSL
    ssl_config_ = client_ssl_config;
#endif
}
void rpc_forward_connection::StartServer2ClientWrite() {
    if (!(status & ClientProxying)) return;
    auto needWrite = server2client.PrepareWriteCache();
    if (needWrite) {
        upstream->async_write_buffers_some(
            server2client.GetWriteBuffer(),
            [this, self = shared_from_this()](std::error_code ec, std::size_t bytesWrite) {
                if (!reference_.is_valid()) {
                    return;
                }
                if (ec) {
                    HandleDisconnect(ec, "Server2ClientWrite", TrafficProxyCloseExceptServerProxying);
                    return;
                }
                server2client.UpdateWriteBuffer(bytesWrite);
                StartServer2ClientWrite();
            });
    } else { // when server connection is aborted and remaining data to transfer to client
        if (downstream_delay_close) {
            HandleDisconnect({}, "ServerRead delay close", TrafficProxyCloseExceptClientProxying);
        }
        if (!(status & ServerProxying)) {
            HandleDisconnect({}, "finish client transfer", ClientProxying);
        }
    }
}

void rpc_forward_connection::StartClientRead() {
    client2server.PrepareReadCache();
    upstream->async_buffer_read_some(
        client2server.GetReadBuffer(), [this, self = shared_from_this()](std::error_code ec, std::size_t bytesRead) {
            if (!reference_.is_valid()) {
                return;
            }
            client2server.UpdateReadBuffer(bytesRead);
            Fundamental::ScopeGuard guard([this]() { StartClient2ServerWrite(); });

            if (ec) {
                auto needWrite = client2server.PrepareWriteCache();
                if (needWrite) {
                    upstream_delay_close = true;
                    FDEBUG("ClientRead request delay close");
                } else {
                    HandleDisconnect(ec, "ClientRead", TrafficProxyCloseExceptServerProxying);
                }
                return;
            }
            StartClientRead();
        });
}

void rpc_forward_connection::StartClient2ServerWrite() {
    if (!(status & ServerProxying)) return;
    auto needWrite = client2server.PrepareWriteCache();
    if (needWrite) {
        downstream_async_write_buffers_some(
            client2server.GetWriteBuffer(),
            [this, self = shared_from_this()](std::error_code ec, std::size_t bytesWrite) {
                if (!reference_.is_valid()) {
                    return;
                }
                if (ec) {
                    HandleDisconnect(ec, "Client2ServerWrite", TrafficProxyCloseExceptClientProxying);
                    return;
                }
                client2server.UpdateWriteBuffer(bytesWrite);
                StartClient2ServerWrite();
            });
    } else { // when client is aborted and remaining data to transfer to server
        if (upstream_delay_close) {
            HandleDisconnect({}, "ClientRead delay close", TrafficProxyCloseExceptServerProxying);
        }
        if (!(status & ClientProxying)) {
            HandleDisconnect({}, "finish server transfer", ServerProxying);
        }
    }
}

void rpc_forward_connection::StartServerRead() {
    server2client.PrepareReadCache();
    downstream_async_buffer_read_some(
        server2client.GetReadBuffer(), [this, self = shared_from_this()](std::error_code ec, std::size_t bytesRead) {
            if (!reference_.is_valid()) {
                return;
            }
            server2client.UpdateReadBuffer(bytesRead);
            Fundamental::ScopeGuard guard([this]() { StartServer2ClientWrite(); });
            if (ec) {

                auto needWrite = server2client.PrepareWriteCache();
                if (needWrite) {
                    downstream_delay_close = true;
                    FDEBUG("ServerRead request delay close");
                } else {
                    HandleDisconnect(ec, "ServerRead", TrafficProxyCloseExceptClientProxying);
                }

                return;
            }
            StartServerRead();
        });
}

void rpc_forward_connection::FallBackProtocal() {
    auto fall_back_stream = std::move(upstream);
    release_obj();
    FDEBUG("fallback to raw rpc protocal");
    fall_back_stream->probe_protocal();
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