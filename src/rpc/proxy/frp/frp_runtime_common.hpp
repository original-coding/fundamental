#pragma once

#include "fundamental/basic/uuid_utils.hpp"
#include "fundamental/basic/utils.hpp"

#include <string>
#include <string_view>

namespace network::proxy
{

std::string frp_generate_runtime_uuid();
std::string frp_generate_server_nonce();
std::string frp_hmac_sha256_hex(std::string_view secret, std::string_view payload);

} // namespace network::proxy
