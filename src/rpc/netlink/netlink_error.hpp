#pragma once
#include "fundamental/basic/error_code.hpp"
#include "fundamental/basic/utils.hpp"

namespace network
{
namespace link
{
enum class netlink_errors : std::int32_t
{
    netlink_success             = 0,
    netlink_failed              = 1,
    netlink_unsupported_type    = 2,
    netlink_timeout             = 3,
    netlink_network_error       = 4,
    netlink_invalid_argument    = 5,
    netlink_operation_cancelled = 6,
};

class netlink_category : public std::error_category, public Fundamental::Singleton<netlink_category> {
public:
    const char* name() const noexcept override {
        return "netlink.opcode";
    }
    std::string message(int value) const override {
        switch (static_cast<netlink_errors>(value)) {
        case netlink_errors::netlink_success: return "success";
        case netlink_errors::netlink_failed: return "failed";
        case netlink_errors::netlink_unsupported_type: return "unsupported type";
        case netlink_errors::netlink_timeout: return "timeout";
        case netlink_errors::netlink_network_error: return "network error";
        case netlink_errors::netlink_invalid_argument: return "invalid argument";
        case netlink_errors::netlink_operation_cancelled: return "operation cancelled";
        default: return "netlink unknown error";
        }
    }
};

inline Fundamental::error_code make_error_code(netlink_errors e, std::string_view details = "") {
    return Fundamental::error_code(static_cast<int>(e), netlink_category::Instance(), details);
}
} // namespace link
} // namespace network