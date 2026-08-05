#pragma once

#include "frp_client_command.hpp"
#include "frp_command.hpp"
#include "frp_config_types.hpp"

#include "network/network.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace network::proxy
{

class frp_signal_client : public std::enable_shared_from_this<frp_signal_client>,
                           private asio::noncopyable {
public:
    using server_cmd_cb = std::function<void(const frp_command_base&, std::string)>;
    using client_cmd_cb = std::function<void(std::uint8_t, std::string)>;
    using subscribe_cb  = std::function<void(const std::vector<frp_visible_service_data>&)>;

    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_signal_client>(std::forward<Args>(args)...);
    }

    explicit frp_signal_client(frp_proxy_client_config config);
    ~frp_signal_client();

    void start();
    void release_obj();

    void set_on_server_command(server_cmd_cb cb) { on_server_cmd_ = std::move(cb); }
    void set_on_client_command(client_cmd_cb cb) { on_client_cmd_ = std::move(cb); }
    void set_on_subscribe(subscribe_cb cb) { on_subscribe_ = std::move(cb); }

    template <typename CommandData>
    void send_command(const CommandData& data) {
        if (signal_) signal_->send_command(data);
    }

    void send_p2p_command(const std::string& peer_uuid, std::string json_payload);

    // State accessors
    const frp_proxy_client_config& config() const { return config_; }
    const std::string& uuid() const { return uuid_; }
    std::uint8_t probed_nat_type() const { return probed_nat_type_; }
    std::uint32_t startup_rtt_ms() const { return startup_rtt_ms_; }
    std::int64_t server_clock_offset_us() const { return server_clock_offset_us_; }
    network_data_reference& reference() { return reference_; }
    bool is_reference_valid() const { return reference_.is_valid(); }

    void set_probed_nat_type(std::uint8_t t) { probed_nat_type_ = t; }
    void set_startup_rtt_ms(std::uint32_t rtt) { startup_rtt_ms_ = rtt; }
    void set_server_clock_offset_us(std::int64_t us) { server_clock_offset_us_ = us; }

    class frp_signal_channel& signal_channel() { return *signal_; }
    const asio::any_io_executor& get_executor() { return executor_; }

    int reconnect_delay_seconds_ = 2;

private:
    void connect_signal_channel();
    void schedule_reconnect();
    void process_command(const frp_command_base& cmd, std::string payload);
    void process_server_command(const frp_command_base& cmd, std::string payload);
    void process_client_command(std::uint8_t cmd, std::string payload);
    void run_nat_probe();
    void run_time_sync();
    void start_polling();
    void do_poll();

    network_data_reference reference_;
    frp_proxy_client_config config_;
    const std::string uuid_;
    // 单 executor：本对象所有定时器、signal channel 及下游（accessor/provider/relay）
    // 统一绑定一个 io_context（规范 §3.1/3.3）——消除"定时器在 A、通道在 B"的交叉访问。
    const asio::any_io_executor executor_;
    asio::steady_timer reconnect_timer_;
    asio::steady_timer poll_timer_;
    std::shared_ptr<class frp_signal_channel> signal_;

    std::uint8_t probed_nat_type_  = frp_nat_type_disabled;
    std::uint32_t startup_rtt_ms_  = 100;
    std::int64_t server_clock_offset_us_ = 0;

    server_cmd_cb on_server_cmd_;
    client_cmd_cb on_client_cmd_;
    subscribe_cb on_subscribe_;
    std::unordered_set<std::string> last_known_services_;
    // 信令保活：连续 ping 无 pong 的计数（超过阈值判定对端假死）
    std::uint32_t ping_wait_count_ = 0;

    // Probe/time-sync UDP resources — created on demand, released by release_obj.
    std::shared_ptr<asio::ip::udp::socket> probe_socket_;
    asio::steady_timer probe_timer_;
};

} // namespace network::proxy
