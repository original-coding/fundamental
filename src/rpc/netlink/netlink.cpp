#include "netlink.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/rttr_handler/deserializer.h"
#include "fundamental/rttr_handler/serializer.h"
#include <unordered_map>

namespace network
{
namespace link
{
constexpr static std::size_t kMaxDataDumpSize = 1024;
struct netlink::netlink_private_data {
    netlink_private_data(netlink& parent, netlink_flush_interface& ref) : parent_(parent), ref_interface(ref) {
    }
    void notify_status_update(bool with_mutex = true) {
        if (with_mutex) {
            std::scoped_lock<std::mutex> locker(link_status_update_notify_mutex);
            link_status_update_notify_cv.notify_all();
        } else {
            link_status_update_notify_cv.notify_all();
        }
    }
    void notify_send_status_update() {
        std::scoped_lock<std::mutex> locker(send_status_mutex);
        send_status_changed_notify_cv.notify_all();
    }
    std::tuple<Fundamental::error_code, bool> check_link_status(LinkNumberType dst_player_no, std::string_view key) {
        LinkCheckCommandData check_data;
        check_data.type      = LinkCommandEnumType::LinkCheckCommandType;
        check_data.key       = key;
        check_data.player_no = config.local_player_no;
        auto [ec, ret]       = parent_.send_command(dst_player_no, Fundamental::io::to_json(check_data));
        return std::make_tuple(ec, ret == LinkCheckCommandData::kSuccessCheckRet);
    }
    Fundamental::error_code handle_link_data(std::string data) {
        LinkCommandData request;
        Fundamental::error_code ec;
        do {
            if (!Fundamental::io::from_json(data, request) || request.player_no == config.local_player_no ||
                request.player_no >= config.player_nums || request.key.empty()) {
                ec = make_error_code(
                    netlink_errors::netlink_failed,
                    Fundamental::StringFormat("invalid link request:{}", data.substr(0, kMaxDataDumpSize)));
                break;
            }
            std::unique_lock<std::mutex> locker(link_status_update_notify_mutex);
            auto& phase_player_map = link_status_cache_storage[request.key];
            auto ret               = phase_player_map.try_emplace(request.player_no, std::move(request.data));
            if (ret.second) {
                notify_status_update(false);
            }
        } while (0);
        return ec;
    }
    std::tuple<Fundamental::error_code, std::string> handle_check_data(std::string data) {
        LinkCheckCommandData request;
        Fundamental::error_code ec;
        std::string ret_data;
        do {
            if (!Fundamental::io::from_json(data, request) || request.player_no == config.local_player_no ||
                request.player_no >= config.player_nums || request.key.empty()) {
                ec = make_error_code(
                    netlink_errors::netlink_failed,
                    Fundamental::StringFormat("invalid link check request:{}", data.substr(0, kMaxDataDumpSize)));
                break;
            }
            std::scoped_lock<std::mutex> locker(link_prepare_data_cache_mutex);
            auto iter = preparing_link_datas.find(request.key);
            if (iter == preparing_link_datas.end()) break;
            if (iter->second.erase(request.player_no) > 0) {
                ret_data = LinkCheckCommandData::kSuccessCheckRet;
            }
        } while (0);
        return std::make_tuple(ec, std::move(ret_data));
    }
    void init() {
        if (is_init) return;
        is_init.exchange(true);
        ref_interface.init(config.player_nums);
    }
    netlink& parent_;
    netlink_flush_interface& ref_interface;
    netlink_config config;
    std::atomic_bool is_init      = false;
    std::atomic_bool is_setup     = false;
    std::atomic_bool is_cancelled = false;

