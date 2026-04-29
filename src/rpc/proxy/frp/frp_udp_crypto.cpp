#include "frp_udp_crypto.hpp"

#include "fundamental/basic/log.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <cstring>

namespace network::proxy
{

std::vector<std::uint8_t> frp_derive_traffic_key(const std::string& traffic_secret) {
    std::vector<std::uint8_t> key(FRP_UDP_KEY_SIZE);

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!pctx) return {};
    if (EVP_PKEY_derive_init(pctx) <= 0) { EVP_PKEY_CTX_free(pctx); return {}; }
    if (EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0) { EVP_PKEY_CTX_free(pctx); return {}; }

    const char* salt = "frp-udp-v1";
    if (EVP_PKEY_CTX_set1_hkdf_salt(pctx,
            reinterpret_cast<const unsigned char*>(salt), std::strlen(salt)) <= 0) {
        EVP_PKEY_CTX_free(pctx); return {};
    }
    if (EVP_PKEY_CTX_set1_hkdf_key(pctx,
            reinterpret_cast<const unsigned char*>(traffic_secret.data()),
            traffic_secret.size()) <= 0) {
        EVP_PKEY_CTX_free(pctx); return {};
    }
    const char* info = "frp-traffic";
    if (EVP_PKEY_CTX_add1_hkdf_info(pctx,
            reinterpret_cast<const unsigned char*>(info), std::strlen(info)) <= 0) {
        EVP_PKEY_CTX_free(pctx); return {};
    }
    std::size_t keylen = key.size();
    if (EVP_PKEY_derive(pctx, key.data(), &keylen) <= 0) { EVP_PKEY_CTX_free(pctx); return {}; }
    EVP_PKEY_CTX_free(pctx);
    return key;
}

std::vector<std::uint8_t> frp_derive_udp_flow_key(
    const std::string& traffic_secret,
    std::uint32_t flow_id,
    bool is_sender) {

    std::vector<std::uint8_t> key(FRP_UDP_KEY_SIZE);

    // Create HKDF context
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!pctx) {
        FERR("frp_derive_udp_flow_key: EVP_PKEY_CTX_new_id failed");
        return {};
    }

    // Initialize HKDF
    if (EVP_PKEY_derive_init(pctx) <= 0) {
        FERR("frp_derive_udp_flow_key: EVP_PKEY_derive_init failed");
        EVP_PKEY_CTX_free(pctx);
        return {};
    }

    // Set hash function to SHA256
    if (EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0) {
        FERR("frp_derive_udp_flow_key: EVP_PKEY_CTX_set_hkdf_md failed");
        EVP_PKEY_CTX_free(pctx);
        return {};
    }

    // Set salt
    const char* salt = "frp-udp-v1";
    if (EVP_PKEY_CTX_set1_hkdf_salt(pctx,
            reinterpret_cast<const unsigned char*>(salt),
            std::strlen(salt)) <= 0) {
        FERR("frp_derive_udp_flow_key: EVP_PKEY_CTX_set1_hkdf_salt failed");
        EVP_PKEY_CTX_free(pctx);
        return {};
    }

    // Set input key material (traffic_secret)
    if (EVP_PKEY_CTX_set1_hkdf_key(pctx,
            reinterpret_cast<const unsigned char*>(traffic_secret.data()),
            traffic_secret.size()) <= 0) {
        FERR("frp_derive_udp_flow_key: EVP_PKEY_CTX_set1_hkdf_key failed");
        EVP_PKEY_CTX_free(pctx);
        return {};
    }

    // Set info: "flow_id:direction"
    std::string info = std::to_string(flow_id) + ":" + (is_sender ? "send" : "recv");
    if (EVP_PKEY_CTX_add1_hkdf_info(pctx,
            reinterpret_cast<const unsigned char*>(info.data()),
            info.size()) <= 0) {
        FERR("frp_derive_udp_flow_key: EVP_PKEY_CTX_add1_hkdf_info failed");
        EVP_PKEY_CTX_free(pctx);
        return {};
    }

    // Derive key
    std::size_t keylen = key.size();
    if (EVP_PKEY_derive(pctx, key.data(), &keylen) <= 0) {
        FERR("frp_derive_udp_flow_key: EVP_PKEY_derive failed");
        EVP_PKEY_CTX_free(pctx);
        return {};
    }

    EVP_PKEY_CTX_free(pctx);
    FINFO("frp_derive_udp_flow_key flow_id={} direction={} key_size={}", flow_id, is_sender ? "send" : "recv", key.size());
    return key;
}

