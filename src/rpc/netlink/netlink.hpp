#pragma once
#include "netlink_definitions.hpp"
#include "netlink_error.hpp"
#include <functional>
#include <future>
#include <map>
#include <string_view>

namespace network
{
namespace link
{

struct netlink_flush_interface {
    enum flush_data_status : std::int32_t
    {
        flush_data_success_status,
        flush_data_network_failed_status,
        flush_data_too_many_request_status
    };
    using netlink_status_change_cb =
        std::function<void(flush_data_status, Fundamental::error_code, std::string /*ret data*/)>;
    virtual ~netlink_flush_interface() {};
    virtual void init(LinkNumberType player_num)                               = 0;
    virtual void flush_data(const std::string& data,
                            const ClusterPlayer& player,
                            std::size_t timeout_msec,
                            std::weak_ptr<netlink_status_change_cb> result_cb) = 0;
    virtual void check_keep_alive()                                            = 0;
    virtual void status_report(std::string status)                             = 0;
};

struct netlink_config {

    constexpr static std::size_t kNetworkRetryIntervalMsec = 1000;
    constexpr static std::size_t kDefaultNetworkTimeoutSec = 60;
    constexpr static std::size_t kMinLinkCheckCnt          = 2;
    std::size_t network_timeout_sec                        = kDefaultNetworkTimeoutSec;
    std::size_t link_timeout_sec                           = kDefaultNetworkTimeoutSec;
    std::size_t max_send_max_try_interval_cnt              = 15;
    std::size_t phase_check_interval_cnt                   = 120;
    // network info
    LinkNumberType local_player_no = 0;
    LinkNumberType player_nums     = 0;
    std::vector<ClusterPlayer> all_players;
};

class netlink {
    struct netlink_private_data;

public:
    using link_data_map       = std::map<LinkNumberType, std::string>;
    using link_dst_player_set = std::set<LinkNumberType>;

public:
    explicit netlink(netlink_config config, netlink_flush_interface& flush_interface);
    virtual ~netlink();
    const netlink_config& get_config() const;
    // init interface,throw when failed
    Fundamental::error_code online_setup(const PeerNetworkInfo& coordinate_server,
                                         LinkNumberType coordinate_server_player_no);
    void local_setup();

    // abort all link wait operations,those operations will return error
    void abort_link();

    link_dst_player_set generate_dst_players() const;
    link_data_map generate_recv_context() const;

    Fundamental::error_code exchange(std::string data,
                                     std::string key,
                                     link_dst_player_set dst_players,
                                     link_data_map& out,
                                     std::string_view call_tag);
    void declare_key_is_progressing(std::string key);
    // basic interface
    Fundamental::error_code send_data(std::string data,
                                      std::string key,
                                      link_dst_player_set dst_players,
                                      std::string_view call_tag);
    Fundamental::error_code recv_data(std::string key, link_data_map& out, std::string_view call_tag);

    std::tuple<Fundamental::error_code, std::string> push_data(std::string data);

    std::tuple<Fundamental::error_code, std::string> send_command(std::uint64_t player_no, std::string command_data);

protected:
    virtual std::tuple<Fundamental::error_code, std::string> handle_custom_data(std::int32_t type, std::string data);

private:
    netlink_private_data* pdata = nullptr;
};

} // namespace link
} // namespace network