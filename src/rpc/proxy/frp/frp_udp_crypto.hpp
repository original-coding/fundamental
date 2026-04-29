#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace network::proxy
{

// UDP packet encryption using ChaCha20-Poly1305 AEAD
// Key derivation using HKDF-SHA256 from traffic_secret

constexpr std::size_t FRP_UDP_KEY_SIZE = 32;  // ChaCha20 key size
constexpr std::size_t FRP_UDP_NONCE_SIZE = 12; // ChaCha20-Poly1305 nonce size
constexpr std::size_t FRP_UDP_TAG_SIZE = 16;   // Poly1305 tag size

// Derive a single shared UDP key from traffic_secret.
// All UDP packets (probe and data) use this key.
std::vector<std::uint8_t> frp_derive_traffic_key(const std::string& traffic_secret);

// Derive per-flow UDP encryption key from traffic_secret
// Uses HKDF-SHA256 with flow_id and direction as context
std::vector<std::uint8_t> frp_derive_udp_flow_key(
    const std::string& traffic_secret,
    std::uint32_t flow_id,
    bool is_sender);

// Encrypt UDP packet using ChaCha20-Poly1305
// Returns: nonce (12 bytes) + ciphertext + tag (16 bytes)
// Returns empty vector on error
std::vector<std::uint8_t> frp_udp_encrypt(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& plaintext);

// Decrypt UDP packet using ChaCha20-Poly1305
// Input: nonce (12 bytes) + ciphertext + tag (16 bytes)
// Returns plaintext on success, empty optional on error
std::optional<std::vector<std::uint8_t>> frp_udp_decrypt(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& encrypted_packet);

// Helper: encrypt string data
inline std::vector<std::uint8_t> frp_udp_encrypt_string(
    const std::vector<std::uint8_t>& key,
    const std::string& plaintext) {
    std::vector<std::uint8_t> data(plaintext.begin(), plaintext.end());
    return frp_udp_encrypt(key, data);
}

// Helper: decrypt to string
inline std::optional<std::string> frp_udp_decrypt_string(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& encrypted_packet) {
    auto plaintext = frp_udp_decrypt(key, encrypted_packet);
    if (!plaintext) return std::nullopt;
    return std::string(plaintext->begin(), plaintext->end());
}

} // namespace network::proxy
