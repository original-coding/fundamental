#include "kcp_channel.hpp"

#include "fundamental/basic/log.h"

namespace network::proxy
{

kcp_channel::kcp_channel(asio::any_io_executor executor,
                         std::string traffic_secret, std::string salt) :
executor_(std::move(executor)), salt_(std::move(salt)), timer_(executor_) {
    traffic_key_ = frp_derive_kcp_key(traffic_secret, salt_);
    io_context_pool::Instance().reg_timer(timer_);
}

kcp_channel::~kcp_channel() {
    close();
    io_context_pool::Instance().unreg_timer(timer_);
}

void kcp_channel::init() {
    if (kcp_) return;
    std::hash<std::string> hasher;
    auto conv = static_cast<std::uint32_t>(hasher(salt_) & 0xFFFFFFFF);
    if (conv == 0) conv = 1;
    kcp_.reset(ikcp_create(conv, this));
    ikcp_setoutput(kcp_.get(), output_cb_static);
    ikcp_wndsize(kcp_.get(), 256, 256);
    ikcp_nodelay(kcp_.get(), 1, 20, 2, 1);
    // 链路检测：2s 探测间隔、5 次连续无响应判死（死链经 ikcp_update 返回值暴露）
    ikcp_enable_keepalive(kcp_.get(), 2000, 5);
    FINFO("kcp_channel init conv={}", conv);
    tick();
}

void kcp_channel::feed_encrypted(const char* data, std::size_t len) {
    if (!kcp_) return;
    ikcp_input(kcp_.get(), data, static_cast<long>(len));
    auto now = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() & 0xffffffffu);
    ikcp_update(kcp_.get(), now);
    recv_loop();
}

void kcp_channel::send_plaintext(const char* data, std::size_t size) {
    if (!kcp_) return;
    if (!active_) { active_ = true; tick(); }
    auto encrypted = frp_kcp_encrypt(traffic_key_,
                                     std::vector<std::uint8_t>(data, data + size));
    if (encrypted.empty()) return;
    ikcp_send(kcp_.get(), reinterpret_cast<const char*>(encrypted.data()),
              static_cast<int>(encrypted.size()));
    auto now = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() & 0xffffffffu);
    ikcp_update(kcp_.get(), now);
}

void kcp_channel::set_on_frame(std::function<void(std::vector<std::uint8_t>)> cb) {
    on_frame_ = std::move(cb);
}

void kcp_channel::set_on_output(std::function<void(const std::uint8_t*, std::size_t)> cb) {
    on_output_ = std::move(cb);
}

void kcp_channel::close() {
    timer_.cancel();
    kcp_.reset();
    active_ = false;
    on_frame_ = nullptr;
    on_output_ = nullptr;
}

int kcp_channel::output_cb_static(const char* buf, int len, ikcpcb*, void* user) {
    auto* ch = static_cast<kcp_channel*>(user);
    if (ch->on_output_) ch->on_output_(reinterpret_cast<const std::uint8_t*>(buf),
                                        static_cast<std::size_t>(len));
    return 0;
}

void kcp_channel::tick() {
    if (!kcp_) return;
    timer_.expires_after(std::chrono::milliseconds(20));
    auto self = shared_from_this();
    timer_.async_wait([this, self](const std::error_code& ec) {
        if (ec || !kcp_) return;
        auto now = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() & 0xffffffffu);
        int update_ret = ikcp_update(kcp_.get(), now);
        if (update_ret != 0) {
            // 数据面死链（keepalive 探测无响应）：通知上层关闭通道
            if (on_dead_cb_) on_dead_cb_();
            return;
        }
        recv_loop();
        tick();
    });
}

void kcp_channel::set_on_dead(std::function<void()> cb) {
    on_dead_cb_ = std::move(cb);
}

void kcp_channel::recv_loop() {
    if (!kcp_ || !on_frame_) return;
    while (true) {
        int peek_size = ikcp_peeksize(kcp_.get());
        if (peek_size < 0) break;
        std::vector<char> buf;
        if (peek_size > static_cast<int>(read_buf_.size()))
            buf.resize(peek_size);
        char* recv_ptr = buf.empty() ? read_buf_.data() : buf.data();
        int recv_len = buf.empty() ? static_cast<int>(read_buf_.size()) : peek_size;
        auto n = ikcp_recv(kcp_.get(), recv_ptr, recv_len);
        if (n < 0) break;
        std::vector<std::uint8_t> encrypted(recv_ptr, recv_ptr + n);
        auto plaintext = frp_kcp_decrypt(traffic_key_, encrypted);
        if (plaintext && !plaintext->empty())
            on_frame_(std::move(*plaintext));
    }
}

} // namespace network::proxy
