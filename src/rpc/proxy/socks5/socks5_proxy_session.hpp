#pragma once
#include "fundamental/basic/log.h"
#include "network/upgrade_interface.hpp"
#include "socks5_type.h"
// linux only
#if TARGET_PLATFORM_LINUX
#include <arpa/inet.h>
#else
#include<WS2tcpip.h>
#endif
namespace SocksV5
{

static inline bool inet_pton_imp(const char *ip_addr,void * dst_addr,bool is_ipv4) {
#if TARGET_PLATFORM_LINUX
    return inet_pton(is_ipv4 ? AF_INET : AF_INET6, ip_addr, dst_addr)==1;
#else
    return inet_pton(is_ipv4 ? AF_INET : AF_INET6, ip_addr, dst_addr) == 1;
#endif
}

class socks5_proxy_imp : public network::network_upgrade_interface {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<socks5_proxy_imp>(std::forward<Args>(args)...);
    }
    template<typename port_type>
    socks5_proxy_imp(const std::string& host,
                     port_type port,
                     const std::string& user,
                     const std::string& passwd,
                     Socks5HostType host_type = Socks5HostType::DoMainName) :
    dst_host(host), dst_port(static_cast<std::uint16_t>(port)), username(user), passwd(passwd), host_type(host_type) {
    }
    const char* interface_name() const override {
        return "socks5";
    }
    void abort_all_operation() override {
        if (abort_cb_) abort_cb_();
    }
    void start() override {
        FASSERT(read_cb_ && write_cb_ && finish_cb_, "call proxy init first");
        // check user info
        if (username.size() > std::numeric_limits<std::uint8_t>::max() ||
            passwd.size() > std::numeric_limits<std::uint8_t>::max()) {
            finish_cb_(std::make_error_code(std::errc::invalid_argument), "user info overflow");
            return;
        }
        FDEBUG("request socks5 proxy to {} {}",dst_host,dst_port);
        greeting();
    }

protected:
    void greeting() {
        sendBufCache.resize(5);
        std::size_t offset     = 0;
        sendBufCache[offset++] = SocksVersion::V5;
        sendBufCache[offset++] = 1;
        sendBufCache[offset++] = Method::NoAuth;
        if (!username.empty()) {
            sendBufCache[1]        = sendBufCache[1] + 1;
            sendBufCache[offset++] = Method::UserPassWd;
        }
        network::write_buffer_t write_buffers;
        using value_type = network::write_buffer_t::value_type;
        write_buffers.emplace_back(value_type { sendBufCache.data(), offset });
        write_cb_(std::move(write_buffers),
                  [this, ptr = shared_from_this()](std::error_code ec, const std::string& msg) {
                      // failed
                      if (ec.value() != kSuccessOpCode) {
                          finish_cb_(ec, msg);
                          return;
                      }
                      handle_greeting_response();
                  });
    }

