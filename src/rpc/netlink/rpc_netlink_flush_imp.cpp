#include "rpc_netlink_flush_imp.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/rttr_handler/deserializer.h"
#include "fundamental/rttr_handler/serializer.h"
#include "rpc/proxy/protocal_pipe/pipe_connection_upgrade_session.hpp"
#include "rpc/proxy/websocket/ws_upgrade_session.hpp"

#include <stdexcept>

namespace network
{
namespace link
{
namespace imp
{
rpc_netlink_writer::rpc_netlink_writer(LinkNumberType link_tag,
                                       rpc_netlink_forward_config config,
                                       std::size_t max_cache_size) :
link_tag_(std::move(link_tag)), max_cache_size_(max_cache_size), forward_config(config) {
}
void rpc_netlink_writer::init(LinkNumberType player_num) {
    if (player_num >= kMaxPlayerNum) {
        throw std::invalid_argument(
            Fundamental::StringFormat("{} players overflow kMaxPlayerNum {}", link_tag_, player_num, kMaxPlayerNum));
    }
    if (rpc_clients.size() < player_num) {
        rpc_clients.resize(player_num);
        for (LinkNumberType no=0; no < player_num; ++no) {
            auto& context = rpc_clients[no];
            if (!context) {
                context = std::make_unique<rpc_client_context>();
            }
        }
    }
}

void rpc_netlink_writer::flush_data(const std::string& data,
                                    const ClusterPlayer& player,
                                    std::size_t timeout_msec,
                                    std::weak_ptr<netlink_status_change_cb> result_cb) {
    Fundamental::error_code result_ec;
    std::string ret_string;
    netlink_flush_interface::flush_data_status status =
        netlink_flush_interface::flush_data_status::flush_data_success_status;
    Fundamental::ScopeGuard failed_g([&]() {
        if (result_ec) {
            auto strong_result_cb = result_cb.lock();
            if (strong_result_cb.get()) {
                strong_result_cb->operator()(status, result_ec, std::move(ret_string));
            }
        }
    });
    do {
        // process request
        if (player.player_no >= rpc_clients.size()) {
            result_ec =
                make_error_code(netlink_errors::netlink_failed,
                                Fundamental::StringFormat("invalid player no {} to flush data", player.player_no));
            break;
        }
        auto& client_context = *rpc_clients[player.player_no];
        std::unique_lock<std::mutex> network_locker(client_context.init_mutex);
        if (!client_context.rpc_client_handle.get() || !client_context.rpc_client_handle.get()->has_connected()) {
            result_ec = client_context.init_client(forward_config, player);
            if (result_ec) {
                status = netlink_flush_interface::flush_data_status::flush_data_network_failed_status;
                break;
            }
        }
        auto client       = client_context.rpc_client_handle.get();
        auto pending_size = client.get()->pending_cache_data_size();
        if (pending_size >= max_cache_size_) {
            status    = netlink_flush_interface::flush_data_status::flush_data_too_many_request_status;
            result_ec = make_error_code(
                netlink_errors::netlink_failed,
                Fundamental::StringFormat("too mant netlink flush request cache {}:bytes overflow {}:bytes",
                                          pending_size, max_cache_size_));
            break;
        }
        network_locker.unlock();
        rpc_netlink_request request;
        request.tag       = link_tag_;
        request.player_no = player.player_no;
        request.data      = data;

        rpc_netlink_response rpc_response;

        try {
            client->async_timeout_call(rpc_netlink_request::kRpcName, timeout_msec, request)
                .async_response<rpc_netlink_response>(
                    [result_cb](Fundamental::error_code ec, rpc_netlink_response response) {
                        if (!ec) {
                            if (response.code !=
                                static_cast<decltype(rpc_response.code)>(netlink_errors::netlink_success)) {
                                ec = make_error_code(netlink_errors::netlink_failed,
                                                     Fundamental::StringFormat("rpc request failed {}[{}]",
                                                                               response.code, response.msg));
                            }
                        }
                        auto strong_result_cb = result_cb.lock();
                        if (strong_result_cb.get()) {
                            strong_result_cb->operator()(
                                netlink_flush_interface::flush_data_status::flush_data_success_status, ec,
                                std::move(response.data));
                        }
                    });
        } catch (...) {
            result_ec = make_error_code(
                netlink_errors::netlink_failed,
                Fundamental::StringFormat("rpc[{}:{} {}:ms] call:{} throw", client->get_remote_peer_ip(),
                                          client->get_remote_peer_port(), timeout_msec, rpc_netlink_request::kRpcName));
            break;
        }
    } while (0);
}
void rpc_netlink_writer::check_keep_alive() {
    FINFO("{} {}", link_tag_, __func__);
}
void rpc_netlink_writer::status_report(std::string status) {
    FINFO("{} {}:[{}]", link_tag_, __func__, status);
}

Fundamental::error_code rpc_netlink_writer::rpc_client_context::init_client(const rpc_netlink_forward_config& config,
                                                                            const ClusterPlayer& player) {
    rpc_client_handle = network::make_guard<network::rpc_service::rpc_client>();
    std::string use_host;
    std::string use_service;
    switch (player.link_type) {
    case LinkNetworkType::InSameInstance: {
        // direct access
        use_host    = player.network_info.host;
        use_service = player.network_info.service;
    } break;
    case LinkNetworkType::InSameMachine: {
        // direct access
        use_host    = player.network_info.host;
        use_service = player.network_info.service;
    } break;
    case LinkNetworkType::InSameCluster: {
        // use rpc master node to proxy data
        use_host    = player.network_info.master_host;
        use_service = player.network_info.master_service;
        rpc_client_handle->append_proxy(
            network::proxy::ws_upgrade_imp::make_shared(player.network_info.route_path, use_host));
    } break;
    case LinkNetworkType::InExternalAccess: {
        // use two rpc node to pipe proxy data
        network::forward::forward_request_context forward_request;
        forward_request.dst_host      = player.network_info.external_host;
        forward_request.dst_service   = player.network_info.external_service;
        forward_request.route_path    = player.network_info.route_path;
        forward_request.ssl_option    = network::forward::forward_optional_option;
        forward_request.socks5_option = network::forward::forward_optional_option;

        rpc_client_handle->append_proxy(network::proxy::pipe_connection_upgrade::make_shared(forward_request));
        use_host    = config.forward_host;
        use_service = config.forward_service;
    } break;
    default: break;
    }
    if (!rpc_client_handle->connect(use_host, use_service)) {
        return make_error_code(netlink_errors::netlink_failed,
                               Fundamental::StringFormat("connect to {} failed", Fundamental::io::to_json(player)));
    } else {
        rpc_client_handle->enable_timeout_check(true, 10000);
    }
    return {};
}
} // namespace imp
} // namespace link
} // namespace network

RTTR_REGISTRATION {
    using namespace network::link::imp;
    {
        using RegisterType = rpc_netlink_request;
        rttr::registration::class_<RegisterType>("network::link::imp::rpc_netlink_request")
            .constructor()(rttr::policy::ctor::as_object)
            .property("data", &RegisterType::data)
            .property("player_no", &RegisterType::player_no)
            .property("tag", &RegisterType::tag);
    }
    {
        using RegisterType = rpc_netlink_response;
        rttr::registration::class_<RegisterType>("network::link::imp::rpc_netlink_response")
            .constructor()(rttr::policy::ctor::as_object)
            .property("data", &RegisterType::data)
            .property("code", &RegisterType::code)
            .property("msg", &RegisterType::msg);
    }
}