#pragma once

#include "frp_kcp_crypto.hpp"

#include "network/network.hpp"
#include "network/rudp/kcp_imp/ikcp.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace network::proxy
{

// =============================================================================
// kcp_channel — self-contained KCP (reliable-UDP) wrapper
// =============================================================================
//
// Wraps IKCPCB lifecycle behind a simple feed/send interface:
//   - feed_encrypted(): ikcp_input encrypted bytes → decrypts internally →
//     emits plaintext frames via on_frame callback
//   - send_plaintext(): plaintext bytes → encrypts internally → ikcp_send →
//     encrypted output via on_output callback
//
// Transport-agnostic: caller injects callbacks for:
//   - on_frame: where decrypted plaintext goes (backend socket, local socket)
//   - on_output: where encrypted KCP output goes (TCP relay, UDP P2P)
//
// Self-driving: init() starts a 20ms update timer. Caller never calls tick().

class kcp_channel : public std::enable_shared_from_this<kcp_channel> {
public:
    kcp_channel(asio::any_io_executor executor, std::string traffic_secret, std::string salt);
    ~kcp_channel();

    // Create IKCPCB, derive traffic key, start self-driving update timer.
    // Idempotent — second call is a no-op.
    void init();

    // Feed encrypted KCP data (from relay TCP or P2P UDP) into KCP.
    // Internally calls ikcp_input + ikcp_update + recv_loop.
    void feed_encrypted(const char* data, std::size_t len);

    // Encrypt plaintext and send through KCP.
    void send_plaintext(const char* data, std::size_t size);

    // Callback: decrypted plaintext frame ready for delivery.
    void set_on_frame(std::function<void(std::vector<std::uint8_t>)> cb);

    // Callback: data-plane link judged dead by ikcp keepalive (triggered from
    // the tick drive path after ikcp_update() returns non-zero).
    void set_on_dead(std::function<void()> cb);

    // Callback: encrypted KCP output ready for transport write.
    void set_on_output(std::function<void(const std::uint8_t*, std::size_t)> cb);

    void close();
    bool is_active() const { return kcp_ != nullptr; }

private:
    void tick();
    void recv_loop();
    static int output_cb_static(const char* buf, int len, ikcpcb* kcp, void* user);

    asio::any_io_executor executor_;
    std::vector<std::uint8_t> traffic_key_;
    std::string salt_;

    struct deleter { void operator()(ikcpcb* p) const noexcept { if (p) ikcp_release(p); } };
    std::unique_ptr<ikcpcb, deleter> kcp_;
    asio::steady_timer timer_;
    bool active_ = false;

    std::array<char, 16 * 1024> read_buf_{};

    std::function<void(std::vector<std::uint8_t>)> on_frame_;
    std::function<void(const std::uint8_t*, std::size_t)> on_output_;
    std::function<void()> on_dead_cb_;
};

} // namespace network::proxy
