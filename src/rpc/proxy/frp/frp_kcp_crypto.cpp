#include "frp_kcp_crypto.hpp"

#include "fundamental/basic/log.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <cstring>

namespace network::proxy
{

std::vector<std::uint8_t> frp_derive_kcp_flow_key(
    const std::string& traffic_secret,
    std::uint32_t flow_id) {

    std::vector<std::uint8_t> key(FRP_KCP_KEY_SIZE);

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!pctx) {
        FERR("frp_derive_kcp_flow_key: EVP_PKEY_CTX_new_id failed");
        return {};
    }
    if (EVP_PKEY_derive_init(pctx) <= 0) {
        FERR("frp_derive_kcp_flow_key: EVP_PKEY_derive_init failed");
        EVP_PKEY_CTX_free(pctx); return {};
    }
    if (EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0) {
        FERR("frp_derive_kcp_flow_key: EVP_PKEY_CTX_set_hkdf_md failed");
        EVP_PKEY_CTX_free(pctx); return {};
    }

    const char* salt = "frp-kcp-v1";
    if (EVP_PKEY_CTX_set1_hkdf_salt(pctx,
            reinterpret_cast<const unsigned char*>(salt), static_cast<int>(std::strlen(salt))) <= 0) {
        FERR("frp_derive_kcp_flow_key: EVP_PKEY_CTX_set1_hkdf_salt failed");
        EVP_PKEY_CTX_free(pctx); return {};
    }
    if (EVP_PKEY_CTX_set1_hkdf_key(pctx,
            reinterpret_cast<const unsigned char*>(traffic_secret.data()),
            static_cast<int>(traffic_secret.size())) <= 0) {
        FERR("frp_derive_kcp_flow_key: EVP_PKEY_CTX_set1_hkdf_key failed");
        EVP_PKEY_CTX_free(pctx); return {};
    }

    std::string info = "flow:" + std::to_string(flow_id);
    if (EVP_PKEY_CTX_add1_hkdf_info(pctx,
            reinterpret_cast<const unsigned char*>(info.data()),
            static_cast<int>(info.size())) <= 0) {
        FERR("frp_derive_kcp_flow_key: EVP_PKEY_CTX_add1_hkdf_info failed");
        EVP_PKEY_CTX_free(pctx); return {};
    }

    std::size_t keylen = key.size();
    if (EVP_PKEY_derive(pctx, key.data(), &keylen) <= 0) {
        FERR("frp_derive_kcp_flow_key: EVP_PKEY_derive failed");
        EVP_PKEY_CTX_free(pctx); return {};
    }
    EVP_PKEY_CTX_free(pctx);
    FINFO("frp_derive_kcp_flow_key flow_id={} key_size={}", flow_id, key.size());
    return key;
}

std::vector<std::uint8_t> frp_kcp_encrypt(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& plaintext) {

    if (key.size() != FRP_KCP_KEY_SIZE) {
        FERR("frp_kcp_encrypt: invalid key size {}", key.size());
        return {};
    }

    // Generate random IV
    std::vector<std::uint8_t> iv(FRP_KCP_IV_SIZE);
    if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) {
        FERR("frp_kcp_encrypt: RAND_bytes failed");
        return {};
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        FERR("frp_kcp_encrypt: EVP_CIPHER_CTX_new failed");
        return {};
    }

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr,
            key.data(), iv.data()) != 1) {
        FERR("frp_kcp_encrypt: EVP_EncryptInit_ex failed");
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Output: IV + ciphertext
    std::vector<std::uint8_t> output;
    output.reserve(FRP_KCP_IV_SIZE + plaintext.size());
    output.insert(output.end(), iv.begin(), iv.end());

    std::vector<std::uint8_t> ciphertext(plaintext.size() + EVP_CIPHER_CTX_block_size(ctx));
    int len = 0;
    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
            plaintext.data(), static_cast<int>(plaintext.size())) != 1) {
        FERR("frp_kcp_encrypt: EVP_EncryptUpdate failed");
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    int ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
        FERR("frp_kcp_encrypt: EVP_EncryptFinal_ex failed");
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    ciphertext_len += len;

    output.insert(output.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);

    EVP_CIPHER_CTX_free(ctx);
    FINFO("frp_kcp_encrypt plaintext_size={} output_size={}", plaintext.size(), output.size());
    return output;
}

std::optional<std::vector<std::uint8_t>> frp_kcp_decrypt(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& encrypted_packet) {

    if (key.size() != FRP_KCP_KEY_SIZE) {
        FERR("frp_kcp_decrypt: invalid key size {}", key.size());
        return std::nullopt;
    }

    if (encrypted_packet.size() < FRP_KCP_IV_SIZE) {
        FERR("frp_kcp_decrypt: packet too small {}", encrypted_packet.size());
        return std::nullopt;
    }

    // Extract IV
    std::vector<std::uint8_t> iv(
        encrypted_packet.begin(),
        encrypted_packet.begin() + FRP_KCP_IV_SIZE);

    const std::uint8_t* ciphertext_ptr = encrypted_packet.data() + FRP_KCP_IV_SIZE;
    std::size_t ciphertext_len = encrypted_packet.size() - FRP_KCP_IV_SIZE;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        FERR("frp_kcp_decrypt: EVP_CIPHER_CTX_new failed");
        return std::nullopt;
    }

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr,
            key.data(), iv.data()) != 1) {
        FERR("frp_kcp_decrypt: EVP_DecryptInit_ex failed");
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }

    std::vector<std::uint8_t> plaintext(ciphertext_len + EVP_CIPHER_CTX_block_size(ctx));
    int len = 0;
    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len,
            ciphertext_ptr, static_cast<int>(ciphertext_len)) != 1) {
        FERR("frp_kcp_decrypt: EVP_DecryptUpdate failed");
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }
    int plaintext_len = len;

    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        FERR("frp_kcp_decrypt: EVP_DecryptFinal_ex failed");
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }
    plaintext_len += len;

    plaintext.resize(plaintext_len);
    EVP_CIPHER_CTX_free(ctx);
    FINFO("frp_kcp_decrypt ok packet_size={} plaintext_size={}", encrypted_packet.size(), plaintext_len);
    return plaintext;
}

} // namespace network::proxy
