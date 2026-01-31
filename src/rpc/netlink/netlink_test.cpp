
#include "fundamental/basic/log.h"
#include "fundamental/basic/parallel.hpp"
#include "fundamental/basic/random_generator.hpp"
#include "fundamental/basic/utils.hpp"
#include "fundamental/basic/uuid_utils.hpp"
#include "fundamental/rttr_handler/serializer.h"

#include "rpc/rpc_server.hpp"
#include "rpc_netlink_flush_imp.hpp"
#include <unordered_map>

using namespace network;

struct test_party {
    std::uint64_t task_id;
    std::uint64_t player_no;
    std::unique_ptr<link::netlink> link_instance;
    std::unique_ptr<link::imp::rpc_netlink_writer> writer;
};
static std::size_t s_link_timeout_sec = 5;
static std::unordered_map<std::uint64_t, test_party> all_parties;

static link::imp::rpc_netlink_response handle_link_request(rpc_service::rpc_conn conn,
                                                           link::imp::rpc_netlink_request request);
static void process_link_test(std::uint64_t player_no);

static auto s_server = std::make_shared<rpc_service::rpc_server>(9000);
static network::proxy::ProxyManager s_manager;
int main(int argc, char* argv[]) {
    if (!::getenv("enable_test") || argc < 2) return 0;
    std::size_t test_players = std::stoul(argv[1]);
    if (test_players < 2) return 0;
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel = Fundamental::LogLevel::debug;
    options.logFormat    = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    Fundamental::Logger::Initialize(std::move(options));
    auto& server = *s_server;
    network::rpc_server_external_config external_config;
    external_config.enable_transparent_proxy = true;
    external_config.transparent_proxy_host   = "127.0.0.1";
    external_config.transparent_proxy_port   = "9000";
    server.set_external_config(external_config);
    server.register_delay_handler(link::imp::rpc_netlink_request::kRpcName, handle_link_request);
    network::init_io_context_pool(1);
    {
        using namespace network::proxy;
        auto& manager = s_manager;
        manager.AddWsProxyRoute("/", proxy::ProxyHost { "127.0.0.1", "9000" });
        server.enable_data_proxy(&s_manager);
    }
    server.start();
    std::thread t([]() { Fundamental::Application::Instance().Loop(); });
    for (std::size_t no = 0; no < test_players; ++no) {
        std::size_t cluster_no                = no / 4;
        std::size_t cluster_player_machine_id = no % 4;
        std::size_t player_id                 = no % 4;
        // 0 , 1 same instance
        if (player_id == 1) {
            player_id = 0;
        }
        // 0 , 2 same machine
        if (player_id == 2) {
            cluster_player_machine_id = 0;
        }
        // 0 , 3 same cluster
        auto& party     = all_parties[no];
        party.task_id   = 1;
        party.player_no = no;
        party.writer    = std::make_unique<link::imp::rpc_netlink_writer>(
            1, link::imp::rpc_netlink_forward_config { "127.0.0.1", "9000" });
        party.writer->init(test_players);
        {
            link::netlink_config config;
            config.link_timeout_sec    = s_link_timeout_sec;
            config.network_timeout_sec = 5;
            config.player_nums         = test_players;
            config.all_players.resize(test_players);
            config.local_player_no = no;
            auto& local_player     = config.all_players[no];
            local_player.player_no = no;
            local_player.player_id = Fundamental::StringFormat("cluster_player_{}_{}", cluster_no, player_id);
            local_player.machine_id =
                Fundamental::StringFormat("cluster_machine_{}_{}", cluster_no, cluster_player_machine_id);
            local_player.master_id                     = Fundamental::StringFormat("cluster_{}", cluster_no);
            local_player.network_info.external_host    = "127.0.0.1";
            local_player.network_info.external_service = "9000";
            local_player.network_info.host             = "127.0.0.1";
            local_player.network_info.service          = "9000";
            local_player.network_info.master_host      = "127.0.0.1";
            local_player.network_info.master_service   = "9000";
            local_player.network_info.route_path       = "/";
            party.link_instance                        = std::make_unique<link::netlink>(config, *party.writer);
            Fundamental::Application::Instance().exitStarted.Connect(
                [&]() { party.link_instance.get()->abort_link(); });
        }
    }

    auto handle_func = [&](std::size_t start_index, std::size_t nums, std::size_t /*group*/
                       ) { process_link_test(start_index); };
    Fundamental::ThreadPoolParallelExecutor executor { test_players - 1 };
    // process query gen
    Fundamental::ParallelRun((std::size_t)0, test_players, handle_func, 1, executor);
    Fundamental::Application::Instance().Exit();
    t.join();
    return 0;
}

