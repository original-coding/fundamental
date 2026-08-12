#pragma once

#include "frp_client_command.hpp"
#include "frp_command.hpp"
#include "frp_config_types.hpp"

#include "network/network.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace network::proxy
{

class relay_data_channel;
class frp_signal_client;

class frp_accessor : public std::enable_shared_from_this<frp_accessor>,
                      private asio::noncopyable {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_accessor>(std::forward<Args>(args)...);
    }

    explicit frp_accessor(std::shared_ptr<frp_signal_client> signal);
    ~frp_accessor();

    void subscribe_all_keys();
    void send_subscribe_request();
    void on_subscribe(const std::vector<frp_visible_service_data>& services);
    void on_client_command(std::uint8_t cmd, std::string payload);
    void close();

    // state access
    std::unordered_map<std::string, std::shared_ptr<relay_data_channel>>& channels() { return channels_; }

private:
    struct listener_runtime {
        explicit listener_runtime(const asio::any_io_executor& ex) : acceptor(ex) {}
        std::string service_name; std::string listen_host; std::uint16_t listen_port = 0;
        std::uint8_t service_type = frp_service_tcp; bool enable_p2p = true;
        bool provider_enable_p2p = true; std::uint8_t provider_nat_type = frp_nat_type_disabled;
        std::uint32_t provider_startup_rtt_ms = 100;
        std::string register_key; std::string provider_uuid;
        asio::ip::tcp::acceptor acceptor;
        // 与 relay 通道共享所有权（3.3）：listener 移除后通道仍持有已关闭的 socket，无悬垂
        std::shared_ptr<asio::ip::udp::socket> udp_socket;
        std::array<char, 16 * 1024> udp_recv_buf {};
        asio::ip::udp::endpoint udp_recv_endpoint_;
        std::unordered_map<asio::ip::udp::endpoint, std::weak_ptr<relay_data_channel>> udp_sessions;
    };

    void reconcile_listeners(const std::vector<frp_visible_service_data>& services);
    // 订阅快照缺服务时按 1s 间隔快速重试（上限 kMaxResubscribeAttempts 次），
    // 之后降级为 30s 周期刷新（恢复旧版"持续探测订阅"行为）。覆盖两类场景：
    //   1. 服务端重启后 provider 注册晚于 accessor 的订阅快照（快速路径）
    //   2. provider 长时间离线后恢复、服务端状态漂移（周期兜底）
    void schedule_resubscribe();
    bool desired_services_satisfied() const;
    void start_tcp_accept_loop(const std::shared_ptr<listener_runtime>& lst);
    void start_udp_receive_loop(const std::shared_ptr<listener_runtime>& lst);
    std::string generate_connection_uuid();
    void open_data_channel(const std::shared_ptr<relay_data_channel>& ch);
    void start_data_forward_read_loop(const std::shared_ptr<relay_data_channel>& ch);
    void start_local_read_loop(const std::shared_ptr<relay_data_channel>& ch);
    void close_data_channel(const std::shared_ptr<relay_data_channel>& ch);
    void maybe_start_p2p(const std::shared_ptr<relay_data_channel>& ch);
    void handle_p2p_message(std::uint8_t cmd, std::string payload);

    std::unordered_map<std::string, std::shared_ptr<relay_data_channel>> channels_;
    std::unordered_map<std::string, std::shared_ptr<listener_runtime>> listeners_;
    std::unordered_set<std::string> last_known_services_;
    std::shared_ptr<frp_signal_client> signal_;
    // 最近一次订阅快照（"service_name:type"，不含自己提供的服务），用于判断期望是否满足
    std::unordered_set<std::string> visible_services_;
    static constexpr std::uint32_t kMaxResubscribeAttempts = 10;
    static constexpr std::chrono::seconds kResubscribeFastInterval{1};
    static constexpr std::chrono::seconds kResubscribeRefreshInterval{30};
    std::uint32_t resubscribe_attempts_ = 0;
    asio::steady_timer resubscribe_timer_;
};

} // namespace network::proxy
