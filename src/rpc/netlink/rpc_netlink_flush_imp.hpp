#pragma once
#include "netlink.hpp"
#include "network/network.hpp"
#include "rpc/rpc_client.hpp"

#include <mutex>

namespace network
{
namespace link
{
namespace imp
{

struct rpc_netlink_request {
    static constexpr const char* kRpcName = "network::link::imp::rpc_netlink_request";
    LinkNumberType tag;
    LinkNumberType player_no = 0;
    std::string data;
};

struct rpc_netlink_response {
    std::int32_t code = 0;
    std::string msg;
    std::string data;
};

struct rpc_netlink_forward_config {
    std::string forward_host;
    std::string forward_service;
};

class rpc_netlink_writer : virtual public netlink_flush_interface {
    using rpc_client_t = network::auto_network_storage_instance<network::rpc_service::rpc_client>;
    struct rpc_client_context {
        rpc_client_t rpc_client_handle;
        std::mutex init_mutex;
        Fundamental::error_code init_client(const rpc_netlink_forward_config& config, const ClusterPlayer& player);
    };

public:
    // 256M
    constexpr static std::size_t kMaxLinkCacheDataSize = 1024 * 1024 * 256;
    constexpr static LinkNumberType kMaxPlayerNum      = 256;

public:
    rpc_netlink_writer(LinkNumberType link_tag,
                       rpc_netlink_forward_config config,
                       std::size_t max_cache_size = kMaxLinkCacheDataSize);
    void init(LinkNumberType player_num) override;
    void flush_data(
        const std::string& data,
        const ClusterPlayer& player,
        std::size_t timeout_msec,
                            std::weak_ptr<netlink_status_change_cb> result_cb) override;
    void check_keep_alive() override;
    void status_report(std::string status) override;

protected:
    const LinkNumberType link_tag_;
    const std::size_t max_cache_size_;
    const rpc_netlink_forward_config forward_config;
    std::vector<std::unique_ptr<rpc_client_context>> rpc_clients;
};

} // namespace imp
} // namespace link
} // namespace network