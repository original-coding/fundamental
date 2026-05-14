#include "frp_runtime_common.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#ifdef _MSC_VER
#include <windows.h>
#endif

namespace network::proxy
{

std::string frp_generate_runtime_uuid() {
    return Fundamental::to_string(Fundamental::GenerateUUID());
}

std::string frp_generate_server_nonce() {
    return frp_generate_runtime_uuid();
}

std::string frp_hmac_sha256_hex(std::string_view secret, std::string_view payload) {
    unsigned int out_len = 0;
    unsigned char out[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(),
         secret.data(),
         static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(payload.data()),
         payload.size(),
         out,
         &out_len);
    return Fundamental::Utils::BufferToHex(out, out_len);
}

std::string safe_error_code_message(const std::error_code& ec) {
    std::string msg = ec.message();
#ifdef _MSC_VER
    // On Windows, std::error_code::message() returns text in the system ANSI
    // code page (e.g. GBK on Chinese Windows). Convert to UTF-8 so that
    // nlohmann::json::dump() does not throw type_error.316 on non-UTF-8 bytes.
    if (msg.empty()) return msg;
    int wide_len = MultiByteToWideChar(CP_ACP, 0, msg.c_str(),
                                        static_cast<int>(msg.size()), nullptr, 0);
    if (wide_len <= 0) return std::to_string(ec.value());
    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_ACP, 0, msg.c_str(),
                         static_cast<int>(msg.size()), &wide[0], wide_len);
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len,
                                        nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return std::to_string(ec.value());
    std::string utf8_str(static_cast<std::size_t>(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len,
                         &utf8_str[0], utf8_len, nullptr, nullptr);
    return utf8_str;
#else
    return msg;
#endif
}

} // namespace network::proxy
