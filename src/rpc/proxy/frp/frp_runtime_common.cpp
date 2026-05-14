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
    if (msg.empty()) return msg;

    // If the message is already valid UTF-8 (modern Windows with UTF-8
    // code page, or ASCII-only text), return it unchanged to avoid
    // double-encoding that turns CJK characters into mojibake.
    auto is_valid_utf8 = [](const std::string& s) {
        for (std::size_t i = 0; i < s.size();) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c <= 0x7F) { ++i; }
            else if (c >= 0xC2 && c <= 0xDF) {
                if (i + 1 >= s.size() || (static_cast<unsigned char>(s[i+1]) >> 6) != 2) return false;
                i += 2;
            } else if (c >= 0xE0 && c <= 0xEF) {
                if (i + 2 >= s.size()
                    || (static_cast<unsigned char>(s[i+1]) >> 6) != 2
                    || (static_cast<unsigned char>(s[i+2]) >> 6) != 2) return false;
                if (c == 0xE0 && static_cast<unsigned char>(s[i+1]) < 0xA0) return false;
                if (c == 0xED && static_cast<unsigned char>(s[i+1]) > 0x9F) return false;
                i += 3;
            } else if (c >= 0xF0 && c <= 0xF4) {
                if (i + 3 >= s.size()
                    || (static_cast<unsigned char>(s[i+1]) >> 6) != 2
                    || (static_cast<unsigned char>(s[i+2]) >> 6) != 2
                    || (static_cast<unsigned char>(s[i+3]) >> 6) != 2) return false;
                if (c == 0xF0 && static_cast<unsigned char>(s[i+1]) < 0x90) return false;
                if (c == 0xF4 && static_cast<unsigned char>(s[i+1]) > 0x8F) return false;
                i += 4;
            } else {
                return false;
            }
        }
        return true;
    };
    if (is_valid_utf8(msg)) return msg;

    // Not valid UTF-8 — likely ANSI code page (e.g. GBK on Chinese Windows).
    // Convert to UTF-8 via wide-char so nlohmann::json::dump() won't throw.
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
