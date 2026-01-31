#include "frp_server_connection.hpp"
namespace network
{
namespace proxy
{

void frp_basic_connection::start_proxy_session_waiting(std::shared_ptr<frp_port_proxy_session> session) {
    ref_port_session = session;
    async_buffers_read({ network_read_buffer_t { preread_buf, 1 } },
                       [this, self = shared_from_this()](std::error_code ec, std::size_t len) {
                           if (!reference_.is_valid()) {
                               return;
                           }
                           do {
                               if (ec) {
                                   FERR("frp {} error:{}", frp_id, Fundamental::error_code(ec));
                                   break;
                               }
                               std::scoped_lock<std::mutex> locker(preread_mutex);
                               preread_cache.push_back(preread_buf[0]);
                               notify_data_pending(preread_cache);
                               return;
                           } while (0);
                           release_obj();
                       });
}

void frp_basic_connection::process_peer_command(frp_command_type next_command) {
    async_buffers_read({ network_read_buffer_t { head_buf, 4 } }, [this, self = shared_from_this(),
                                                                   next_command](std::error_code ec, std::size_t) {
        if (!reference_.is_valid()) {
            return;
        }
        do {
            if (ec) {
                FERR("frp {} recv head error:{}", frp_id, Fundamental::error_code(ec));
                break;
            }
            std::uint32_t command_len = 0;
            Fundamental::net_buffer_copy(head_buf, &command_len, 4);
            if (command_len > frp_command_base::kMaxCommandPayloadLen) {
                FERR("payload len:{} overflow:{}", command_len, frp_command_base::kMaxCommandPayloadLen);
                break;
            }
            payload.resize(command_len);
            async_buffers_read({ network_read_buffer_t { payload.data(), payload.size() } },
                               [this, self = shared_from_this(), next_command](std::error_code ec, std::size_t) {
                                   if (!reference_.is_valid()) {
                                       return;
                                   }
                                   do {
                                       if (ec) {
                                           FERR("frp {} recv payload error:{}", frp_id, Fundamental::error_code(ec));
                                           break;
                                       }
                                       frp_command_base base_command;
                                       Fundamental::io::from_json(payload, base_command);
                                       if (base_command.command != next_command) {
                                           FERR("want command:{} actual:{}", static_cast<std::int32_t>(next_command),
                                                static_cast<std::int32_t>(base_command.command));
                                           break;
                                       }
                                       switch (next_command) {
                                       case frp_command_type::frp_setup_request_command:
                                           process_frp_setup(std::move(payload));
                                           break;
                                       default: break;
                                       }
                                       return;
                                   } while (0);
                                   release_obj();
                               });
            return;
        } while (0);
        release_obj();
    });
}

void frp_basic_connection::process_frp_setup(std::string commad_data) {
    frp_setup_request_data request_data;
    Fundamental::io::from_json(commad_data, request_data);
    if (!verify_func_ || !verify_func_(request_data.proxy_port)) {
        FERR("frp:{} port:{} verify failed", request_data.frp_id, request_data.proxy_port);
        return;
    }
    proxy_port = request_data.proxy_port;
    frp_id     = request_data.frp_id;
    send_frp_setup_response();
}

void frp_basic_connection::send_frp_setup_response() {
    frp_setup_response_data request_data;
    request_data.command    = frp_command_type::frp_setup_response_command;
    request_data.frp_id     = frp_id;
    request_data.proxy_port = proxy_port;
    auto command_data       = packet_frp_command_data(request_data);
    async_buffers_write({ network_write_buffer_t { command_data->data(), command_data->size() } },
                        [this, self = shared_from_this(), command_data](std::error_code ec, std::size_t) {
                            if (!reference_.is_valid()) {
                                return;
                            }
                            if (ec) {
                                FERR("frp {} error:{}", frp_id, Fundamental::error_code(ec));
                                release_obj();
                                return;
                            }
                            FINFO("frp:{} port:{} handshake success", frp_id, proxy_port);
                            notify_connection_ready.Emit(shared_from_this());
                        });
}

void frp_basic_connection::ssl_handshake() {
    // handle ssl
#ifndef NETWORK_DISABLE_SSL
    auto self   = shared_from_this();
    ssl_stream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket_, *ssl_context_ref);

