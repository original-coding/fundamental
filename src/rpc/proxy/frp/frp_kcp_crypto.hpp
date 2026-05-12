#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace network::proxy
{

// KCP traffic encryption using AES-256-CTR (symmetric stream cipher).
// No authentication tag -- KCP provides its own integrity checks.
// Key derivation using HKDF-SHA256 from traffic_secret + flow_id.
//
// flow_id=0 is used for non-flow traffic (probes, startup NAT detection).
// For data channels, the per-flow key is derived from traffic_secret + flow_id;
// both sides derive the identical key, achieving symmetric encryption.

constexpr std::size_t FRP_KCP_KEY_SIZE = 32;  // AES-256 key size
constexpr std::size_t FRP_KCP_IV_SIZE  = 16;  // AES-CTR IV size

// Derive per-flow KCP encryption key from traffic_secret and flow_id.
// HKDF-SHA256 with salt "frp-kcp-v1", info "flow:<flow_id>".
// Both sides call this with the same flow_id -> identical key (symmetric).
std::vector<std::uint8_t> frp_derive_kcp_flow_key(
    const std::string& traffic_secret,
    std::uint32_t flow_id);

// Encrypt plaintext using AES-256-CTR.
// Returns: IV (16 bytes) + ciphertext.
// Returns empty vector on error.
std::vector<std::uint8_t> frp_kcp_encrypt(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& plaintext);

// Decrypt packet using AES-256-CTR.
// Input: IV (16 bytes) + ciphertext.
// Returns plaintext on success, empty optional on error.
std::optional<std::vector<std::uint8_t>> frp_kcp_decrypt(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& encrypted_packet);

// Helper: encrypt string data
inline std::vector<std::uint8_t> frp_kcp_encrypt_string(
    const std::vector<std::uint8_t>& key,
    const std::string& plaintext) {
    std::vector<std::uint8_t> data(plaintext.begin(), plaintext.end());
    return frp_kcp_encrypt(key, data);
}

// Helper: decrypt to string
inline std::optional<std::string> frp_kcp_decrypt_string(
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& encrypted_packet) {
    auto plaintext = frp_kcp_decrypt(key, encrypted_packet);
    if (!plaintext) return std::nullopt;
    return std::string(plaintext->begin(), plaintext->end());
}

} // namespace network::proxy
