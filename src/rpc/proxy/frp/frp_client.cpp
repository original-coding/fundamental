#include "frp_client.hpp"
#include "frp_command.hpp"

#include "fundamental/basic/uuid_utils.hpp"
namespace network
{
namespace proxy
{

frp_client::frp_client(const std::string& host,
                       const std::string& service,
                       std::uint16_t want_port,
                       const std::string& proxy_host,
                       const std::string& proxy_service) :
host(host), service(service), want_proxy_port(want_port), proxy_host(proxy_host), proxy_service(proxy_service),
frp_id(Fundamental::to_string(Fundamental::GenerateUUID())),
delay_retry_timer(network::io_context_pool::Instance().get_io_context()), executor_(delay_retry_timer.get_executor()) {
}
void frp_client::setup_frp_session() {
    if (!reference_.is_valid()) return;
    pending_client = frp_client_upstream::make_shared(executor_, host, service);
    pending_client->enable_ssl(ssl_config_);
    pending_client->notify_connect_result.Connect(
        shared_from_this(), [this](Fundamental::error_code ec, std::shared_ptr<frp_client_upstream> upstream) {
            if (ec || !upstream) {
                FERR("frp {} by {}:{} port:{} to origin {}:{} failed:{}", frp_id, host, service, want_proxy_port,
                     proxy_host, proxy_service, ec);
                delay_setup_frp_session();
            } else {
                auto new_client_session =
                    frp_client_session::make_shared(upstream, frp_id, proxy_host, proxy_service, want_proxy_port);
                new_client_session->notify_setup_port.Connect(shared_from_this(), [this](std::uint16_t final_port) {
                    FINFO("frp {} by {}:{} port:{} to origin {}:{} handshake success! use port:{}", frp_id, host,
                          service, want_proxy_port, proxy_host, proxy_service, final_port);
                });
                new_client_session->notify_accept_result.Connect(shared_from_this(), [this](bool b_success) {
                    if (!b_success) {
                        FERR("frp {} by {}:{} port:{} to origin {}:{} accept failed", frp_id, host, service,
                             want_proxy_port, proxy_host, proxy_service);
                        delay_setup_frp_session();
                    } else {
                        FINFO("frp {} by {}:{} port:{} to origin {}:{} accept success", frp_id, host, service,
                              want_proxy_port, proxy_host, proxy_service);
                        setup_frp_session();
                    }
                });
                new_client_session->start();
            }
        });
    pending_client->start_async_connect();
}

void frp_client::delay_setup_frp_session() {
    delay_retry_timer.expires_after(std::chrono::milliseconds(2000));
    delay_retry_timer.async_wait([this, self = shared_from_this()](const asio::error_code& ec) {
        if (!reference_.is_valid()) {
            return;
        }
        if (ec) {
            return;
        }
        setup_frp_session();
    });
}

frp_client_session::frp_client_session(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                       const std::string& frp_id,
                                       const std::string& dst_host,
                                       const std::string& dst_port,
                                       std::uint16_t proxy_port) :
rpc_forward_connection(ref_connection), frp_id(frp_id), proxy_port(proxy_port) {
    proxy_host    = dst_host;
    proxy_service = dst_port;
}

void frp_client_session::StartProtocal() {
    StartDnsResolve(proxy_host, proxy_service);
}

void frp_client_session::HandleConnectSuccess() {
    StartClientRead();
    rpc_forward_connection::StartForward();
}

frp_client_session::~frp_client_session() {
    try_notify_accept_result(false);
}

void frp_client_session::process_protocal() {
    origin_timeout_msec = idle_check_interval_msec;
    // 2 hour
    idle_check_interval_msec = 2 * 3600 * 1000;
    RestartTimeoutIdleCheck();
    send_frp_setup();
}

void frp_client_session::send_frp_setup() {

    frp_setup_request_data request_data;
    request_data.command    = frp_command_type::frp_setup_request_command;
    request_data.frp_id     = frp_id;
    request_data.proxy_port = proxy_port;
    auto command_data       = packet_frp_command_data(request_data);

    forward_async_write_buffers({ network_write_buffer_t { command_data->data(), command_data->size() } },
                                [this, self = shared_from_this(), command_data](std::error_code ec, std::size_t) {
                                    if (!reference_.is_valid()) {
                                        return;
                                    }
                                    if (ec) {
                                        FERR("frp {} send_frp_setup error:{}", frp_id, Fundamental::error_code(ec));
                                        release_obj();
                                    }
                                });
    process_peer_command(frp_command_type::frp_setup_response_command);
}

void frp_client_session::process_peer_command(frp_command_type next_command) {
    forward_async_buffers_read(
        { network_read_buffer_t { head_buf, 4 } },
        [this, self = shared_from_this(), next_command](std::error_code ec, std::size_t) {
            if (!reference_.is_valid()) {
                return;
            }
            do {
                if (ec) {
                    FERR("frp {} process_peer_command error:{}", frp_id, Fundamental::error_code(ec));
                    break;
                }
                std::uint32_t command_len = 0;
                Fundamental::net_buffer_copy(head_buf, &command_len, 4);
                if (command_len > frp_command_base::kMaxCommandPayloadLen) {
                    FERR("payload len:{} overflow:{}", command_len, frp_command_base::kMaxCommandPayloadLen);
                    break;
                }
                payload.resize(command_len);
                forward_async_buffers_read(
                    { network_read_buffer_t { payload.data(), payload.size() } },
                    [this, self = shared_from_this(), next_command](std::error_code ec, std::size_t) {
                        if (!reference_.is_valid()) {
                            return;
                        }
                        do {
                            if (ec) {
                                FERR("frp {} process_peer_command error:{}", frp_id, Fundamental::error_code(ec));
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
                            case frp_command_type::frp_setup_response_command:
                                process_frp_setup_response(std::move(payload));
                                break;
                            case frp_command_type::frp_accept_notify_command:
                                process_frp_accept_signal(std::move(payload));
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

void frp_client_session::process_frp_setup_response(std::string commad_data) {

    frp_setup_response_data response_data;
    Fundamental::io::from_json(commad_data, response_data);
    if (response_data.proxy_port == 0) {
        FERR("{} no avaliable proxy port", frp_id);
        return;
    }
    notify_setup_port.Emit(response_data.proxy_port);
    process_peer_command(frp_command_type::frp_accept_notify_command);
}

void frp_client_session::process_frp_accept_signal(std::string) {
    // reset idle check timer
    idle_check_interval_msec = origin_timeout_msec;
    RestartTimeoutIdleCheck();
    try_notify_accept_result(true);
    // now we should connect to proxy host:port
    StartProtocal();
}

void frp_client_session::try_notify_accept_result(bool b_success) {
    bool expected = false;
    if (has_notify_accept_result.compare_exchange_strong(expected, true)) {
        notify_accept_result.Emit(b_success);
    }
}

} // namespace proxy
} // namespace network