    void handle_greeting_response() {
        // ver auth_method
        recvBufCache.resize(2);
        network::read_buffer_t read_buffers;
        using value_type = network::read_buffer_t::value_type;
        read_buffers.emplace_back(value_type { recvBufCache.data(), recvBufCache.size() });
        read_cb_(std::move(read_buffers), [this, ptr = shared_from_this()](std::error_code ec, const std::string& msg) {
            // failed
            if (ec.value() != kSuccessOpCode) {
                finish_cb_(ec, msg);
                return;
            }
            if (recvBufCache[0] != SocksVersion::V5) {
                finish_cb_(std::make_error_code(std::errc::not_supported), "ver not supported");
                return;
            }
            switch (recvBufCache[1]) {
            case Method::NoAuth: socks5_request(); break;
            case Method::GSSAPI: finish_cb_(std::make_error_code(std::errc::not_supported), "ver not supported"); break;
            case Method::UserPassWd: verify_user_info(); break;
            default: finish_cb_(std::make_error_code(std::errc::not_supported), "ver not supported"); break;
            }
        });
    }
    void verify_user_info() {
        std::size_t packet_len = 1 /*ver*/ + 1 /*username len*/ + username.size() + 1 /*passwd len*/ + passwd.size();
        sendBufCache.resize(packet_len);
        std::size_t offset     = 0;
        sendBufCache[offset++] = SocksVersion::V5;
        sendBufCache[offset++] = static_cast<std::uint8_t>(username.size());
        std::memcpy(sendBufCache.data() + offset, username.data(), username.size());
        offset += username.size();
        sendBufCache[offset++] =static_cast<std::uint8_t>( passwd.size());
        std::memcpy(sendBufCache.data() + offset, passwd.data(), passwd.size());
        offset += passwd.size();
        network::write_buffer_t write_buffers;
        using value_type = network::write_buffer_t::value_type;
        write_buffers.emplace_back(value_type { sendBufCache.data(), offset });
        write_cb_(std::move(write_buffers),
                  [this, ptr = shared_from_this()](std::error_code ec, const std::string& msg) {
                      // failed
                      if (ec.value() != kSuccessOpCode) {
                          finish_cb_(ec, msg);
                          return;
                      }
                      handle_verify_user_info_response();
                  });
    }
    void handle_verify_user_info_response() {
        recvBufCache.resize(2);
        network::read_buffer_t read_buffers;
        using value_type = network::read_buffer_t::value_type;
        read_buffers.emplace_back(value_type { recvBufCache.data(), recvBufCache.size() });
        read_cb_(std::move(read_buffers), [this, ptr = shared_from_this()](std::error_code ec, const std::string& msg) {
            // failed
            if (ec.value() != kSuccessOpCode) {
                finish_cb_(ec, msg);
                return;
            }
            if (recvBufCache[0] != SocksVersion::V5) {
                finish_cb_(std::make_error_code(std::errc::not_supported), "ver not supported");
                return;
            }
            switch (recvBufCache[1]) {
            case ReplyAuthStatus::Success: socks5_request(); break;
            default: finish_cb_(std::make_error_code(std::errc::permission_denied), "auth failed"); break;
            }
        });
    }
    void socks5_request() {
        std::size_t packet_max_len =
            1 /*ver*/ + 1 /*cmd*/ + 1 /*reserve*/ + 1 /*type*/ + 257 /*4 ipv4 16->ipv6  257->domain*/ + 2 /*port*/;
        sendBufCache.resize(packet_max_len);
        std::size_t offset     = 0;
        sendBufCache[offset++] = SocksVersion::V5;
        sendBufCache[offset++] = RequestCMD::Connect;
        sendBufCache[offset++] = 0x00; // reserved
        sendBufCache[offset++] = host_type;
        switch (host_type) {
        case Socks5HostType::Ipv4: {
            if (!inet_pton_imp( dst_host.c_str(), sendBufCache.data() + offset,true)) {
                finish_cb_(std::make_error_code(std::errc::invalid_argument), "Invalid IPv4 address");
                return;
            }
            offset += 4;
        } break;
        case Socks5HostType::Ipv6: {
            if (!inet_pton_imp(dst_host.c_str(), sendBufCache.data() + offset, false)) {
                finish_cb_(std::make_error_code(std::errc::invalid_argument), "Invalid IPv6 address");
                return;
            }
            offset += 16;
        } break;
        default: {
            sendBufCache[offset++] = static_cast<std::uint8_t>(dst_host.size());
            std::memcpy(sendBufCache.data() + offset, dst_host.data(), dst_host.size());
            offset += dst_host.size();
        } break;
        }
        // set big-endian port
        sendBufCache[offset++] = static_cast<std::uint8_t>(dst_port >> 8);
        sendBufCache[offset++] = static_cast<std::uint8_t>(dst_port & 0xff);
        network::write_buffer_t write_buffers;
        using value_type = network::write_buffer_t::value_type;
        write_buffers.emplace_back(value_type { sendBufCache.data(), offset });
        write_cb_(std::move(write_buffers),
                  [this, ptr = shared_from_this()](std::error_code ec, const std::string& msg) {
                      // failed
                      if (ec.value() != kSuccessOpCode) {
                          finish_cb_(ec, msg);
                          return;
                      }
                      handle_socks5_request_response();
                  });
    }
    void handle_socks5_request_response() {
        // ver response_status reserve addr_type
        recvBufCache.resize(4);
        network::read_buffer_t read_buffers;
        using value_type = network::read_buffer_t::value_type;
        read_buffers.emplace_back(value_type { recvBufCache.data(), recvBufCache.size() });
        read_cb_(std::move(read_buffers), [this, ptr = shared_from_this()](std::error_code ec, const std::string& msg) {
            // failed
            if (ec.value() != kSuccessOpCode) {
                finish_cb_(ec, msg);
                return;
            }
            if (recvBufCache[0] != SocksVersion::V5) {
                finish_cb_(std::make_error_code(std::errc::not_supported), "ver not supported");
                return;
            }
            if (recvBufCache[1] != ReplyREP::Succeeded) {
                finish_cb_(
                    std::make_error_code(std::errc::address_not_available),
                    Fundamental::StringFormat("connected failed {}", static_cast<std::uint32_t>(recvBufCache[1])));
                return;
            }
            peek_remote_connect_info(static_cast<Socks5HostType>(recvBufCache[3]));
        });
    }
    void peek_remote_connect_info(Socks5HostType type) {
        switch (type) {
        case Socks5HostType::Ipv4: recvBufCache.resize(4 + 2); break;
        case Socks5HostType::Ipv6: recvBufCache.resize(16 + 2); break;
        default: finish_cb_(std::make_error_code(std::errc::not_supported), "type not supported"); return;
        }
        network::read_buffer_t read_buffers;
        using value_type = network::read_buffer_t::value_type;
        read_buffers.emplace_back(value_type { recvBufCache.data(), recvBufCache.size() });
        read_cb_(std::move(read_buffers),
                 [this, ptr = shared_from_this(), type](std::error_code ec, const std::string& msg) {
                     // failed
                     if (ec.value() != kSuccessOpCode) {
                         finish_cb_(ec, msg);
                         return;
                     }
                     auto convert_func = [&]() -> std::string {
                         char addr[UINT8_MAX];
                         std::memset(addr, 0, sizeof(addr));

                         switch (type) {
                         case Socks5HostType::Ipv4: {
                             std::snprintf(addr, sizeof(addr), "%d.%d.%d.%d", recvBufCache[0], recvBufCache[1],
                                           recvBufCache[2], recvBufCache[3]);
                         } break;

                         case Socks5HostType::Ipv6: {
                             std::snprintf(addr, sizeof(addr),
                                           "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%"
                                           "02x:%02x%02x:%"
                                           "02x%02x",
                                           recvBufCache[0], recvBufCache[1], recvBufCache[2], recvBufCache[3],
                                           recvBufCache[4], recvBufCache[5], recvBufCache[6], recvBufCache[7],
                                           recvBufCache[8], recvBufCache[9], recvBufCache[10], recvBufCache[11],
                                           recvBufCache[12], recvBufCache[13], recvBufCache[14], recvBufCache[15]);
                         } break;

                         default: {
                         } break;
                         }
                         return std::string(addr);
                     };

                     std::string proxy_ip     = convert_func();
                     std::uint16_t proxy_port = recvBufCache[recvBufCache.size() - 2];
                     proxy_port               = recvBufCache[recvBufCache.size() - 1] | (proxy_port << 8);
                     finish_cb_(ec, Fundamental::StringFormat("ip:{} port:{}", proxy_ip, proxy_port));
                 });
    }

private:
    const std::string dst_host;
    const std::uint16_t dst_port;
    const std::string username;
    const std::string passwd;
    const Socks5HostType host_type = Socks5HostType::DoMainName;

    std::vector<std::uint8_t> sendBufCache;
    std::vector<std::uint8_t> recvBufCache;
};
} // namespace SocksV5