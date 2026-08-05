#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace network::proxy
{

// KCP traffic encryption using AES-256-CTR (symmetric stream cipher).
// No authentication tag -- KCP provides its own integrity checks.
// Key derivation using HKDF-SHA256 from traffic_secret + salt.
//
// Default salt "frp-default" is used for general-purpose traffic (probes,
// startup NAT detection, time sync).
// For data channels, pass a per-channel salt (e.g. connection_uuid) so each
// channel derives an independent key.

constexpr std::size_t FRP_KCP_KEY_SIZE = 32;  // AES-256 key size
constexpr std::size_t FRP_KCP_IV_SIZE  = 16;  // AES-CTR IV size

// Derive KCP encryption key from traffic_secret and salt.
// HKDF-SHA256 with fixed salt "frp-kcp-v1", info = salt parameter.
// Both sides call this with the same salt -> identical key (symmetric).
std::vector<std::uint8_t> frp_derive_kcp_key(
    const std::string& traffic_secret,
    const std::string& salt = "frp-default");

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

} // namespace network::proxy