link::imp::rpc_netlink_response handle_link_request(rpc_service::rpc_conn conn,
                                                    link::imp::rpc_netlink_request request) {
    link::imp::rpc_netlink_response response;
    auto player_no = request.player_no;
    if (player_no >= all_parties.size() || request.tag != 1) {
        response.code = 1;
        response.msg  = Fundamental::StringFormat("invalid link info {}:{}", request.tag, player_no);
    } else {
        auto [ec, ret_string] = all_parties[player_no].link_instance->push_data(std::move(request.data));
        response.code         = ec.value();
        response.msg          = ec.full_message();
        response.data         = std::move(ret_string);
    }
    return response;
}

void process_link_test(std::uint64_t player_no) {
    auto& party = all_parties[player_no];
    Fundamental::ScopeGuard release_g([&]() {
        FINFO("player_no:{} finished all test case", player_no);
        party.link_instance->abort_link();
    });
    party.link_instance->online_setup(all_parties[0].link_instance.get()->get_config().all_players[0].network_info, 0);
    // notify global config
    {
        std::string key       = "global config";
        std::string test_data = Fundamental::StringFormat("{}_0", key);
        if (player_no == 0) {
            party.link_instance->send_data(test_data, key, party.link_instance->generate_dst_players(),
                                           Fundamental::StringFormat("{}_{}", __func__, __LINE__));
        } else {
            link::netlink::link_data_map map;
            auto& recv_data = map[0];
            auto ec = party.link_instance->recv_data(key, map, Fundamental::StringFormat("{}_{}", __func__, __LINE__));
            FASSERT_ACTION(!ec && recv_data == test_data, throw, "player:{} {}", player_no, ec);
        }
    }
    // test exchange results
    // Each player receives data from the two players with the previous sequential numbers, and sends data to the two
    // players with the next sequential numbers.
    {
        std::string key       = "exchange loop two players";
        std::string send_data = Fundamental::StringFormat("{}_{}", key, player_no);
        link::netlink::link_data_map recv_map;
        link::netlink::link_dst_player_set send_dst_players;
        std::size_t send_begin_player_no = player_no + all_parties.size();
        std::size_t recv_begin_player_no = player_no;
        std::size_t loop_cnt             = 2;
        do {
            --send_begin_player_no;
            ++recv_begin_player_no;
            --loop_cnt;
            send_dst_players.insert(send_begin_player_no % all_parties.size());
            recv_map[recv_begin_player_no % all_parties.size()] = "";
        } while (loop_cnt > 0);

        auto ec = party.link_instance->exchange(send_data, key, send_dst_players, recv_map,
                                                Fundamental::StringFormat("{}_{}", __func__, __LINE__));
        FASSERT_ACTION(!ec, throw, "player:{} {}", player_no, ec);
        for (auto& item : recv_map) {
            FASSERT_ACTION(item.second == Fundamental::StringFormat("{}_{}", key, item.first), throw,
                           "player:{} exchange from:{}", player_no, item.first);
        }
    }
    // test busy wait
    {
        std::string key       = "test busy wait";
        std::string test_data = Fundamental::StringFormat("{}_0", key);
        if (player_no >= 2) {
            // busy wait player 0 send this key data,but player 0 wontt send this key
            // this case will always failed
            link::netlink::link_data_map map;
            map[0]  = "";
            auto ec = party.link_instance->recv_data(key, map, Fundamental::StringFormat("{}_{}", __func__, __LINE__));
            FASSERT_ACTION(ec, throw, "player:{} {}", player_no, ec);
            return;
        } else if (player_no == 1) {
            std::size_t sleep_cnt = s_link_timeout_sec * 2;
            while (sleep_cnt > 0) {
                party.link_instance->declare_key_is_progressing(key);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                sleep_cnt--;
            }
            auto ec = party.link_instance->send_data("", key, std::set<std::uint64_t> { 0 },
                                                     Fundamental::StringFormat("{}_{}", __func__, __LINE__));
            FASSERT_ACTION(!ec, throw, "player:{} {}", player_no, ec);
            return;
        } else if (player_no == 0) {
            link::netlink::link_data_map map;
            auto& recv_data = map[1];
            auto ec = party.link_instance->recv_data(key, map, Fundamental::StringFormat("{}_{}", __func__, __LINE__));
            FASSERT_ACTION(!ec && recv_data.empty(), throw, "player:{} {}", player_no, ec);
        }
    }
    // test failed send
    {
        std::string key       = "test failed send";
        std::string test_data = Fundamental::StringFormat("{}_0", key);
        // sleep just ten milleseconds to wait player 1 thread exited
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto ec = party.link_instance->send_data("", key, std::set<std::uint64_t> { 1 },
                                                 Fundamental::StringFormat("{}_{}", __func__, __LINE__));
        FASSERT_ACTION(ec, throw, "player:{} {}", player_no, ec);
    }
}