std::vector<std::uint8_t> frp_udp_encrypt(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& plaintext) {

    if (key.size() != FRP_UDP_KEY_SIZE) {
        FERR("frp_udp_encrypt: invalid key size {}", key.size());
        return {};
    }

    // Generate random nonce
    std::vector<std::uint8_t> nonce(FRP_UDP_NONCE_SIZE);
    if (RAND_bytes(nonce.data(), nonce.size()) != 1) {
        FERR("frp_udp_encrypt: RAND_bytes failed");
        return {};
    }

    // Create cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        FERR("frp_udp_encrypt: EVP_CIPHER_CTX_new failed");
        return {};
    }

    // Initialize encryption with ChaCha20-Poly1305
    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr,
            key.data(), nonce.data()) != 1) {
        FERR("frp_udp_encrypt: EVP_EncryptInit_ex failed");
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Prepare output buffer: nonce + ciphertext + tag
    std::vector<std::uint8_t> output;
    output.reserve(FRP_UDP_NONCE_SIZE + plaintext.size() + FRP_UDP_TAG_SIZE);

    // Copy nonce to output
    output.insert(output.end(), nonce.begin(), nonce.end());

    // Encrypt plaintext
    std::vector<std::uint8_t> ciphertext(plaintext.size() + EVP_CIPHER_CTX_block_size(ctx));
    int len = 0;
    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
            plaintext.data(), plaintext.size()) != 1) {
        FERR("frp_udp_encrypt: EVP_EncryptUpdate failed");
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    int ciphertext_len = len;

    // Finalize encryption
    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
        FERR("frp_udp_encrypt: EVP_EncryptFinal_ex failed");
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    ciphertext_len += len;

    // Append ciphertext to output
    output.insert(output.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);

    // Get authentication tag
    std::vector<std::uint8_t> tag(FRP_UDP_TAG_SIZE);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, FRP_UDP_TAG_SIZE, tag.data()) != 1) {
        FERR("frp_udp_encrypt: EVP_CIPHER_CTX_ctrl GET_TAG failed");
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Append tag to output
    output.insert(output.end(), tag.begin(), tag.end());

    EVP_CIPHER_CTX_free(ctx);
    FINFO("frp_udp_encrypt plaintext_size={} output_size={}", plaintext.size(), output.size());
    return output;
}

std::optional<std::vector<std::uint8_t>> frp_udp_decrypt(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& encrypted_packet) {

    if (key.size() != FRP_UDP_KEY_SIZE) {
        FERR("frp_udp_decrypt: invalid key size {}", key.size());
        return std::nullopt;
    }

    // Minimum size: nonce + tag
    if (encrypted_packet.size() < FRP_UDP_NONCE_SIZE + FRP_UDP_TAG_SIZE) {
        FERR("frp_udp_decrypt: packet too small {}", encrypted_packet.size());
        return std::nullopt;
    }

    // Extract nonce
    std::vector<std::uint8_t> nonce(
        encrypted_packet.begin(),
        encrypted_packet.begin() + FRP_UDP_NONCE_SIZE);

    // Extract ciphertext (between nonce and tag)
    std::size_t ciphertext_len = encrypted_packet.size() - FRP_UDP_NONCE_SIZE - FRP_UDP_TAG_SIZE;
    const std::uint8_t* ciphertext_ptr = encrypted_packet.data() + FRP_UDP_NONCE_SIZE;

    // Extract tag
    std::vector<std::uint8_t> tag(
        encrypted_packet.end() - FRP_UDP_TAG_SIZE,
        encrypted_packet.end());

    // Create cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        FERR("frp_udp_decrypt: EVP_CIPHER_CTX_new failed");
        return std::nullopt;
    }

    // Initialize decryption with ChaCha20-Poly1305
    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr,
            key.data(), nonce.data()) != 1) {
        FERR("frp_udp_decrypt: EVP_DecryptInit_ex failed");
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }

    // Decrypt ciphertext
    std::vector<std::uint8_t> plaintext(ciphertext_len + EVP_CIPHER_CTX_block_size(ctx));
    int len = 0;
    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len,
            ciphertext_ptr, ciphertext_len) != 1) {
        FERR("frp_udp_decrypt: EVP_DecryptUpdate failed");
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }
    int plaintext_len = len;

    // Set expected tag
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, FRP_UDP_TAG_SIZE,
            const_cast<std::uint8_t*>(tag.data())) != 1) {
        FERR("frp_udp_decrypt: EVP_CIPHER_CTX_ctrl SET_TAG failed");
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }

    // Finalize decryption (verifies tag)
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        FINFO("frp_udp_decrypt: AEAD tag verification failed packet_size={}", encrypted_packet.size());
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }
    plaintext_len += len;

    plaintext.resize(plaintext_len);
    EVP_CIPHER_CTX_free(ctx);
    FINFO("frp_udp_decrypt ok packet_size={} plaintext_size={}", encrypted_packet.size(), plaintext_len);
    return plaintext;
}

} // namespace network::proxy