    ssl_stream_->async_handshake(asio::ssl::stream_base::server, [this, self](const asio::error_code& error) {
        if (!reference_.is_valid()) {
            return;
        }
        if (error) {
            FERR("ssl handshake failed with ec:{} {}", error.value(), error.message());
            release_obj();
            return;
        }
        start_protocal();
    });
#endif
}

frp_port_proxy_connection::frp_port_proxy_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                                     ::asio::ip::tcp::socket&& downstream_socket) :
rpc_forward_connection(ref_connection) {
    proxy_socket_ = std::move(downstream_socket);
}

void frp_port_proxy_connection::delay_client_read_init(std::string preread_data) {
    load_preread_data(std::move(preread_data));
    StartClientRead();
}

void frp_port_proxy_connection::process_protocal() {
    frp_accept_notify_data request_data;
    request_data.command = frp_command_type::frp_accept_notify_command;
    auto command_data    = packet_frp_command_data(request_data);
    forward_async_write_buffers({ network_write_buffer_t { command_data->data(), command_data->size() } },
                                [this, self = shared_from_this(), command_data](std::error_code ec, std::size_t) {
                                    if (!reference_.is_valid()) {
                                        return;
                                    }
                                    if (ec) {
                                        release_obj();
                                        return;
                                    }
                                    rpc_forward_connection::StartForward();
                                });
}

frp_port_proxy_session::frp_port_proxy_session(std::uint16_t proxy_port, const std::string& frp_id) :
acceptor_(io_context_pool::Instance().get_io_context()), frp_id_(frp_id) {
    protocal_helper::init_acceptor(acceptor_, static_cast<std::uint16_t>(proxy_port));
}
void frp_port_proxy_session::push_new_session(std::shared_ptr<frp_basic_connection> new_session_connection) {
    new_session_connection->start_proxy_session_waiting(shared_from_this());
    {
        std::scoped_lock<std::mutex> locker(pending_mutex);
        pending_requests.push(new_session_connection);
    }
    try_accept();
}
frp_port_proxy_session::~frp_port_proxy_session() {
    FINFO("release frp {} proxy session", frp_id_);
}

void frp_port_proxy_session::try_accept() {
    std::scoped_lock<std::mutex> locker(pending_mutex);
    if (pending_requests.empty()) return;
    if (accept_flag.test_and_set()) return;
    acceptor_.async_accept(
        acceptor_.get_executor(), [this, ptr = weak_from_this()](asio::error_code ec, asio::ip::tcp::socket socket) {
            auto ref = ptr.lock();
            if (!ref) return;
            accept_flag.clear();
            if (ec) {
                // maybe system error... ignored
            } else {
                // do next accept task
                Fundamental::ScopeGuard accept_g([&]() { try_accept(); });
                // take underlying socker ownership to change executor
                auto native_sock = socket.native_handle();
                auto protocal    = socket.local_endpoint().protocol();
                Fundamental::error_code release_ec;
                socket.release(release_ec);
                if (release_ec) return;
                while (!pending_requests.empty()) {
                    auto front_session = pending_requests.front().lock();
                    pending_requests.pop();
                    if (!front_session) {
                        continue;
                    }

                    auto new_proxy_session = frp_port_proxy_connection::make_shared(
                        front_session,
                        ::asio::ip::tcp::socket(front_session->get_current_executor(), protocal, native_sock));
                    front_session->finish_proxy_session_waiting(
                        [raw_ptr = new_proxy_session.get(),
                         ref     = new_proxy_session->weak_from_this()](std::string preread_data) {
                            auto strong = ref.lock();
                            if (!strong) return;
                            raw_ptr->delay_client_read_init(std::move(preread_data));
                        });
                    new_proxy_session->start();
                    break;
                }
            }
        });
}

} // namespace proxy
} // namespace network