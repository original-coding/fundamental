#pragma once

#include <asio.hpp>
#include <string>
#include <string_view>

namespace network::http
{
enum MethodFilter : std::uint32_t
{
    HttpNone     = 0,
    HttpGet      = (1U << 0),
    HttpPost     = (1U << 1),
    HttpPut      = (1U << 2),
    HttpDelete   = (1U << 3),
    HttpPatch    = (1U << 4),
    HttpCopy     = (1U << 5),
    HttpHead     = (1U << 6),
    HttpOptions  = (1U << 7),
    HttpLink     = (1U << 8),
    HttpUnlink   = (1U << 9),
    HttpPurge    = (1U << 10),
    HttpLock     = (1U << 11),
    HttpUnlock   = (1U << 12),
    HttpPropfind = (1U << 13),
    HttpView     = (1U << 14),
    HttpAll      = static_cast<std::uint32_t>(~0),
};

struct http_header {
    std::string name;
    std::string value;
};
/// Convert a file extension into a MIME type.
std::string ExtensionToType(const std::string& extension);

struct http_server_config {
    std::uint16_t port       = 8000;
    std::size_t timeout_msec = 30000;
    bool head_case_sensitive = false;
    std::string root_path;
};

static constexpr std::size_t kInvalidHttpContentLength = std::numeric_limits<std::size_t>::max();

} // namespace network::http