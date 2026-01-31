//
// @author lightning1993 <469953258@qq.com> 2025/08
//
#pragma once

#include <cstddef>
#include <cstdint>

#include "fundamental/basic/error_code.hpp"
#include "fundamental/basic/utils.hpp"

namespace network::rudp
{
enum rudp_config_type : std::uint32_t
{
    RUDP_CONNECT_TIMEOUT_MS,
    RUDP_COMMAND_MAX_TRY_CNT,
    RUDP_MAX_SEND_WINDOW_SIZE,
    RUDP_MAX_RECV_WINDOW_SIZE,
    RUDP_MTU_SIZE,
    RUDP_ENABLE_NO_DELAY,
    RUDP_UPDATE_INTERVAL_MS,
    RUDP_FASK_RESEND_SKIP_CNT,
    RUDP_ENABLE_NO_CONGESTION_CONTROL,
    RUDP_ENABLE_AUTO_KEEPALIVE,
    RUDP_ENABLE_STREAM_MODE,
    RUDP_MAX_IDLE_CONNECTION_TIME_MS
};

namespace error
{
enum class rudp_errors : std::int32_t
{
    rudp_failed                  = -1,
    rudp_success                 = 0,
    rudp_broken_pipe             = static_cast<std::int32_t>(std::errc::broken_pipe),
    rudp_no_buffer_space         = static_cast<std::int32_t>(std::errc::no_buffer_space),
    rudp_timed_out               = static_cast<std::int32_t>(std::errc::timed_out),
    rudp_not_connected           = static_cast<std::int32_t>(std::errc::not_connected),
    rudp_already_connected       = static_cast<std::int32_t>(std::errc::already_connected),
    rudp_bad_file_descriptor     = static_cast<std::int32_t>(std::errc::bad_file_descriptor),
    rudp_invalid_argument        = static_cast<std::int32_t>(std::errc::invalid_argument),
    rudp_device_or_resource_busy = static_cast<std::int32_t>(std::errc::device_or_resource_busy),
    rudp_operation_canceled      = static_cast<std::int32_t>(std::errc::operation_canceled),
    rudp_operation_in_progress   = static_cast<std::int32_t>(std::errc::operation_in_progress),
    rudp_connection_reset        = static_cast<std::int32_t>(std::errc::connection_reset),
    rudp_network_unreachable     = static_cast<std::int32_t>(std::errc::network_unreachable),
};

class rudp_category : public std::error_category, public Fundamental::Singleton<rudp_category> {
public:
    const char* name() const noexcept override {
        return "network.rudp";
    }
    std::string message(int value) const override {
        switch (static_cast<rudp_errors>(value)) {
        case rudp_errors::rudp_failed: return "rudp failed";
        case rudp_errors::rudp_success: return "rudp success";
        case rudp_errors::rudp_broken_pipe: return "rudp broken pipe";
        case rudp_errors::rudp_no_buffer_space: return "rudp no buffer space";
        case rudp_errors::rudp_timed_out: return "rudp timeout";
        case rudp_errors::rudp_not_connected: return "rudp not connected";
        case rudp_errors::rudp_already_connected: return "rudp already connected";
        case rudp_errors::rudp_bad_file_descriptor: return "rudp bad file descriptor";
        case rudp_errors::rudp_invalid_argument: return "rudp invalid argument";
        case rudp_errors::rudp_device_or_resource_busy: return "rudp device ro resource busy";
        case rudp_errors::rudp_operation_canceled: return "rudp operation canceled";
        case rudp_errors::rudp_operation_in_progress: return "rudp operation in progress";
        case rudp_errors::rudp_connection_reset: return "rudp connection reset";
        case rudp_errors::rudp_network_unreachable: return "rudp unreachable";
        default: return "network.rudp error";
        }
    }
};

inline Fundamental::error_code make_error_code(rudp_errors e, std::string_view details = "") {
    return Fundamental::error_code(static_cast<int>(e), rudp_category::Instance(), details);
}

inline Fundamental::error_code make_error_code(const std::error_code& ec, std::string_view details) {
    return Fundamental::error_code(ec, details);
}
} // namespace error
} // namespace network::rudp