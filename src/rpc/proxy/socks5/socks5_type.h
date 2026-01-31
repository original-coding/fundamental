#pragma once
#include <cstddef>
#include <cstdint>
namespace SocksV5
{
// clang-format off

/* SOCKSv5 Protocol Supported Methods Field */
enum  Method : uint8_t {
    NoAuth                  = 0x00,
    GSSAPI                  = 0x01,
    UserPassWd              = 0x02,
    NoAcceptable            = 0xFF,
};

/* SOCKSv5 Server Reply Autu Status */
enum  ReplyAuthStatus : uint8_t {
    Success                 = 0x00,
    Failure                 = 0xFF,
};

/* SOCKSv5 Client Request CMD Field */
enum  RequestCMD : uint8_t {
    Connect                 = 0x01,
    Bind                    = 0x02,
    UdpAssociate            = 0x03,
};

enum  Socks5HostType : uint8_t {
    Ipv4                    = 0x01,
    DoMainName              = 0x03,
    Ipv6                    = 0x04,
};

/* SOCKSv5 Server Reply REP Field */
enum  ReplyREP : uint8_t {
    Succeeded               = 0x00,
    GenServFailed           = 0x01,
    NotAllowed              = 0x02,
    NetworkUnreachable      = 0x03,
    HostUnreachable         = 0x04,
    ConnRefused             = 0x05,
    TtlExpired              = 0x06,
    CommandNotSupported     = 0x07,
    AddrTypeNotSupported    = 0x08,
};

/* SOCKS Protocol Version Field */
enum SocksVersion : uint8_t
{
    V4 = 0x04,
    V5 = 0x05,
};

// clang-format on

} // namespace SocksV5
