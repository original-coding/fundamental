#include "connection.h"
#include "proxy/protocal_pipe/protocal_pipe_connection.hpp"
#include "proxy/socks5/socks5_session.h"
#include "proxy/transparent_proxy_connection.hpp"
#include "proxy/websocket/ws_forward_connection.hpp"
#include "rpc/proxy/proxy_manager.hpp"
#include "rpc_server.hpp"

namespace network
{
namespace rpc_service
{
std::shared_ptr<ServerStreamReadWriter> connection::InitRpcStream() {
    if (!rpc_stream_rw_) {
        FWARN("it's not a stream rpc call,abort it");
        throw std::logic_error("invalid rpc call with a stream interface");
    }
    std::shared_ptr<ServerStreamReadWriter> ret = rpc_stream_rw_;
    // release server stream write when connection was released
    auto release_handle = reference_.notify_release.Connect([con = ret->weak_from_this()]() {
        auto ptr = con.lock();
        if (ptr) ptr->release_obj();
    });
    // unbind
    ret->reference_.notify_release.Connect([release_handle, s = weak_from_this(), this]() {
        auto ptr = s.lock();
        if (ptr) reference_.notify_release.DisConnect(release_handle);
    });
    ret->start();
    rpc_stream_rw_ = nullptr;
    return ret;
}

void connection::process_transparent_proxy(std::size_t preread_len) {

    auto self(shared_from_this());

    // switch to proxy connection,don't need timer check any more
    cancel_timer();
    b_waiting_process_any_data.exchange(false);
    do {
        auto server = server_wref_.lock();
        if (!server) {
            FERR("server maybe post stop, cancel proxy");
            break;
        }
        FDEBUG("start raw transparent proxy {}:{} to {}:{}", get_remote_peer_ip(), get_remote_peer_port(),
               external_config.transparent_proxy_host, external_config.transparent_proxy_port);
        auto ret = network::proxy::transparent_proxy_connection::make_shared(
            shared_from_this(), external_config.transparent_proxy_host, external_config.transparent_proxy_port,
            std::string(head_, head_ + preread_len));
        // release proxy connection when server was released
        auto release_handle = server->reference_.notify_release.Connect([con = ret->weak_from_this()]() {
            auto ptr = con.lock();
            if (ptr) ptr->release_obj();
        });
        // unbind
        ret->get_reference_obj().notify_release.Connect([release_handle, s = server_wref_]() {
            auto ptr = s.lock();
            if (ptr) ptr->reference_.notify_release.DisConnect(release_handle);
        });
        ret->start();
        return;
    } while (0);
    release_obj();
}

void connection::process_ws_request(std::size_t preread_len) {
    // switch to proxy connection,don't need timer check any more
    cancel_timer();
    b_waiting_process_any_data.exchange(false);
    do {
        auto server = server_wref_.lock();
        if (!server) {
            FERR("server maybe post stop, cancel proxy");
            break;
        }
        auto query_func =
            [this, ptr = shared_from_this()](
                const std::string& api) -> std::tuple<bool, network::proxy::ProxyHost, Fundamental::ScopeGuard> {
            bool ret = false;
            network::proxy::ProxyHost proxy_host;
            Fundamental::ScopeGuard release_guard;
            do {
                if (!proxy_manager_) break;
                auto host = proxy_manager_->GetWsProxyRoute(api);
                if (!host) break;
                ret        = true;
                proxy_host = *host;
                // ref
                release_guard.reset([host]() {});
            } while (0);
            return std::make_tuple(ret, proxy_host, std::move(release_guard));
        };

        auto ret = network::proxy::websocket_forward_connection::make_shared(shared_from_this(), query_func,
                                                                             std::string(head_, head_ + preread_len));

        // release proxy connection when server was released
        auto release_handle = server->reference_.notify_release.Connect([con = ret->weak_from_this()]() {
            auto ptr = con.lock();
            if (ptr) ptr->release_obj();
        });
        // unbind
        ret->get_reference_obj().notify_release.Connect([release_handle, s = server_wref_]() {
            auto ptr = s.lock();
            if (ptr) ptr->reference_.notify_release.DisConnect(release_handle);
        });
        if (external_config.proxy_speed_limit_bytes_per_second > 0) {
            ret->set_forward_speed_limit(external_config.proxy_speed_limit_bytes_per_second);
        }
        ret->start();

        // we don't release connection here,forward connection will manage the lifetime of the connection
        return;
    } while (0);
    release_obj();
}

void connection::process_socks5_proxy(const void* preread_data, std::size_t len) {
    auto self(shared_from_this());

    // switch to proxy connection,don't need timer check any more
    cancel_timer();
    b_waiting_process_any_data.exchange(false);
    do {
        auto server = server_wref_.lock();
        if (!server) {
            FERR("server maybe post stop, cancel proxy");
            break;
        }
        auto ret = SocksV5::Socks5Session::make_shared(executor_, socks5_handler, std::move(socket_));
        ret->start(preread_data, len);
    } while (0);
    release_obj();
}

void connection::process_pipe_connection(std::size_t preread_len) {
    // switch to proxy connection,don't need timer check any more
    cancel_timer();
    b_waiting_process_any_data.exchange(false);
    do {
        auto server = server_wref_.lock();
        if (!server) {
            FERR("server maybe post stop, cancel proxy");
            break;
        }
        auto ret = network::proxy::protocal_pipe_connection::make_shared(
            shared_from_this(), external_config.forward_config, std::string(head_, head_ + preread_len));

        if (proxy_manager_) {
            ret->set_add_route_entry_function(
                [server_manager = proxy_manager_](std::string route, std::string host,
                                                  std::string service) mutable -> std::tuple<bool, std::string> {
                    network::proxy::ProxyHost proxy_host;
                    proxy_host.host                     = host;
                    proxy_host.service                  = service;
                    proxy_host.enable_cache_auto_remove = true;
                    server_manager->AddWsProxyRoute(route, std::move(proxy_host));
                    return std::make_tuple(true, "");
                });
        }
        // release proxy connection when server was released
        auto release_handle = server->reference_.notify_release.Connect([con = ret->weak_from_this()]() {
            auto ptr = con.lock();
            if (ptr) ptr->release_obj();
        });
        // unbind
        ret->get_reference_obj().notify_release.Connect([release_handle, s = server_wref_]() {
            auto ptr = s.lock();
            if (ptr) ptr->reference_.notify_release.DisConnect(release_handle);
        });
        if (external_config.proxy_speed_limit_bytes_per_second > 0) {
            ret->set_forward_speed_limit(external_config.proxy_speed_limit_bytes_per_second);
        }
        ret->start();

        // we don't release connection here,forward connection will manage the lifetime of the connection
        return;
    } while (0);
    release_obj();
}

void connection::probe_protocal(std::size_t offset, std::size_t target_probe_size) {
    FASSERT_ACTION(
        target_probe_size <= kMaxProbeReadSize,
        {
            release_obj();
            return;
        },
        "probe size must < kMaxProbeReadSize");
    auto self(shared_from_this());
    if (offset < target_probe_size) {
        async_buffers_read({ network_read_buffer_t(head_ + offset, target_probe_size - offset) },
                           [this, self, target_probe_size](asio::error_code ec, std::size_t length) {
                               if (!reference_.is_valid()) {
                                   return;
                               }
                               if (!socket_.is_open()) {
                                   on_net_err_(self, "socket already closed");
                                   return;
                               }
                               if (ec) {
#ifdef RPC_VERBOSE
                                   FDEBUG("{:p} rpc read probe head failed {}", (void*)this, ec.message());
#endif
                                   on_net_err_(self, ec.message());
                                   release_obj();
                                   return;
                               }
                               probe_protocal(target_probe_size, target_probe_size);
                           });
    } else {

        switch (head_[0]) {
            // Protocols with packet lengths that may be smaller than the RPC header length (18) should be processed
            // separately.
        case SocksV5::SocksVersion::V5: {
            if (!(external_config.rpc_protocal_mask & network::rpc_protocal_filter_socks5)) goto RPC_FALL_DOWN_ACTION;
            if (!socks5_handler) goto RPC_FALL_DOWN_ACTION;
            process_socks5_proxy(head_, offset);
            break;
        }
        case websocket::kWsMagicNum: {
            if (!(external_config.rpc_protocal_mask & network::rpc_protocal_filter_http_ws)) goto RPC_FALL_DOWN_ACTION;
            process_ws_request(offset);
            // use default handler
            break;
        }
        case forward::forward_request_context::kForwardMagicNum: {
            if (!(external_config.rpc_protocal_mask & network::rpc_protocal_filter_pipe_connection))
                goto RPC_FALL_DOWN_ACTION;
            process_pipe_connection(offset);
            // use default handler
            break;
        }
        case RPC_MAGIC_NUM: {
            if (!(external_config.rpc_protocal_mask & network::rpc_protocal_filter_rpc)) goto RPC_FALL_DOWN_ACTION;
            // ensure offset==rpc len
            if (offset < kMaxProbeReadSize) {
                probe_protocal(offset, kMaxProbeReadSize);
                break;
            }
            read_rpc_head(offset);
        } break;
        default: goto RPC_FALL_DOWN_ACTION;
        }
        return;
    RPC_FALL_DOWN_ACTION:
        // work as a transparent_proxy，proxy directly
        if (external_config.enable_transparent_proxy) {
            process_transparent_proxy(offset);
            return;
        }
        if (head_[0] == 0x16) {
            FERR("invalid ssl config");
        } else {
            FERR("none supported protocal {}", Fundamental::Utils::BufferToHex((const void*)&head_[0], 1));
        }

        release_obj();
    }
}

void connection::read_rpc_head(std::size_t offset) {
    FASSERT(offset <= kRpcHeadLen);
    auto self(shared_from_this());
    async_buffers_read(
        { network_read_buffer_t(head_ + offset, kRpcHeadLen - offset) },
        [this, self](asio::error_code ec, std::size_t length) {
            if (!reference_.is_valid()) {
                return;
            }
            if (!socket_.is_open()) {
                on_net_err_(self, "socket already closed");
                return;
            }
            if (ec) {
#ifdef RPC_VERBOSE
                FDEBUG("{:p} rpc read head failed {}", (void*)this, ec.message());
#endif
                on_net_err_(self, ec.message());
                release_obj();
                return;
            }
            b_waiting_process_any_data.exchange(false);
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FINFO("server {:p} read head {}", (void*)this, Fundamental::Utils::BufferToHex(head_, kRpcHeadLen));
#endif
            switch (head_[0]) {
            case RPC_MAGIC_NUM: process_rpc_request(); break;
            default: {
                // shoule never reach
                FASSERT(false, "{:p} protocol error magic  {:02x}", (void*)this, static_cast<std::uint8_t>(head_[0]));
                release_obj();
            } break;
            }
        });
}

void connection::read_body(uint32_t func_id, std::size_t size, std::size_t start_offset) {
    auto self(shared_from_this());
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
    FINFO("server {:p} try read size: {}", (void*)this, size - start_offset);
#endif
    async_buffers_read_some(
        { network_read_buffer_t(body_.data() + start_offset, size - start_offset) },
        [this, func_id, self, size, start_offset](asio::error_code ec, std::size_t length) {
            if (!reference_.is_valid()) {
                return;
            }
            if (!socket_.is_open()) {
                on_net_err_(self, "socket already closed");
                return;
            }
            if (ec) {
                // do't close socket, wait for write done
                on_net_err_(self, ec.message());
                return;
            }
            b_waiting_process_any_data.exchange(false);
            auto current_read_offset = start_offset + length;
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FINFO("server {:p} read some need:{}  current: {} new:{}", (void*)this, size, current_read_offset, length);
#endif
            if (current_read_offset < size) {
                read_body(func_id, size, current_read_offset);
                return;
            }
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
            FINFO("server {:p} read {} body {}", (void*)this, size,
                  Fundamental::Utils::BufferToHex(body_.data(), size > 32 ? 32 : size, 16));
#endif
            read_rpc_head();
            try {
                switch (req_type_) {
                case request_type::rpc_req: process_request(func_id, body_.data(), size); break;
                case request_type::rpc_subscribe: {
                    msgpack_codec codec;
                    auto p              = codec.unpack_tuple<std::tuple<std::string>>(body_.data(), size);
                    auto new_subscriber = std::get<0>(p);
                    auto ret            = subscribers_.insert(new_subscriber);
                    if (ret.second) {
                        on_new_subscriber_added(new_subscriber, weak_from_this());
                    }
                    response_interal(req_id_, msgpack_codec::pack_args_str(result_code::OK, ""));
                } break;
                case request_type::rpc_unsubscribe: {
                    msgpack_codec codec;
                    auto p                  = codec.unpack_tuple<std::tuple<std::string>>(body_.data(), size);
                    auto removed_subscriber = std::get<0>(p);
                    auto iter               = subscribers_.find(removed_subscriber);
                    if (iter != subscribers_.end()) {
                        subscribers_.erase(iter);
                        on_subscribers_removed({ removed_subscriber }, weak_from_this());
                    }
                    response_interal(req_id_, msgpack_codec::pack_args_str(result_code::OK, ""));
                } break;
                case request_type::rpc_publish: {
                    msgpack_codec codec;
                    auto p    = codec.unpack_tuple<std::tuple<std::string, std::string>>(body_.data(), size);
                    auto& key = std::get<0>(p);
                    auto& msg = std::get<1>(p);
                    on_publish_msg(std::move(key), std::move(msg));
                    response_interal(req_id_, msgpack_codec::pack_args_str(result_code::OK, ""));
                } break;
                default: {
                    auto result = msgpack_codec::pack_args_str(
                        result_code::FAIL,
                        Fundamental::StringFormat("bad request type:{}", static_cast<std::int32_t>(req_type_)));
                    response_interal(req_id_, std::move(result));
                } break;
                }
            } catch (const std::exception& ex) {
                auto result = msgpack_codec::pack_args_str(result_code::FAIL, ex.what());
                response_interal(req_id_, std::move(result));
            }
        });
}

void connection::process_rpc_request() {
    rpc_header header;
    header.DeSerialize(head_, kRpcHeadLen);
    req_id_                 = header.req_id;
    const uint32_t body_len = header.body_len;
    req_type_               = header.req_type;
    switch (req_type_) {
    case request_type::rpc_req:
    case request_type::rpc_subscribe:
    case request_type::rpc_unsubscribe:
    case request_type::rpc_publish: {
        if (body_len > 0) {
            if (body_len >= MAX_BUF_LEN) {
                response_interal(req_id_, msgpack_codec::pack_args_str(result_code::FAIL, "body too large"));
                break;
            }
            if (body_.size() < body_len) {
                body_.resize(body_len);
            }
            read_body(header.func_id, body_len);
        } else if (req_type_ == request_type::rpc_req) {
            handle_none_param_request(header.func_id);
        } else {
            response_interal(req_id_, msgpack_codec::pack_args_str(result_code::FAIL, "bad request"));
        }
    } break;
    case request_type::rpc_stream: {
        process_rpc_stream(header.func_id);
    } break;
    case request_type::rpc_heartbeat: {
        read_rpc_head();
        response_interal(req_id_, "", network::rpc_service::request_type::rpc_heartbeat);
        return;
    }
    default: {
        response_interal(req_id_, msgpack_codec::pack_args_str(result_code::FAIL, "bad request type"));
        break;
    }
    }
}
void connection::ssl_handshake() {
    auto self(shared_from_this());
    asio::async_read(socket_, network_read_buffer_t(head_, kSslPreReadSize),
                     [this, self](asio::error_code ec, std::size_t length) {
                         if (!reference_.is_valid()) {
                             return;
                         }
                         if (!socket_.is_open()) {
                             on_net_err_(self, "socket already closed");
                             return;
                         }
                         do {
                             if (ec) {
                                 FDEBUG("ssl handshake read failed {}", ec.message());
                                 on_net_err_(self, ec.message());

                                 break;
                             }
                             const std::uint8_t* p_data = (const std::uint8_t*)head_;
                             if (p_data[0] == 0x16) // ssl Handshake
                             {
                                 if (p_data[1] != 0x03 || p_data[2] > 0x03) {
                                     on_net_err_(self, "rpc only support SSL 3.0 to TLS 1.2");
                                     break;
                                 }
                                 do_ssl_handshake(head_, kSslPreReadSize);
                             } else {
                                 // rpc request ssl check
                                 if (!enable_no_ssl_ && p_data[0] == RPC_MAGIC_NUM) {
                                     on_net_err_(self, "only tls connection is supported");
                                     break;
                                 }
#ifdef RPC_VERBOSE
                                 FDEBUG("[rpc] WARNNING!!! falling  down to no ssl socket");
#endif
                                 ssl_context_ref = nullptr;
                                 probe_protocal(kSslPreReadSize);
                             }
                             return;
                         } while (0);
                         release_obj();
                     });
}

void connection::write() {
    if (write_buffers_.empty()) {
        auto& msg   = write_queue_.front();
        write_size_ = (uint32_t)msg.content.size();
        rpc_header { RPC_MAGIC_NUM, msg.req_type, write_size_, msg.req_id, static_cast<std::uint32_t>(conn_id_) }
            .Serialize(write_head_buffer, kRpcHeadLen);

        write_buffers_.emplace_back(network_read_buffer_t(write_head_buffer, kRpcHeadLen));
        if (write_size_ > 0) write_buffers_.emplace_back(network_read_buffer_t(msg.content.data(), write_size_));
    }

    auto self = shared_from_this();
    async_buffers_write_some(std::vector<network_write_buffer_t>(write_buffers_.begin(), write_buffers_.end()),
                             [this, self](asio::error_code ec, std::size_t length) {
                                 if (!reference_.is_valid()) {
                                     return;
                                 }
                                 if (ec) {
                                     FDEBUG("{:p} rpc write failed {}", (void*)this, ec.message());
                                     on_net_err_(shared_from_this(), ec.message());
                                     release_obj();
                                     return;
                                 }
                                 b_waiting_process_any_data.exchange(false);
#if defined(RPC_VERBOSE) && defined(RPC_DATA_VERBOSE)
                                 FINFO("server {:p} write some size:{}", (void*)this, length);
#endif
                                 while (length != 0) {
                                     if (write_buffers_.empty()) break;
                                     auto current_size = write_buffers_.front().size();
                                     if (length >= current_size) {
                                         length -= current_size;
                                         write_buffers_.pop_front();
                                         continue;
                                     }
                                     write_buffers_.front() = network_write_buffer_t(
                                         (std::uint8_t*)write_buffers_.front().data() + length, current_size - length);
                                     break;
                                 }
                                 if (!write_buffers_.empty()) { // write rest data

                                     write();
                                     return;
                                 }
                                 write_queue_.pop_front();

                                 if (!write_queue_.empty()) {
                                     write();
                                 }
                             });
}
} // namespace rpc_service
} // namespace network