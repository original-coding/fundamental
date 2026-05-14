#pragma once

#include "fundamental/basic/uuid_utils.hpp"
#include "fundamental/basic/utils.hpp"

#include <string>
#include <string_view>
#include <system_error>

namespace network::proxy
{

std::string frp_generate_runtime_uuid();
std::string frp_generate_server_nonce();
std::string frp_hmac_sha256_hex(std::string_view secret, std::string_view payload);

// On Windows, std::error_code::message() returns text in the system ANSI code page
// (e.g., GBK on Chinese Windows), which causes nlohmann::json::dump() to throw
// type_error.316. This helper ensures the message is always valid UTF-8.
std::string safe_error_code_message(const std::error_code& ec);

} // namespace network::proxy