    std::unordered_map<std::string, link_data_map> link_status_cache_storage;
    std::mutex link_status_update_notify_mutex;
    std::condition_variable link_status_update_notify_cv;
    std::unordered_map<std::string, link_dst_player_set> preparing_link_datas;
    std::mutex link_prepare_data_cache_mutex;
    std::condition_variable send_status_changed_notify_cv;
    std::mutex send_status_mutex;
    link_dst_player_set all_other_players;
    link_data_map all_other_players_recv_context;
};

netlink::netlink(netlink_config config, netlink_flush_interface& flush_interface) :
pdata(new netlink_private_data(*this, flush_interface)) {
    pdata->config = std::move(config);
    if (pdata->config.all_players.size() < pdata->config.player_nums) {
        pdata->config.all_players.resize(pdata->config.player_nums);
    }
    for (LinkNumberType no = 0; no < pdata->config.player_nums; ++no) {
        pdata->config.all_players[no].player_no = no;
        if (no != pdata->config.local_player_no) {
            pdata->all_other_players.insert(no);
            pdata->all_other_players_recv_context.emplace(no, "");
        }
    }
}

netlink::~netlink() {
    if (pdata) delete pdata;
}

const netlink_config& netlink::get_config() const {
    return pdata->config;
}

Fundamental::error_code netlink::online_setup(const PeerNetworkInfo& coordinate_server,
                                              LinkNumberType coordinate_server_player_no) {
    pdata->init();
    const static std::string kSetupKey          = "__online_setup__";
    const static std::string kSetupBroadCastKey = "__online_setup_broadcast__";
    Fundamental::error_code ec;
    do {
        if (pdata->is_setup) break;

        if (ec) break;
        if (coordinate_server_player_no >= pdata->config.player_nums ||
            pdata->config.local_player_no >= pdata->config.player_nums) {
            ec = make_error_code(
                netlink_errors::netlink_invalid_argument,
                Fundamental::StringFormat(
                    "invalid player info coordinate_server_player_no:{} local_player_no:{} player_nums:{}",
                    coordinate_server_player_no, pdata->config.local_player_no, pdata->config.player_nums));
            break;
        }
        auto& coordinate_server_player     = pdata->config.all_players[coordinate_server_player_no];
        coordinate_server_player.link_type = LinkNetworkType::InExternalAccess;
        if (coordinate_server_player_no == pdata->config.local_player_no) {
            // recv all other data
            auto recv_map = generate_recv_context();
            ec            = recv_data(kSetupKey, recv_map, __func__);
            if (ec) break;
            for (auto& player_data : recv_map) {
                ClusterPlayer& player_info = pdata->config.all_players[player_data.first];
                if (!Fundamental::io::from_json(player_data.second, player_info) ||
                    player_info.player_no != player_data.first) {
                    ec = make_error_code(netlink_errors::netlink_invalid_argument,
                                         Fundamental::StringFormat("invalid player info  from player:{} data:{}",
                                                                   player_data.first,
                                                                   player_data.second.substr(0, kMaxDataDumpSize)));
                    break;
                }
            }
            if (ec) break;
            local_setup();
            // send network info to ohter peer
            ec = send_data(Fundamental::io::to_json(pdata->config.all_players), kSetupBroadCastKey,
                           generate_dst_players(), __func__);
        } else {
            coordinate_server_player.network_info = coordinate_server;
            // send network info to coordinate_server_player_no
            ec = send_data(Fundamental::io::to_json(pdata->config.all_players[pdata->config.local_player_no]),
                           kSetupKey, { coordinate_server_player_no }, __func__);
            if (ec) break;
            // recv all network info from coordinate_server_player_no
            link_data_map recv_map;
            auto& want_data = recv_map[coordinate_server_player_no];
            ec              = recv_data(kSetupBroadCastKey, recv_map, __func__);
            if (ec) break;
            decltype(pdata->config.all_players) actual_config;
            if (!Fundamental::io::from_json(want_data, actual_config) ||
                actual_config.size() != pdata->config.player_nums) {
                ec = make_error_code(netlink_errors::netlink_invalid_argument,
                                     Fundamental::StringFormat("invalid all players info  from player:{} data:{}",
                                                               coordinate_server_player_no,
                                                               want_data.substr(0, kMaxDataDumpSize)));
                break;
            }
            for (LinkNumberType player_no = 0; player_no < pdata->config.player_nums; ++player_no) {
                if (player_no == pdata->config.local_player_no) continue;
                pdata->config.all_players[player_no] = std::move(actual_config[player_no]);
            }
            local_setup();
        }
    } while (0);
    return ec;
}

void netlink::local_setup() {
    pdata->init();
    if (pdata->is_setup) return;
    pdata->is_setup.exchange(true);
    if (pdata->config.local_player_no >= pdata->config.player_nums) {
        throw std::invalid_argument(Fundamental::StringFormat("invalid player info  local_player_no:{} player_nums:{}",
                                                              pdata->config.local_player_no,
                                                              pdata->config.player_nums));
    }
    // verify other player no
    auto& local_player = pdata->config.all_players[pdata->config.local_player_no];
    for (LinkNumberType player_no = 0; player_no < pdata->config.player_nums; ++player_no) {
        auto& player = pdata->config.all_players[player_no];
        if (player.player_no != player_no) {
            throw std::invalid_argument(Fundamental::StringFormat(
                "invalid player info  want player_no:{} actual player_no:{}", player_no, player.player_no));
        }
        if (player.player_id.empty() || player.master_id.empty()) {
            throw std::invalid_argument(
                Fundamental::StringFormat("invalid player info  player_id and master_id can't be empty"));
        }
        if (player.player_id == local_player.player_id) {
            player.link_type = LinkNetworkType::InSameInstance;
        } else if (player.master_id == local_player.master_id) {
            if (player.machine_id == local_player.machine_id) {
                player.link_type = LinkNetworkType::InSameMachine;
            } else {
                player.link_type = LinkNetworkType::InSameCluster;
            }
        } else {
            player.link_type = LinkNetworkType::InExternalAccess;
        }
    }
    pdata->ref_interface.status_report(Fundamental::StringFormat("player:{} use netlink:{}",
                                                                 pdata->config.local_player_no,
                                                                 Fundamental::io::to_json(pdata->config.all_players)));
}

void netlink::abort_link() {
    pdata->is_cancelled.exchange(true);
    pdata->notify_status_update();
    pdata->notify_send_status_update();
}

netlink::link_dst_player_set netlink::generate_dst_players() const {
    return pdata->all_other_players;
}

netlink::link_data_map netlink::generate_recv_context() const {
    return pdata->all_other_players_recv_context;
}

Fundamental::error_code netlink::exchange(std::string data,
                                          std::string key,
                                          link_dst_player_set dst_players,
                                          link_data_map& out,
                                          std::string_view call_tag) {
    Fundamental::error_code ec;
    do {
        ec = send_data(std::move(data), key, std::move(dst_players), call_tag);
        if (ec) break;
        ec = recv_data(std::move(key), out, call_tag);
    } while (0);
    return ec;
}

void netlink::declare_key_is_progressing(std::string key) {
    std::scoped_lock<std::mutex> locker(pdata->link_prepare_data_cache_mutex);
    pdata->preparing_link_datas[key] = pdata->all_other_players;
}

Fundamental::error_code netlink::send_data(std::string data,
                                           std::string key,
                                           link_dst_player_set dst_players,
                                           std::string_view call_tag) {
    if (pdata->is_cancelled) {
        return make_error_code(netlink_errors::netlink_operation_cancelled,
                               Fundamental::StringFormat("send link data[{}] by {} was aborted", key, call_tag));
    }
    Fundamental::ScopeGuard remoge_phase_cache_g([&]() {
        std::scoped_lock<std::mutex> locker(pdata->link_prepare_data_cache_mutex);
        pdata->preparing_link_datas.erase(key);
    });

    Fundamental::error_code ec;
    dst_players.erase(pdata->config.local_player_no);
    do {
        if (dst_players.empty()) break;
        for (const auto& item : dst_players) {
            if (item >= pdata->config.player_nums) {
                return make_error_code(netlink_errors::netlink_invalid_argument,
                                       Fundamental::StringFormat("send data player:{} is invalid,player_nums:{}", item,
                                                                 pdata->config.player_nums));
            }
        }
        LinkCommandData command;
        command.type      = LinkCommandEnumType::LinkCommandType;
        command.player_no = pdata->config.local_player_no;
        command.data      = std::move(data);
        command.key       = key;
        auto command_data = Fundamental::io::to_json(command);
        pdata->ref_interface.status_report(Fundamental::StringFormat("player:{} send link data[{}] by {} size:{}",
                                                                     pdata->config.local_player_no, key, call_tag,
                                                                     command_data.size()));
        struct send_context {
            LinkNumberType dst_player_no;
            std::size_t retry_cnt                    = 0;
            std::size_t retry_threshold_cnt          = 0;
            std::atomic_bool is_send_action_finished = false;
            netlink_flush_interface::flush_data_status status;
            Fundamental::error_code send_ec;
            std::string ret_data;
            std::shared_ptr<netlink_flush_interface::netlink_status_change_cb> result_cb;
        };
        std::map<LinkNumberType, std::shared_ptr<send_context>> contexts;
        // result_cb's lifetime should  be shorter than contexts
        Fundamental::ScopeGuard manager_g([&]() {
            for (auto& c : contexts)
                c.second->result_cb.reset();
        });
        std::atomic_bool send_status_has_changed = false;
        bool has_delay_retry_demand              = false;
        for (auto no : dst_players) {
            auto& new_context          = contexts[no];
            new_context                = std::make_shared<send_context>();
            new_context->dst_player_no = no;
            new_context->result_cb     = std::make_shared<netlink_flush_interface::netlink_status_change_cb>(
                [&, op_context = new_context](netlink_flush_interface::flush_data_status status,
                                              Fundamental::error_code send_ec, std::string ret_data) mutable {
                    auto& context                   = *op_context;
                    context.status                  = status;
                    context.send_ec                 = send_ec;
                    context.ret_data                = std::move(ret_data);
                    context.is_send_action_finished = true;
                    send_status_has_changed         = true;
                    pdata->notify_send_status_update();
                });

            pdata->ref_interface.flush_data(command_data, pdata->config.all_players[no],
                                            pdata->config.network_timeout_sec * 1000, new_context->result_cb);
        }
        // wait all data sent
        while (!contexts.empty()) {
            for (auto iter = contexts.begin(); iter != contexts.end();) {
                try {
                    // failed retry case
                    if (iter->second->retry_cnt > 0) {
                        iter->second->retry_cnt++;
                        if (iter->second->retry_cnt >= iter->second->retry_threshold_cnt) {
                            iter->second->retry_cnt = 0;

                            pdata->ref_interface.flush_data(
                                command_data, pdata->config.all_players[iter->second->dst_player_no],
                                pdata->config.network_timeout_sec * 1000, iter->second->result_cb);

                        } else {
                            has_delay_retry_demand = true;
                        }
                        // wait next turn retry
                        ++iter;
                        continue;
                    }
                    // unfinished case
                    if (!iter->second->is_send_action_finished) {
                        ++iter;
                        continue;
                    }
                    // normally finished
                    if (!iter->second->send_ec) {
                        iter = contexts.erase(iter);
                        continue;
                    }
                    // normally failed
                    if (iter->second->status == netlink_flush_interface::flush_data_success_status) {
                        ec = make_error_code(netlink_errors::netlink_failed,
                                             Fundamental::StringFormat("send link data[{}] by {} failed:{}", key,
                                                                       call_tag, iter->second->send_ec));
                        break;
                    }
                    // network poor
                    if (iter->second->status == netlink_flush_interface::flush_data_too_many_request_status) {
                        ec = make_error_code(
                            netlink_errors::netlink_failed,
                            Fundamental::StringFormat(
                                "send link data[{}] by {} failed:{} too many pending request,network is poor", key,
                                call_tag, iter->second->send_ec));
                        break;
                    }
                    // network unreachable,wait retry
                    iter->second->retry_cnt = 1;
                    has_delay_retry_demand  = true;
                    iter->second->retry_threshold_cnt++;
                    if (iter->second->retry_threshold_cnt >= pdata->config.max_send_max_try_interval_cnt) {
                        iter->second->retry_threshold_cnt = pdata->config.max_send_max_try_interval_cnt;
                    }
                    ++iter;

                } catch (...) {
                    ec = make_error_code(
                        netlink_errors::netlink_failed,
                        Fundamental::StringFormat("throw failed when send link data[{}] by {}", key, call_tag));
                    break;
                }
            }
            if (contexts.empty() || ec) break;
            std::unique_lock<std::mutex> locker(pdata->send_status_mutex);
            // loop to check timeout and send status changed
            // break when timeout with resend demand
            while (!pdata->send_status_changed_notify_cv.wait_for(
                       locker, std::chrono::milliseconds(pdata->config.kNetworkRetryIntervalMsec),
                       [&]() { return pdata->is_cancelled.load() || send_status_has_changed; }) &&
                   !has_delay_retry_demand) {
            }
            if (pdata->is_cancelled) {
                ec = make_error_code(netlink_errors::netlink_operation_cancelled,
                                     Fundamental::StringFormat("send link data[{}] by {} was aborted", key, call_tag));
                break;
            }
            has_delay_retry_demand = false;
            // reset send_status_has_changed flag
            send_status_has_changed.exchange(false);
        }
    } while (0);
    return ec;
}

Fundamental::error_code netlink::recv_data(std::string key, link_data_map& out, std::string_view call_tag) {
    if (pdata->is_cancelled) {
        return make_error_code(netlink_errors::netlink_operation_cancelled,
                               Fundamental::StringFormat("player:{} recv timeout/cancel signal when {} wait {} ",
                                                         pdata->config.local_player_no, call_tag, key));
    }
    do {
        out.erase(pdata->config.local_player_no);
        if (out.empty()) {
            break;
        }
        // check player_no
        for (const auto& item : out) {
            if (item.first >= pdata->config.player_nums) {
                return make_error_code(netlink_errors::netlink_invalid_argument,
                                       Fundamental::StringFormat("recv data player:{} is invalid,player_nums:{}",
                                                                 item.first, pdata->config.player_nums));
            }
        }
        pdata->ref_interface.status_report(Fundamental::StringFormat("player:{} wait link data[{}] by {}",
                                                                     pdata->config.local_player_no, key, call_tag));
        std::unique_lock<std::mutex> locker(pdata->link_status_update_notify_mutex);
        auto max_phase_wait_cnt = pdata->config.link_timeout_sec / 2;
        if (max_phase_wait_cnt < pdata->config.kMinLinkCheckCnt) max_phase_wait_cnt = pdata->config.kMinLinkCheckCnt;
        for (auto& item : out) {
            auto& phase_storage                    = pdata->link_status_cache_storage[key];
            std::size_t progressing_check_flag_cnt = max_phase_wait_cnt;
            std::size_t timeout_cnt                = 0;
            while (true) {
                auto ret = pdata->link_status_update_notify_cv.wait_for(
                    locker, std::chrono::milliseconds(pdata->config.kNetworkRetryIntervalMsec),
                    [&]() { return phase_storage.count(item.first) != 0 || pdata->is_cancelled.load(); });
                // wait next loop
                if (!ret) {
                    ++timeout_cnt;
                    // update progresing check cnt
                    if (progressing_check_flag_cnt > 0) --progressing_check_flag_cnt;
                    if (0 == timeout_cnt % max_phase_wait_cnt) {
                        auto [ec, is_phase_progressing] = pdata->check_link_status(item.first, key);
                        if (ec) {
                            return make_error_code(
                                netlink_errors::netlink_failed,
                                Fundamental::StringFormat(
                                    "player:{} check player:{} link status failed:{}  when {} wait {}",
                                    pdata->config.local_player_no, item.first, ec, call_tag, key));
                        }
                        pdata->ref_interface.status_report(Fundamental::StringFormat(
                            "player:{} wait link data:[{}]  from other players by {} check_cnt:{} result:{}",
                            pdata->config.local_player_no, key, call_tag, timeout_cnt, is_phase_progressing));
                        if (is_phase_progressing) {
                            // reset check status
                            progressing_check_flag_cnt = max_phase_wait_cnt;
                            pdata->ref_interface.check_keep_alive();
                        } else {
                            if (0 == progressing_check_flag_cnt) {
                                // no progresing updated,we should abort this task
                                return make_error_code(
                                    netlink_errors::netlink_failed,
                                    Fundamental::StringFormat(
                                        "player:{} check player:{} link timeout for {} second when {} wait {} ",
                                        pdata->config.local_player_no, item.first, max_phase_wait_cnt, call_tag, key));
                            }
                        }
                    }
                    continue;
                }

                if (pdata->is_cancelled) {
                    return make_error_code(
                        netlink_errors::netlink_operation_cancelled,
                        Fundamental::StringFormat("player:{} recv timeout/cancel signal when {} wait {} ",
                                                  pdata->config.local_player_no, call_tag, key));
                }
                auto data_iter = phase_storage.find(item.first);
                if (data_iter == phase_storage.end()) continue;
                item.second = std::move(data_iter->second);
                phase_storage.erase(data_iter);
                break;
            }
            if (phase_storage.empty()) {
                pdata->link_status_cache_storage.erase(key);
            }
        }
    } while (0);
    return {};
}

std::tuple<Fundamental::error_code, std::string> netlink::push_data(std::string data) {
    LinkCommandBaseData request;
    Fundamental::error_code ec;
    std::string ret_data;
    do {
        if (pdata->is_cancelled) {
            ec = make_error_code(
                netlink_errors::netlink_operation_cancelled,
                Fundamental::StringFormat("player:{} was already released", pdata->config.local_player_no));
            break;
        }
        if (!Fundamental::io::from_json(data, request)) {
            ec = make_error_code(netlink_errors::netlink_invalid_argument,
                                 Fundamental::StringFormat("bad link data:{}", data.substr(0, kMaxDataDumpSize)));
            break;
        }
        switch (request.type) {
        case LinkCommandEnumType::LinkCommandType: ec = pdata->handle_link_data(std::move(data)); break;
        case LinkCommandEnumType::LinkCheckCommandType: {
            auto [ret_ec, ret_data_tmp] = pdata->handle_check_data(std::move(data));
            ec                          = ret_ec;
            ret_data                    = std::move(ret_data_tmp);
            break;
        }

        default: return handle_custom_data(request.type, std::move(data));
        }
    } while (0);

    return std::make_tuple(ec, std::move(ret_data));
}
std::tuple<Fundamental::error_code, std::string> netlink::send_command(std::uint64_t player_no,
                                                                       std::string command_data) {
    Fundamental::error_code ec;
    std::string data;
    do {
        if (pdata->is_cancelled) {
            ec = make_error_code(netlink_errors::netlink_operation_cancelled,
                                 Fundamental::StringFormat("link was already aborted"));
            break;
        }
        if (player_no == pdata->config.local_player_no || command_data.empty()) break;
        if (player_no >= pdata->config.player_nums) {
            ec = make_error_code(
                netlink_errors::netlink_invalid_argument,
                Fundamental::StringFormat("invalid player no:{} player_nums:{}", player_no, pdata->config.player_nums));
        }
        try {
            std::atomic_bool is_send_finished = false;
            auto result_cb                    = std::make_shared<netlink_flush_interface::netlink_status_change_cb>(
                [&](netlink_flush_interface::flush_data_status ready, Fundamental::error_code send_ec,
                    std::string ret_data) {
                    ec   = send_ec;
                    data = std::move(ret_data);
                    is_send_finished.exchange(true);
                    pdata->notify_send_status_update();
                });
            pdata->ref_interface.flush_data(std::move(command_data), pdata->config.all_players[player_no],
                                            pdata->config.network_timeout_sec * 1000, result_cb);
            std::unique_lock<std::mutex> locker(pdata->send_status_mutex);
            while (!pdata->send_status_changed_notify_cv.wait_for(
                locker, std::chrono::milliseconds(pdata->config.kNetworkRetryIntervalMsec),
                [&]() -> bool { return pdata->is_cancelled || is_send_finished; })) {
            }
            if (!is_send_finished) {
                if (pdata->is_cancelled) {
                    ec = make_error_code(netlink_errors::netlink_operation_cancelled,
                                         Fundamental::StringFormat("link was already aborted"));
                    break;
                }
            }
        } catch (...) {
            ec = make_error_code(netlink_errors::netlink_failed,
                                 Fundamental::StringFormat("throw when send command data"));
            break;
        }

    } while (0);
    return std::make_tuple(ec, std::move(data));
}
std::tuple<Fundamental::error_code, std::string> netlink::handle_custom_data(std::int32_t type, std::string data) {
    return std::make_tuple(make_error_code(netlink_errors::netlink_unsupported_type,
                                           Fundamental::StringFormat("bad link command type:{} data:{}", type,
                                                                     data.substr(0, kMaxDataDumpSize))),
                           "");
}

} // namespace link
} // namespace network

RTTR_REGISTRATION {
    network::link::__register_netlink_reflect_type__();
}