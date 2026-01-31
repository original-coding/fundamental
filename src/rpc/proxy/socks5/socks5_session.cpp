#include "socks5_session.h"
#include "fundamental/basic/log.h"
#include "network/network.hpp"
namespace SocksV5
{
using network::network_read_buffer_t;
using network::network_write_buffer_t;
Socks5Session::Socks5Session(const asio::any_io_executor& ioc_,
                             std::shared_ptr<const SocksV5::Sock5Handler> handler,
                             asio::ip::tcp::socket&& socket_) :
ioc(ioc_), udp_resolver(ioc_), socket(std::move(socket_)), dst_socket(ioc_), deadline(ioc_), ref_handler(handler) {
    deadline.expires_at(asio::steady_timer::time_point::max());
}

asio::ip::tcp::socket& Socks5Session::get_socket() {
    return socket;
}

void Socks5Session::start(const void* probe_data, std::size_t probe_len) {
    if (!ref_handler) {
        FWARN("Socks5 Session Failed to Start : probe len {} overflow {}", probe_len, kMaxProbeLen);
        stop();
        return;
    }
    if (probe_len > kMaxProbeLen) {
        FWARN("Socks5 Session Failed to Start : proxy handler is invalid");
        stop();
        return;
    }
    timeout = ref_handler->timeout_check_sec_interval;
    try {
        local_endpoint   = socket.local_endpoint();
        tcp_cli_endpoint = socket.remote_endpoint();

        FDEBUG("New Client Connection {}", convert::format_address(tcp_cli_endpoint));

        check_deadline();
        keep_alive();
        if (probe_len > 0) {
            std::memcpy(probe_head, probe_data, probe_len);
        }
        if (probe_len == kMaxProbeLen) {
            get_version_and_nmethods();
        } else {
            // read  probe data
            auto self = shared_from_this();
            asio::async_read(socket, network_read_buffer_t(probe_head + probe_len, kMaxProbeLen - probe_len),
                             [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
                                 if (!ec) {
                                     get_version_and_nmethods();
                                 } else {
                                     FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
                                     stop();
                                 }
                             });
        }
    } catch (const asio::system_error& e) {
        FWARN("Socks5 Session Failed to Start : ERR_MSG = [{}]", std::string(e.what()));
        stop();
    }
}

void Socks5Session::stop() {
    asio::error_code ignored_ec;
    udp_resolver.cancel();
    socket.close(ignored_ec);
    dst_socket.close(ignored_ec);
    try {
        deadline.cancel();
    } catch (...) {
    }
}

void Socks5Session::check_deadline() {
    if (!socket.is_open() && !dst_socket.is_open()) {
        return;
    }

    if (deadline.expiry() <= asio::steady_timer::clock_type::now()) {
        FDEBUG("Client {} Timeout", convert::format_address(tcp_cli_endpoint));
        stop();
    } else {
        auto self = shared_from_this();
        deadline.async_wait([this, self = weak_from_this()](asio::error_code ec) {
            auto strong = self.lock();
            if (!strong) return;
            Socks5Session::check_deadline();
        });
    }
}

void Socks5Session::keep_alive() {
    if (timeout > 0) {
        FTRACE("Connection Keep Alive");
        deadline.expires_after(asio::chrono::seconds(timeout));
    }
}

void Socks5Session::get_version_and_nmethods() {
    do {
        ver      = static_cast<decltype(ver)>(probe_head[0]);
        nmethods = static_cast<decltype(nmethods)>(probe_head[1]);
        FDEBUG("Client {} -> Proxy {} DATA : [VER = "
               "X'{:02x}', "
               "NMETHODS = {}]",
               convert::format_address(tcp_cli_endpoint), convert::format_address(local_endpoint),
               static_cast<int16_t>(ver), static_cast<int16_t>(nmethods));
        if (ver != SocksVersion::V5) {
            FDEBUG("Unsupported protocol version {}", static_cast<std::uint32_t>(ver));
            break;
        }
        methods.resize(nmethods);
        if (nmethods > 0) {
            methods[0] = static_cast<decltype(methods)::value_type>(probe_head[2]);
        }
        get_methods_list();
        return;
    } while (0);
    stop();
}

std::string Socks5Session::methods_toString() {
    std::string methods_String;
    char hex[3];

    for (auto method_tmp : methods) {
        std::snprintf(hex, sizeof(hex), "%02x", static_cast<uint8_t>(method_tmp));
        methods_String += std::string(hex);
        methods_String.push_back(' ');
    }

    methods_String.pop_back();
    return methods_String;
}

void Socks5Session::get_methods_list() {

    auto final_process_func = [this]() {
        FDEBUG("Client {} -> Proxy {} DATA : [METHODS = {}]", convert::format_address(tcp_cli_endpoint),
               convert::format_address(local_endpoint), methods_toString());

        method = choose_method();
        reply_support_method();
    };
    if (methods.size() > 1) {
        auto self = shared_from_this();
        asio::async_read(socket, network_read_buffer_t(methods.data() + 1, methods.size() - 1),
                         [this, self, final_process_func](asio::error_code ec, size_t /*bytes_transferred*/) {
                             if (!ec) {
                                 final_process_func();
                             } else {
                                 FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
                                 stop();
                             }
                         });
    } else {
        final_process_func();
    }
}

SocksV5::Method Socks5Session::choose_method() {
    for (auto&& method_tmp : methods) {
        Fundamental::error_code ec;
        ref_handler->method_verify_handler.Emit(method_tmp, ec);
        if (!ec) {
            return method_tmp;
        }
    }
    return SocksV5::Method::NoAcceptable;
}

void Socks5Session::reply_support_method() {
    std::array<network_write_buffer_t, 2> buf = { { network_read_buffer_t(&ver, 1),
                                                    network_read_buffer_t(&method, 1) } };

    auto self = shared_from_this();
    asio::async_write(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            FDEBUG("Proxy {} -> Client {} DATA : [VER = "
                   "X'{:02x}', "
                   "METHOD = X'{:02x}']",
                   convert::format_address(local_endpoint), convert::format_address(tcp_cli_endpoint),
                   static_cast<int16_t>(ver), static_cast<int16_t>(method));

            switch (method) {
            case SocksV5::Method::NoAuth: {
                do_no_auth();
            } break;

            case SocksV5::Method::UserPassWd: {
                do_username_password_auth();
            } break;

            case SocksV5::Method::GSSAPI: {
                // not supported
            } break;

            case SocksV5::Method::NoAcceptable: {
                stop();
            } break;
            }

        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::do_no_auth() {
    get_request_from_client();
}

void Socks5Session::do_username_password_auth() {
    get_username_length();
}

void Socks5Session::get_username_length() {
    std::array<network_read_buffer_t, 2> buf = { { network_read_buffer_t(&ver, 1), network_read_buffer_t(&ulen, 1) } };

    auto self = shared_from_this();
    asio::async_read(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            FDEBUG("Client {} -> Proxy {} DATA : [VER = "
                   "X'{:02x}', ULEN = {}]",
                   convert::format_address(tcp_cli_endpoint), convert::format_address(local_endpoint),
                   static_cast<int16_t>(ver), static_cast<int16_t>(ulen));

            uname.resize(static_cast<std::size_t>(ulen));
            get_username_content();
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::get_username_content() {
    auto self = shared_from_this();
    asio::async_read(socket, network_read_buffer_t(uname.data(), uname.size()),
                     [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
                         if (!ec) {
                             FDEBUG("Client {} -> Proxy {} DATA : [UNAME = {}]",
                                    convert::format_address(tcp_cli_endpoint), convert::format_address(local_endpoint),
                                    std::string(uname.begin(), uname.end()));

                             get_password_length();
                         } else {
                             FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
                             stop();
                         }
                     });
}

void Socks5Session::get_password_length() {
    std::array<network_read_buffer_t, 1> buf = { { network_read_buffer_t(&plen, 1) } };

    auto self = shared_from_this();
    asio::async_read(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            FDEBUG("Client {} -> Proxy {} DATA : [PLEN = {}]", convert::format_address(tcp_cli_endpoint),
                   convert::format_address(local_endpoint), static_cast<int16_t>(plen));

            passwd.resize(static_cast<std::size_t>(plen));
            get_password_content();
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::get_password_content() {
    auto self = shared_from_this();
    asio::async_read(socket, network_read_buffer_t(passwd.data(), passwd.size()),
                     [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
                         if (!ec) {
                             FDEBUG("Client {} -> Proxy {} DATA : [PASSWD = {}]",
                                    convert::format_address(tcp_cli_endpoint), convert::format_address(local_endpoint),
                                    std::string(passwd.begin(), passwd.end()));

                             do_auth_and_reply();
                         } else {
                             FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
                             stop();
                         }
                     });
}

void Socks5Session::do_auth_and_reply() {
    Fundamental::error_code ec;
    ref_handler->user_verify_handler.Emit(std::string(uname.begin(), uname.end()),
                                          std::string(passwd.begin(), passwd.end()), ec);
    if (!ec) {
        status = SocksV5::ReplyAuthStatus::Success;
    } else {
        status = SocksV5::ReplyAuthStatus::Failure;
    }

    std::array<network_write_buffer_t, 2> buf = { { network_read_buffer_t(&ver, 1),
                                                    network_read_buffer_t(&status, 1) } };

    auto self = shared_from_this();
    asio::async_write(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            FDEBUG("Proxy {} -> Client {} DATA : [VER = "
                   "X'{:02x}', STATUS = X'{:02x}']",
                   convert::format_address(local_endpoint), convert::format_address(tcp_cli_endpoint),
                   static_cast<int16_t>(ver), static_cast<int16_t>(status));

            if (status == SocksV5::ReplyAuthStatus::Success) {
                get_request_from_client();
            } else {
                stop();
            }
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::get_request_from_client() {
    std::array<network_read_buffer_t, 4> buf = { { network_read_buffer_t(&ver, 1), network_read_buffer_t(&cmd, 1),
                                                   network_read_buffer_t(&rsv, 1),
                                                   network_read_buffer_t(&request_atyp, 1) } };

    auto self = shared_from_this();
    asio::async_read(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            FDEBUG("Client {} -> Proxy {} DATA : [VER = X'{:02x}', CMD "
                   "= X'{:02x}, RSV = X'{:02x}', ATYP = X'{:02x}']",
                   convert::format_address(tcp_cli_endpoint), convert::format_address(local_endpoint),
                   static_cast<int16_t>(ver), static_cast<int16_t>(cmd), static_cast<int16_t>(rsv),
                   static_cast<int16_t>(request_atyp));

            get_dst_information();
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::get_dst_information() {
    switch (request_atyp) {
    case SocksV5::Socks5HostType::Ipv4: {
        dst_addr.resize(4);
        resolve_ipv4();
    } break;

    case SocksV5::Socks5HostType::Ipv6: {
        dst_addr.resize(16);
        resolve_ipv6();
    } break;

    case SocksV5::Socks5HostType::DoMainName: {
        dst_addr.resize(UINT8_MAX);
        resolve_domain();
    } break;

    default: {
        FWARN("Unkown Request Atyp");
        reply_and_stop(SocksV5::ReplyREP::AddrTypeNotSupported);
    } break;
    }
}

void Socks5Session::resolve_ipv4() {
    std::array<network_read_buffer_t, 2> buf = { { network_read_buffer_t(dst_addr.data(), dst_addr.size()),
                                                   network_read_buffer_t(&dst_port, 2) } };

    auto self = shared_from_this();
    asio::async_read(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            // network octet order convert to host octet order
            dst_port = ntohs(dst_port);

            FDEBUG("Client {} -> Proxy {} DATA : [DST.ADDR = "
                   "{}, DST.PORT = {}]",
                   convert::format_address(tcp_cli_endpoint), convert::format_address(local_endpoint),
                   convert::dst_to_string(dst_addr, Socks5HostType::Ipv4), dst_port);

            execute_command();
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::resolve_ipv6() {
    std::array<network_read_buffer_t, 2> buf = { { network_read_buffer_t(dst_addr.data(), dst_addr.size()),
                                                   network_read_buffer_t(&dst_port, 2) } };

    auto self = shared_from_this();
    asio::async_read(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            // network octet order convert to host octet order
            dst_port = ntohs(dst_port);

            FDEBUG("Client {} -> Proxy {} DATA : [DST.ADDR "
                   "= {}, DST.PORT = {}]",
                   convert::format_address(tcp_cli_endpoint), convert::format_address(local_endpoint),
                   convert::dst_to_string(dst_addr, Socks5HostType::Ipv6), dst_port);

            execute_command();
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::resolve_domain() {
    resolve_domain_length();
}

void Socks5Session::resolve_domain_length() {
    std::array<network_read_buffer_t, 1> buf = { network_read_buffer_t(dst_addr.data(), 1) };

    auto self = shared_from_this();
    asio::async_read(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            FDEBUG("Client {} -> Proxy {} DATA : "
                   "[DOMAIN_LENGTH = {}]",
                   convert::format_address(tcp_cli_endpoint), convert::format_address(local_endpoint),
                   static_cast<int16_t>(dst_addr[0]));

            dst_addr.resize(static_cast<std::size_t>(dst_addr[0]));
            resolve_domain_content();
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::resolve_domain_content() {
    std::array<network_read_buffer_t, 2> buf = { { network_read_buffer_t(dst_addr.data(), dst_addr.size()),
                                                   network_read_buffer_t(&dst_port, 2) } };

    auto self = shared_from_this();
    asio::async_read(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            // network octet order convert to host octet order
            dst_port = ntohs(dst_port);

            FDEBUG("Client {} -> Proxy {} DATA : [DST.ADDR = "
                   "{}, DST.PORT = {}]",
                   convert::format_address(tcp_cli_endpoint), convert::format_address(local_endpoint),
                   convert::dst_to_string(dst_addr, Socks5HostType::DoMainName), dst_port);

            execute_command();
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::execute_command() {
    switch (cmd) {
    case SocksV5::RequestCMD::Connect: {
        set_connect_endpoint();
    } break;

    case SocksV5::RequestCMD::Bind: {
        /*not supported*/
        reply_and_stop(SocksV5::ReplyREP::CommandNotSupported);
    } break;

    case SocksV5::RequestCMD::UdpAssociate: {
        set_udp_associate_endpoint();
    } break;

    default: {
        reply_and_stop(SocksV5::ReplyREP::CommandNotSupported);
    } break;
    }
}

void Socks5Session::set_connect_endpoint() {
    switch (request_atyp) {
    case SocksV5::Socks5HostType::Ipv4: {
        std::error_code ec;
        tcp_dst_endpoint = asio::ip::tcp::endpoint(
            asio::ip::make_address(convert::dst_to_string(dst_addr, Socks5HostType::Ipv4), ec), dst_port);

        connect_dst_host();
    } break;

    case SocksV5::Socks5HostType::Ipv6: {
        std::error_code ec;
        tcp_dst_endpoint = asio::ip::tcp::endpoint(
            asio::ip::make_address(convert::dst_to_string(dst_addr, Socks5HostType::Ipv6), ec), dst_port);

        connect_dst_host();
    } break;

    case SocksV5::Socks5HostType::DoMainName: {
        async_dns_reslove();
    } break;
    }
}

void Socks5Session::set_udp_associate_endpoint() {
    switch (request_atyp) {
    case SocksV5::Socks5HostType::Ipv4: {
        std::error_code ec;
        udp_cli_endpoint = asio::ip::udp::endpoint(
            asio::ip::make_address(convert::dst_to_string(dst_addr, Socks5HostType::Ipv4), ec), dst_port);

        reply_udp_associate();
    } break;

    case SocksV5::Socks5HostType::Ipv6: {
        std::error_code ec;
        udp_cli_endpoint = asio::ip::udp::endpoint(
            asio::ip::make_address(convert::dst_to_string(dst_addr, Socks5HostType::Ipv6), ec), dst_port);

        reply_udp_associate();
    } break;

    case SocksV5::Socks5HostType::DoMainName: {
        async_udp_dns_reslove();
    } break;
    }
}

void Socks5Session::async_udp_dns_reslove() {
    auto self = shared_from_this();
    udp_resolver.async_resolve(
        convert::dst_to_string(dst_addr, Socks5HostType::DoMainName), std::to_string(dst_port),
        [this, self](asio::error_code ec, const asio::ip::udp::resolver::results_type& result) {
            if (!ec) {
                resolve_results = result;

                // use first endpoint
                udp_cli_endpoint = resolve_results.begin()->endpoint();

                FDEBUG("Reslove Domain {} {} result sets in total",
                       convert::dst_to_string(dst_addr, Socks5HostType::DoMainName), resolve_results.size());

                reply_udp_associate();
            } else {
                FWARN("Failed to Reslove Domain {}, ERR_MSG = [{}]",
                      convert::dst_to_string(dst_addr, Socks5HostType::DoMainName), ec.message());

                reply_and_stop(SocksV5::ReplyREP::HostUnreachable);
            }
        });
}

void Socks5Session::reply_udp_associate() {
    rep = SocksV5::ReplyREP::Succeeded;
    try {
        if (udp_cli_endpoint.address().is_v4()) {
            reply_atyp = SocksV5::Socks5HostType::Ipv4;
            udp_socket.reset(new asio::ip::udp::socket(ioc, asio::ip::udp::endpoint(asio::ip::udp::v4(), 0)));
            bnd_addr.resize(4);
            std::memcpy(bnd_addr.data(), udp_socket->local_endpoint().address().to_v4().to_bytes().data(), 4);
        } else {
            reply_atyp = SocksV5::Socks5HostType::Ipv6;
            udp_socket.reset(new asio::ip::udp::socket(ioc, asio::ip::udp::endpoint(asio::ip::udp::v6(), 0)));
            bnd_addr.resize(16);
            std::memcpy(bnd_addr.data(), udp_socket->local_endpoint().address().to_v6().to_bytes().data(), 16);
        }

        udp_bnd_endpoint = udp_socket->local_endpoint();

        // host octet order convert to network octet order
        bnd_port = htons(udp_bnd_endpoint.port());

    } catch (const asio::system_error& e) {
        FWARN("Failed to reply udp associate, ERR_MSG = [{}]", std::string(e.what()));
    }

    std::array<network_read_buffer_t, 6> buf = {
        { network_read_buffer_t(&ver, 1), network_read_buffer_t(&rep, 1), network_read_buffer_t(&rsv, 1),
          network_read_buffer_t(&reply_atyp, 1), network_read_buffer_t(bnd_addr.data(), bnd_addr.size()),
          network_read_buffer_t(&bnd_port, 2) }
    };

    auto self = shared_from_this();
    asio::async_write(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            FDEBUG("Proxy {} -> Client {} DATA : [VER = "
                   "X'{:02x}', REP = X'{:02x}', RSV = X'{:02x}' "
                   "ATYP = X'{:02x}', BND.ADDR = {}, BND.PORT = {}]",
                   convert::format_address(local_endpoint), convert::format_address(tcp_cli_endpoint),
                   static_cast<int16_t>(ver), static_cast<int16_t>(rep), static_cast<int16_t>(rsv),
                   static_cast<int16_t>(reply_atyp), udp_bnd_endpoint.address().to_string(), udp_bnd_endpoint.port());

            client_buffer.resize(BUFSIZ);

            get_udp_client();
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::get_udp_client() {
    auto self = shared_from_this();
    udp_socket->async_receive_from(network_read_buffer_t(client_buffer.data(), client_buffer.size()), sender_endpoint,
                                   [this, self](asio::error_code ec, size_t length) {
                                       if (!ec) {
                                           udp_length = length;

                                           FDEBUG("UDP Client {} -> Proxy {} Data Length = {}",
                                                  convert::format_address(sender_endpoint),
                                                  convert::format_address(udp_bnd_endpoint), udp_length);

                                           if (check_sender_endpoint()) {
                                               parse_udp_message();
                                           } else {
                                               get_udp_client();
                                           }
                                       } else {
                                           FDEBUG("Failed to receive UDP message from client");
                                           stop();
                                       }
                                   });
}

bool Socks5Session::check_sender_endpoint() {
    if (check_all_zeros()) {
        udp_cli_endpoint = sender_endpoint;
        return true;
    }

    switch (request_atyp) {
    case SocksV5::Socks5HostType::Ipv4: {
        if (check_dst_addr_all_zeros()) {
            // udp_cli_endpoint =
            //     asio::ip::udp::endpoint(asio::ip::address_v4::loopback(),
            //                             udp_cli_endpoint.port());
            udp_cli_endpoint = sender_endpoint;
            return true;
        }
    } break;

    case SocksV5::Socks5HostType::Ipv6: {
        if (check_dst_addr_all_zeros()) {
            // udp_cli_endpoint =
            //     asio::ip::udp::endpoint(asio::ip::address_v6::loopback(),
            //                             udp_cli_endpoint.port());
            udp_cli_endpoint = sender_endpoint;
            return true;
        }
    } break;

    case SocksV5::Socks5HostType::DoMainName: {
        for (auto iter : resolve_results) {
            if (iter.endpoint() == sender_endpoint) {
                return true;
            }
        }
        return false;
    } break;
    };

    return udp_cli_endpoint == sender_endpoint;
}

bool Socks5Session::check_all_zeros() {
    return udp_cli_endpoint == asio::ip::udp::endpoint(udp_cli_endpoint.protocol(), 0);
}

bool Socks5Session::check_dst_addr_all_zeros() {
    for (auto d : dst_addr) {
        if (d != 0) {
            return false;
        }
    }

    return true;
}

void Socks5Session::parse_udp_message() {
    if (udp_length <= 4) {
        FWARN("Udp Associate Header Length Error");
        stop();
        return;
    }

    std::memcpy(&udp_rsv, client_buffer.data(), sizeof(udp_rsv));
    udp_rsv = ntohs(udp_rsv);
    if (udp_rsv != 0x0000) {
        FWARN("Udp Associate RSV Not Zero");
        stop();
        return;
    }

    std::memcpy(&frag, client_buffer.data() + 2, sizeof(frag));
    if (frag != 0) {
        FWARN("Udp Associate Not Support Splice Process");
        stop();
        return;
    }

    std::memcpy(&reply_atyp, client_buffer.data() + 3, sizeof(reply_atyp));

    switch (reply_atyp) {
    case SocksV5::Socks5HostType::Ipv4: {
        if (udp_length <= 10) {
            FWARN("Udp Associate Ipv4 Length Error");
            stop();
            return;
        }

        dst_addr.resize(4);
        std::memcpy(dst_addr.data(), client_buffer.data() + 4, 4);
        std::memcpy(&dst_port, client_buffer.data() + 8, sizeof(dst_port));
        dst_port = ntohs(dst_port);
        try {
            udp_dst_endpoint = asio::ip::udp::endpoint(
                asio::ip::make_address(convert::dst_to_string(dst_addr, Socks5HostType::Ipv4)), dst_port);
        } catch (const std::exception& e) {
            FWARN("Udp Associate Ipv4 format error {}", e.what());
            stop();
            return;
        }

        udp_length -= 10;
        std::memmove(client_buffer.data(), client_buffer.data() + 10, udp_length);

        send_udp_to_dst();
    } break;

    case SocksV5::Socks5HostType::Ipv6: {
        if (udp_length <= 18) {
            FWARN("Udp Associate Ipv6 Length Error");
            stop();
            return;
        }

        dst_addr.resize(16);
        std::memcpy(dst_addr.data(), client_buffer.data() + 4, 16);
        std::memcpy(&dst_port, client_buffer.data() + 20, sizeof(dst_port));
        dst_port = ntohs(dst_port);
        try {
            udp_dst_endpoint = asio::ip::udp::endpoint(
                asio::ip::make_address(convert::dst_to_string(dst_addr, Socks5HostType::Ipv6)), dst_port);
        } catch (const std::exception& e) {
            FWARN("Udp Associate Ipv4 format error {}", e.what());
            stop();
            return;
        }

        udp_length -= 22;
        std::memmove(client_buffer.data(), client_buffer.data() + 22, udp_length);

        send_udp_to_dst();
    } break;

    case SocksV5::Socks5HostType::DoMainName: {
        dst_addr.resize(1 + UINT8_MAX);
        std::memcpy(dst_addr.data(), client_buffer.data() + 4, sizeof(dst_addr[0]));
        uint8_t domain_length = dst_addr[0];
        if (udp_length <= static_cast<size_t>(domain_length + 7)) { // 4 + 1 + len + 2
            FWARN("Udp Associate DoMainName Length Error");
            stop();
            return;
        }

        dst_addr.resize(1 + domain_length);
        std::memcpy(dst_addr.data() + 1, client_buffer.data() + 5, domain_length);
        std::memcpy(&dst_port, client_buffer.data() + 5 + domain_length, sizeof(dst_port));
        dst_port = ntohs(dst_port);

        udp_length -= domain_length + 7;
        std::memmove(client_buffer.data(), client_buffer.data() + domain_length + 7, udp_length);

        async_send_udp_message();
    } break;
    }
}

void Socks5Session::async_send_udp_message() {
    auto self = shared_from_this();
    udp_resolver.async_resolve(std::string(dst_addr.begin() + 1, dst_addr.end()), std::to_string(dst_port),
                               [this, self](asio::error_code ec, const asio::ip::udp::resolver::results_type& result) {
                                   if (!ec) {
                                       resolve_results = result;

                                       FDEBUG("Reslove Domain {} {} result sets in total",
                                              std::string(dst_addr.begin() + 1, dst_addr.end()),
                                              resolve_results.size());

                                       try_to_send_by_iterator(resolve_results.begin());
                                   } else {
                                       FWARN("Failed to Reslove Domain {}, ERR_MSG = [{}]",
                                             std::string(dst_addr.begin() + 1, dst_addr.end()), ec.message());

                                       stop();
                                   }
                               });
}

void Socks5Session::try_to_send_by_iterator(asio::ip::udp::resolver::results_type::const_iterator iter) {
    if (iter == resolve_results.end()) {
        stop();
        return;
    }

    udp_dst_endpoint = iter->endpoint();

    FDEBUG("Try to Send {}", convert::format_address(udp_dst_endpoint));

    ++iter;

    auto self = shared_from_this();
    udp_socket->async_send_to(network_read_buffer_t(client_buffer.data(), udp_length), udp_dst_endpoint,
                              [this, self, iter](asio::error_code ec, size_t length) {
                                  if (!ec) {
                                      FTRACE("Proxy {} -> UDP Server {} Data Length = {}",
                                             convert::format_address(udp_bnd_endpoint),
                                             convert::format_address(udp_dst_endpoint), length);

                                      keep_alive();
                                      receive_udp_message();
                                  } else {
                                      try_to_send_by_iterator(iter);
                                  }
                              });
}

void Socks5Session::send_udp_to_dst() {
    auto self = shared_from_this();
    udp_socket->async_send_to(
        network_read_buffer_t(client_buffer.data(), udp_length), udp_dst_endpoint,
        [this, self](asio::error_code ec, size_t length) {
            if (!ec) {
                FTRACE("Proxy {} -> UDP Server {} Data Length = {}", convert::format_address(udp_bnd_endpoint),
                       convert::format_address(udp_dst_endpoint), length);

                keep_alive();
                receive_udp_message();
            } else {
                FWARN("Failed to send message to UDP Server {}", convert::format_address(udp_dst_endpoint));
                stop();
            }
        });
}

void Socks5Session::send_udp_to_client() {
    dst_port                                  = htons(dst_port);
    std::array<network_write_buffer_t, 5> buf = {
        { network_read_buffer_t(&udp_rsv, 2), network_read_buffer_t(&frag, 1), network_read_buffer_t(&reply_atyp, 1),
          network_read_buffer_t(dst_addr.data(), dst_addr.size()), network_read_buffer_t(&dst_port, 2) }
    };

    size_t buf_bytes = 0;
    for (const auto& b : buf) {
        buf_bytes += b.size();
    }

    if (udp_length + buf_bytes > client_buffer.size()) {
        client_buffer.resize(udp_length + buf_bytes);
    }

    // user data move data to back
    std::memmove(client_buffer.data() + buf_bytes, client_buffer.data(), udp_length);

    // add header to front
    buf_bytes = 0;
    for (const auto& b : buf) {
        std::memcpy(client_buffer.data() + buf_bytes, b.data(), b.size());
        buf_bytes += b.size();
    }

    udp_length += buf_bytes;

    auto self = shared_from_this();
    udp_socket->async_send_to(
        network_read_buffer_t(client_buffer.data(), udp_length), udp_cli_endpoint,
        [this, self](asio::error_code ec, size_t length) {
            if (!ec) {
                FTRACE("Proxy {} -> UDP Client {} Data Length = {}", convert::format_address(udp_bnd_endpoint),
                       convert::format_address(udp_cli_endpoint), length);

                keep_alive();
                receive_udp_message();
            } else {
                FWARN("Failed to send message to UDP Client {}", convert::format_address(udp_cli_endpoint));

                stop();
            }
        });
}

void Socks5Session::receive_udp_message() {
    auto self = shared_from_this();
    udp_socket->async_receive_from(
        network_read_buffer_t(client_buffer.data(), client_buffer.size()), sender_endpoint,
        [this, self](asio::error_code ec, size_t length) {
            if (!ec) {
                udp_length = length;

                keep_alive();
                if (sender_endpoint == udp_cli_endpoint) {
                    FTRACE("UDP Client {} -> Proxy {} Data Length = {}", convert::format_address(udp_cli_endpoint),
                           convert::format_address(udp_bnd_endpoint), length);

                    parse_udp_message();
                } else if (sender_endpoint == udp_dst_endpoint) {
                    FTRACE("UDP Server {} -> Proxy {} Data Length = {}", convert::format_address(udp_dst_endpoint),
                           convert::format_address(udp_bnd_endpoint), length);

                    send_udp_to_client();
                } else {
                    // unkown vistor (ignore)
                    receive_udp_message();
                }

            } else {
                FWARN("Failed to receive UDP message");
                stop();
            }
        });
}

void Socks5Session::connect_dst_host() {
    auto self = shared_from_this();
    dst_socket.async_connect(tcp_dst_endpoint, [this, self](asio::error_code ec) {
        if (!ec) {
            try {
                tcp_bnd_endpoint = dst_socket.local_endpoint();
            } catch (const asio::system_error&) {
                reply_and_stop(SocksV5::ReplyREP::ConnRefused);
                return;
            }

            rep = SocksV5::ReplyREP::Succeeded;

            set_reply_address(tcp_bnd_endpoint);

            FDEBUG("Proxy {} -> Server {} Connection Successed", convert::format_address(tcp_bnd_endpoint),
                   convert::format_address(tcp_dst_endpoint));

            reply_connect_result();
        } else {
            FDEBUG("Server {} Connection Failed", convert::format_address(tcp_dst_endpoint));
            stop();
        }
    });
}

void Socks5Session::async_dns_reslove() {
    auto self = shared_from_this();
    udp_resolver.async_resolve(
        convert::dst_to_string(dst_addr, Socks5HostType::DoMainName), std::to_string(dst_port),
        [this, self](asio::error_code ec, const asio::ip::udp::resolver::results_type& result) {
            if (!ec) {
                resolve_results = result;
                FDEBUG("Reslove Domain {} {} result sets in total",
                       convert::dst_to_string(dst_addr, Socks5HostType::DoMainName), resolve_results.size());

                try_to_connect_by_iterator(resolve_results.begin());
            } else {
                FWARN("Failed to Reslove Domain {}, ERR_MSG = [{}]",
                      convert::dst_to_string(dst_addr, Socks5HostType::DoMainName), ec.message());

                reply_and_stop(SocksV5::ReplyREP::HostUnreachable);
            }
        });
}

void Socks5Session::try_to_connect_by_iterator(asio::ip::udp::resolver::results_type::const_iterator iter) {
    if (iter == resolve_results.end()) {
        reply_and_stop(SocksV5::ReplyREP::NetworkUnreachable);
        return;
    }

    tcp_dst_endpoint = asio::ip::tcp::endpoint(iter->endpoint().address(), iter->endpoint().port());

    FDEBUG("Try to Connect {}", convert::format_address(tcp_dst_endpoint));

    ++iter;

    auto self = shared_from_this();
    dst_socket.async_connect(tcp_dst_endpoint, [this, self, iter](asio::error_code ec) {
        if (!ec) {
            try {
                tcp_bnd_endpoint = dst_socket.local_endpoint();
            } catch (const asio::system_error&) {
                reply_and_stop(SocksV5::ReplyREP::ConnRefused);
                return;
            }

            rep = SocksV5::ReplyREP::Succeeded;

            set_reply_address(tcp_bnd_endpoint);

            FDEBUG("Proxy {} -> Server {} Connection Successed", convert::format_address(tcp_bnd_endpoint),
                   convert::format_address(tcp_dst_endpoint));

            reply_connect_result();
        } else {
            try_to_connect_by_iterator(iter);
        }
    });
}

void Socks5Session::reply_and_stop(SocksV5::ReplyREP rep_current) {
    rep        = rep_current;
    reply_atyp = SocksV5::Socks5HostType::Ipv4;
    bnd_addr   = { 0, 0, 0, 0 };
    bnd_port   = 0;

    std::array<network_write_buffer_t, 6> buf = {
        { network_read_buffer_t(&ver, 1), network_read_buffer_t(&rep, 1), network_read_buffer_t(&rsv, 1),
          network_read_buffer_t(&reply_atyp, 1), network_read_buffer_t(bnd_addr.data(), bnd_addr.size()),
          network_read_buffer_t(&bnd_port, 2) }
    };

    auto self = shared_from_this();
    asio::async_write(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            FDEBUG("Proxy {} -> Client {} DATA : [VER = X'{:02x}', REP "
                   "= X'{:02x}, RSV = X'{:02x}', ATYP = X'{:02x}', "
                   "BND.ADDR = {}, BND.PORT = {}]",
                   convert::format_address(local_endpoint), convert::format_address(tcp_cli_endpoint),
                   static_cast<int16_t>(ver), static_cast<int16_t>(rep), static_cast<int16_t>(rsv),
                   static_cast<int16_t>(reply_atyp), convert::dst_to_string(bnd_addr, Socks5HostType::Ipv4), bnd_port);

            stop();
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::reply_connect_result() {
    std::array<network_write_buffer_t, 6> buf = {
        { network_read_buffer_t(&ver, 1), network_read_buffer_t(&rep, 1), network_read_buffer_t(&rsv, 1),
          network_read_buffer_t(&reply_atyp, 1), network_read_buffer_t(bnd_addr.data(), bnd_addr.size()),
          network_read_buffer_t(&bnd_port, 2) }
    };

    auto self = shared_from_this();
    asio::async_write(socket, buf, [this, self](asio::error_code ec, size_t /*bytes_transferred*/) {
        if (!ec) {
            FDEBUG("Proxy {} -> Client {} DATA : [VER = X'{:02x}', REP "
                   "= X'{:02x}, RSV = X'{:02x}', ATYP = X'{:02x}', "
                   "BND.ADDR = {}, BND.PORT = {}]",
                   convert::format_address(local_endpoint), convert::format_address(tcp_cli_endpoint),
                   static_cast<int16_t>(ver), static_cast<int16_t>(rep), static_cast<int16_t>(rsv),
                   static_cast<int16_t>(reply_atyp), tcp_bnd_endpoint.address().to_string(), tcp_bnd_endpoint.port());

            client_buffer.resize(BUFSIZ);
            dst_buffer.resize(BUFSIZ);

            keep_alive();

            read_from_client();
            read_from_dst();
        } else {
            FDEBUG("Client {} Closed", convert::format_address(tcp_cli_endpoint));
            stop();
        }
    });
}

void Socks5Session::read_from_client() {
    auto self = shared_from_this();
    socket.async_read_some(network_read_buffer_t(client_buffer.data(), client_buffer.size()),
                           [this, self](asio::error_code ec, size_t length) {
                               if (!ec) {
                                   FTRACE("Client {} -> Proxy {} Data Length = {}",
                                          convert::format_address(tcp_cli_endpoint),
                                          convert::format_address(local_endpoint), length);

                                   keep_alive();
                                   send_to_dst(length);
                               } else {
                                   FTRACE("Client {} Closed", convert::format_address(tcp_cli_endpoint));
                                   stop();
                               }
                           });
}

void Socks5Session::send_to_dst(size_t write_length) {
    auto self = shared_from_this();
    asio::async_write(dst_socket, network_read_buffer_t(client_buffer.data(), write_length),
                      [this, self](asio::error_code ec, size_t length) {
                          if (!ec) {
                              FTRACE("Proxy {} -> Server {} Data Length = {}",
                                     convert::format_address(tcp_bnd_endpoint),
                                     convert::format_address(tcp_dst_endpoint), length);

                              keep_alive();
                              read_from_client();
                          } else {
                              FTRACE("Server {} Closed", convert::format_address(tcp_dst_endpoint));
                              stop();
                          }
                      });
}

void Socks5Session::read_from_dst() {
    auto self = shared_from_this();
    dst_socket.async_read_some(
        network_read_buffer_t(dst_buffer.data(), dst_buffer.size()), [this, self](asio::error_code ec, size_t length) {
            if (!ec) {
                FTRACE("Server {} -> Proxy {} Data Length = {}", convert::format_address(tcp_dst_endpoint),
                       convert::format_address(tcp_bnd_endpoint), length);

                keep_alive();
                send_to_client(length);
            } else {
                FTRACE("Server {} Closed", convert::format_address(tcp_dst_endpoint));
                stop();
            }
        });
}

void Socks5Session::send_to_client(size_t write_length) {
    auto self = shared_from_this();
    asio::async_write(socket, network_read_buffer_t(dst_buffer.data(), write_length),
                      [this, self](asio::error_code ec, size_t length) {
                          if (!ec) {
                              FTRACE("Proxy {} -> Client {} Data Length = {}", convert::format_address(local_endpoint),
                                     convert::format_address(tcp_cli_endpoint), length);

                              keep_alive();
                              read_from_dst();
                          } else {
                              FTRACE("Client {} Closed", convert::format_address(tcp_cli_endpoint));
                              stop();
                          }
                      });
}
} // namespace SocksV5