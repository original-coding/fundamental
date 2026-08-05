#pragma once

#include "frp_client_command.hpp"
#include "frp_command.hpp"
#include "frp_config_types.hpp"

#include "network/network.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace network::proxy
{

class relay_data_channel;
class frp_signal_client;

class frp_provider : public std::enable_shared_from_this<frp_provider>,
                      private asio::noncopyable {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<frp_provider>(std::forward<Args>(args)...);
    }

    explicit frp_provider(std::shared_ptr<frp_signal_client> signal);
    ~frp_provider() = default;

    void set_service_map(std::unordered_map<std::string,
                         std::unordered_map<std::string, frp_provider_service_config>> m);
    void register_all_services();
    void on_client_command(std::uint8_t cmd, std::string payload);
    void close();

    // state access
    std::unordered_map<std::string, std::shared_ptr<relay_data_channel>>& channels() { return channels_; }

private:
    void handle_client_open(const frp_client_open_data& data);
    void start_backend_connect(const std::shared_ptr<relay_data_channel>& ch);
    void start_backend_read_loop(const std::shared_ptr<relay_data_channel>& ch);
    void setup_data_channel(const std::shared_ptr<relay_data_channel>& ch);
    void start_data_forward_read_loop(const std::shared_ptr<relay_data_channel>& ch);
    void close_data_channel(const std::shared_ptr<relay_data_channel>& ch);
    void maybe_start_p2p(const std::shared_ptr<relay_data_channel>& ch);
    void handle_p2p_message(std::uint8_t cmd, std::string payload);

    std::unordered_map<std::string, std::shared_ptr<relay_data_channel>> channels_;
    std::unordered_map<std::string, std::unordered_map<std::string, frp_provider_service_config>> services_by_key_;
    std::shared_ptr<frp_signal_client> signal_;
};

} // namespace network::proxy
