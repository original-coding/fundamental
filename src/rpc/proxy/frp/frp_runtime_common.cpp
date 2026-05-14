#include "frp_runtime_common.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

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

} // namespace network::proxy
