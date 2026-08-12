#include "frp_accessor.hpp"
#include "frp_client.hpp"   // for relay_data_channel, frp_tcp_channel
#include "frp_signal_client.hpp"
#include "frp_common.hpp"
#include "frp_kcp_crypto.hpp"
#include "frp_punch_engine.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/uuid_utils.hpp"

namespace {
std::string sha256_hex(std::string_view input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
    return Fundamental::Utils::BufferToHex(hash, SHA256_DIGEST_LENGTH);
}
} // namespace


namespace network::proxy
{

frp_accessor::frp_accessor(std::shared_ptr<frp_signal_client> signal) :
signal_(std::move(signal)), resubscribe_timer_(signal_->get_executor()) {
    io_context_pool::Instance().reg_timer(resubscribe_timer_);
}

frp_accessor::~frp_accessor() {
    io_context_pool::Instance().unreg_timer(resubscribe_timer_);
}

void frp_accessor::on_subscribe(const std::vector<frp_visible_service_data>& services) {

    std::unordered_set<std::string> cur;

    visible_services_.clear();

    for (const auto& svc : services) {

        cur.insert(Fundamental::StringFormat("{}@{}", svc.service_name, svc.provider_uuid));

        // 快照键与 reconcile_listeners 的 by_name 一致，且排除自己提供的服务

        if (signal_->uuid() != svc.provider_uuid) {

            visible_services_.insert(Fundamental::StringFormat("{}:{}", svc.service_name,
                                                               static_cast<int>(svc.service_type)));

        }

    }

    if (cur != last_known_services_) {

        FINFO("subscribe services={} (changed)", services.size());

        last_known_services_ = std::move(cur);

        reconcile_listeners(services);

    }

    // 期望服务缺失时立即收紧重试节奏，覆盖
    // "服务端重启后 provider 注册晚于 accessor 订阅快照" 的竞态。

    if (!desired_services_satisfied()) schedule_resubscribe();

}


void frp_accessor::on_client_command(std::uint8_t cmd, std::string payload) {
    frp_client_command_base ccmd;
    if (!Fundamental::io::from_json(payload, ccmd)) return;
    switch (ccmd.command) {
    case frp_client_accept: {
        frp_client_accept_data a;
        if (!Fundamental::io::from_json(payload, a)) break;
        FINFO("recv accept conn={}", a.connection_uuid);
        auto it = channels_.find(a.connection_uuid);
        if (it != channels_.end()) {
            auto& c = it->second;
            c->set_traffic_secret(signal_->config().traffic_secret);
            c->init_kcp();
            for (auto& pw : c->pending_writes())
                c->send_bytes(pw->data(), pw->size());
            c->pending_writes().clear();
            start_data_forward_read_loop(c);
            maybe_start_p2p(c);
        }
        break;
    }
    case frp_client_reject: {
        frp_client_reject_data r;
        if (!Fundamental::io::from_json(payload, r)) break;
        FWARN("recv reject conn={} reason={}", r.connection_uuid, r.reason);
        auto it = channels_.find(r.connection_uuid);
        if (it != channels_.end()) close_data_channel(it->second);
        break;
    }
    case frp_client_provider_p2p_handshake:
    case frp_client_provider_confirm_ack:
    case frp_client_provider_probe_match:
        handle_p2p_message(ccmd.command, std::move(payload));
        break;
    default: break;
    }
}



void frp_accessor::subscribe_all_keys() {

    resubscribe_timer_.cancel();

    resubscribe_attempts_ = 0; // 每次重新连接（auth ok）重置预算

    send_subscribe_request();

    schedule_resubscribe(); // 启动自适应刷新链（快速重试 -> 30s 周期探测）

}

void frp_accessor::send_subscribe_request() {

    frp_subscribe_services_data req;

    req.command = frp_subscribe_services_command;

    for (const auto& g : signal_->config().groups) {

        if (g.listeners.empty()) continue;

        req.register_keys.push_back(g.register_key);

    }

    if (!req.register_keys.empty()) signal_->send_command(req);

}

bool frp_accessor::desired_services_satisfied() const {

    for (const auto& group : signal_->config().groups) {

        for (const auto& lc : group.listeners) {

            auto key = Fundamental::StringFormat("{}:{}", lc.service_name,
                                                 static_cast<int>(lc.service_type));

            if (!visible_services_.count(key)) return false;

        }

    }

    return true;

}

void frp_accessor::schedule_resubscribe() {

    resubscribe_timer_.cancel();

    if (!signal_->is_reference_valid()) return;

    bool has_listeners = false;

    for (const auto& group : signal_->config().groups)

        if (!group.listeners.empty()) { has_listeners = true; break; }

    if (!has_listeners) return; // 纯 provider 无需订阅刷新

    std::chrono::seconds interval = kResubscribeRefreshInterval;

    if (!desired_services_satisfied()) {

        if (resubscribe_attempts_ < kMaxResubscribeAttempts) {

            ++resubscribe_attempts_;

            interval = kResubscribeFastInterval; // 快速路径：1s 一次，最多 10 次

        } else if (resubscribe_attempts_ == kMaxResubscribeAttempts) {

            ++resubscribe_attempts_; // 进入降级态，只告警一次

            FWARN("resubscribe fast path exhausted, fall back to {}s periodic refresh",
                  kResubscribeRefreshInterval.count());

        }

    } else {

        resubscribe_attempts_ = 0; // 满足时重置预算，下次变化重新获得完整快速路径

    }

    resubscribe_timer_.expires_after(interval);

    resubscribe_timer_.async_wait([this, self = shared_from_this()](const std::error_code& ec) {

        if (ec || !signal_->is_reference_valid()) return;

        send_subscribe_request();

        schedule_resubscribe();

    });

}


void frp_accessor::reconcile_listeners(const std::vector<frp_visible_service_data>& services) {

    std::unordered_map<std::string, frp_visible_service_data> by_name;

    for (const auto& svc : services) {

        if (signal_->uuid() == svc.provider_uuid) continue;

        by_name[Fundamental::StringFormat("{}:{}", svc.service_name, static_cast<int>(svc.service_type))] = svc;

    }

    std::unordered_set<std::string> desired;

    for (const auto& group : signal_->config().groups) {

        for (const auto& lc : group.listeners) {

            auto lk = Fundamental::StringFormat("{}:{}", lc.service_name, static_cast<int>(lc.service_type));

            auto it = by_name.find(lk);

            if (it == by_name.end()) { FWARN("service not found: {}", lc.service_name); continue; }

            const auto key = Fundamental::StringFormat("{}:{}:{}", lc.service_name, lc.listen_host, lc.listen_port);

            desired.insert(key);

            if (listeners_.count(key)) {
                // 提供方可能已变化（provider 进程重启、新 uuid 接管同名服务）：
                // 必须更新绑定，否则 channel_open 打到失效 uuid，server 静默丢弃 -> 永远等不到 accept。
                auto& lst   = listeners_[key];
                const auto& svc = it->second;
                if (lst->provider_uuid != svc.provider_uuid ||
                    lst->provider_enable_p2p != svc.enable_p2p ||
                    lst->provider_nat_type != svc.provider_nat_type ||
                    lst->provider_startup_rtt_ms != svc.provider_startup_rtt_ms) {
                    FINFO("listener {} rebind provider {} -> {}", lst->service_name, lst->provider_uuid,
                          svc.provider_uuid);
                    lst->provider_uuid = svc.provider_uuid;
                    lst->provider_enable_p2p = svc.enable_p2p;
                    lst->provider_nat_type = svc.provider_nat_type;
                    lst->provider_startup_rtt_ms = svc.provider_startup_rtt_ms;
                    lst->register_key = group.register_key;
                }
                continue;
            }



            const auto& svc = it->second;

            auto lst = std::make_shared<listener_runtime>(signal_->get_executor());

            lst->service_name = lc.service_name;

            lst->listen_host = lc.listen_host;

            lst->listen_port = lc.listen_port;

            lst->service_type = lc.service_type;

            lst->enable_p2p = lc.enable_p2p;

            lst->provider_enable_p2p = svc.enable_p2p;

            lst->provider_nat_type = svc.provider_nat_type;

            lst->provider_startup_rtt_ms = svc.provider_startup_rtt_ms;

            lst->register_key = group.register_key;

            lst->provider_uuid = svc.provider_uuid;



            std::error_code ec;

            auto addr = asio::ip::make_address(lc.listen_host, ec);

            if (ec) continue;



            if (lc.service_type == frp_service_udp) {

                lst->udp_socket = std::make_shared<asio::ip::udp::socket>(signal_->get_executor());

                lst->udp_socket->open(addr.is_v6() ? asio::ip::udp::v6() : asio::ip::udp::v4(), ec);

                if (ec) continue;

                lst->udp_socket->set_option(asio::ip::udp::socket::reuse_address(true), ec);

                lst->udp_socket->bind(asio::ip::udp::endpoint(addr, lc.listen_port), ec);

                if (ec) { FERR("bind udp listener {} err={}", lc.service_name, ec.message()); continue; }

                FINFO("udp listener {} :{}", lst->service_name, lc.listen_port);

                start_udp_receive_loop(lst);

            } else {

                lst->acceptor.open(addr.is_v6() ? asio::ip::tcp::v6() : asio::ip::tcp::v4(), ec);

                if (ec) continue;

                lst->acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);

                lst->acceptor.bind(asio::ip::tcp::endpoint(addr, lc.listen_port), ec);

                if (ec) { FERR("bind tcp listener {} err={}", lc.service_name, ec.message()); continue; }

                lst->acceptor.listen(asio::socket_base::max_listen_connections, ec);

                if (ec) continue;

                FINFO("tcp listener {} :{}", lst->service_name, lc.listen_port);

                start_tcp_accept_loop(lst);

            }

            listeners_[key] = std::move(lst);

        }

    }

    for (auto it = listeners_.begin(); it != listeners_.end();) {

        if (!desired.count(it->first)) {

            std::error_code ec; it->second->acceptor.close(ec);

            if (it->second->udp_socket) { it->second->udp_socket->close(ec); it->second->udp_socket.reset(); }

            it = listeners_.erase(it);

        } else ++it;

    }

}


std::string frp_accessor::generate_connection_uuid() { return frp_generate_uuid(); }


void frp_accessor::open_data_channel(const std::shared_ptr<relay_data_channel>& ch) {

    frp_client_open_data od;

    od.command = frp_client_open;

    od.accessor_uuid = signal_->uuid();

    od.connection_uuid = ch->connection_uuid();

    od.register_nonce = frp_generate_uuid();

    od.register_hash  = sha256_hex(ch->register_key() + od.register_nonce);

    od.service_name = ch->service_name();

    od.transport = ch->transport();



    auto open_json = Fundamental::io::to_json(od);



    frp_channel_open_request_data req;

    req.command = frp_channel_open_command;

    req.from_uuid = signal_->uuid();

    req.dst_uuid = ch->peer_uuid();

    req.connection_uuid = ch->connection_uuid();

    req.status = 0;

    req.payload = std::move(open_json);

    auto packet = packet_frp_command_data(req);

    if (!packet) { close_data_channel(ch); return; }



    auto self = shared_from_this();

    auto tcp = frp_tcp_channel::make_shared(

        signal_->get_executor(),

        signal_->config().public_server_host, std::to_string(signal_->config().public_server_tcp_port));

    tcp->enable_ssl(to_network_config(signal_->config().ssl));

    tcp->notify_connect_result.Connect(shared_from_this(),

        [this, self, ch, packet = std::move(packet)](Fundamental::error_code ec, std::shared_ptr<frp_tcp_channel> t) mutable {

            if (!signal_->is_reference_valid() || ch->is_closed()) return;

            if (ec || !t) { close_data_channel(ch); return; }

            ch->attach_tcp(std::move(t));

            ch->tcp()->async_write_framed(packet);

            // Data forwarding starts when accept arrives on signal channel.

        });

    tcp->start_async_connect();

}


void frp_accessor::start_local_read_loop(const std::shared_ptr<relay_data_channel>& ch) {

    if (!ch || ch->is_closed()) return;

    auto self = shared_from_this();

    ch->local_socket().async_read_some(asio::buffer(ch->read_buf().data(), ch->read_buf().size()),

        [this, self, ch](const std::error_code& ec, std::size_t n) {

            if (!signal_->is_reference_valid() || ch->is_closed()) return;

            if (ec) { close_data_channel(ch); return; }

            if (n > 0) {

                if (ch->has_kcp()) {

                    ch->send_bytes(ch->read_buf().data(), n);

                } else {

                    // KCP 未就绪（本地连接早于 accept 到达）：缓冲，init_kcp 后统一经 KCP 补发。

                    ch->pending_writes().emplace_back(
                        std::make_shared<std::string>(ch->read_buf().data(), n));

                }

            }

            start_local_read_loop(ch);

        });

}


void frp_accessor::start_data_forward_read_loop(const std::shared_ptr<relay_data_channel>& ch) {

    if (!ch->tcp() || ch->is_closed() || ch->is_p2p_active()) return;

    auto self = shared_from_this();

    ch->tcp()->async_read_raw([this, self, ch](const char* data, std::size_t n) {

        if (!signal_->is_reference_valid() || ch->is_closed()) return;



        // Feed relay data through KCP

        if (ch->has_kcp()) {

            ch->feed_kcp(data, n);

        }

        start_data_forward_read_loop(ch);

    });

}


void frp_accessor::close_data_channel(const std::shared_ptr<relay_data_channel>& ch) {

    if (!ch || ch->is_closed()) return;

    FINFO("close_data_channel conn={}", ch->connection_uuid());

    ch->close();

}


void frp_accessor::start_tcp_accept_loop(const std::shared_ptr<listener_runtime>& lst) {

    lst->acceptor.async_accept([this, self = shared_from_this(), lst](std::error_code ec, asio::ip::tcp::socket sock) {

        if (!signal_->is_reference_valid()) return;

        if (!ec) {

            auto cid = generate_connection_uuid();

            auto ch = relay_data_channel::create(signal_->get_executor(), cid, lst->provider_uuid);

            ch->local_socket() = std::move(sock);

            ch->set_register_key(lst->register_key);

            ch->set_service_name(lst->service_name);

            ch->set_transport(lst->service_type);

            ch->set_on_release([this, cid]() { channels_.erase(cid); });

            ch->set_idle_timeout_seconds(signal_->config().data_channel_idle_timeout_seconds);

            channels_[cid] = ch;

            start_local_read_loop(ch);

            open_data_channel(ch);

        }

        start_tcp_accept_loop(lst);

    });

}


void frp_accessor::start_udp_receive_loop(const std::shared_ptr<listener_runtime>& lst) {

    if (!lst->udp_socket) return;

    lst->udp_socket->async_receive_from(

        asio::buffer(lst->udp_recv_buf.data(), lst->udp_recv_buf.size()), lst->udp_recv_endpoint_,

        [this, self = shared_from_this(), lst](std::error_code ec, std::size_t n) {

            if (!signal_->is_reference_valid() || !lst->udp_socket) return;

            if (ec) { start_udp_receive_loop(lst); return; }

            auto& ep = lst->udp_recv_endpoint_;

            auto it = lst->udp_sessions.find(ep);

            std::shared_ptr<relay_data_channel> ch;

            if (it != lst->udp_sessions.end()) { ch = it->second.lock(); if (!ch) lst->udp_sessions.erase(it); }

            if (!ch) {

                auto cid = generate_connection_uuid();

                ch = relay_data_channel::create(signal_->get_executor(), cid, lst->provider_uuid);

                ch->set_register_key(lst->register_key);

                ch->set_service_name(lst->service_name);

                ch->set_transport(frp_service_udp);

                ch->local_udp_socket() = lst->udp_socket;

                ch->local_udp_endpoint() = ep;

                ch->set_on_release([cid, wl = std::weak_ptr<listener_runtime>(lst), ep]() {

                    if (auto l = wl.lock()) l->udp_sessions.erase(ep);

                });

                ch->set_idle_timeout_seconds(signal_->config().data_channel_idle_timeout_seconds);

                lst->udp_sessions[ep] = ch;

                channels_[cid] = ch;

                open_data_channel(ch);

            }

            if (ch && n > 0) {

                if (ch->has_kcp()) {

                    ch->send_bytes(lst->udp_recv_buf.data(), n);

                } else {

                    // KCP 未就绪：缓冲，init_kcp 后统一经 KCP 补发（禁止裸写进 KCP 流）

                    ch->pending_writes().emplace_back(std::make_shared<std::string>(lst->udp_recv_buf.data(), n));

                }

            }

            start_udp_receive_loop(lst);

        });

}


void frp_accessor::handle_p2p_message(std::uint8_t cmd, std::string payload) {

    switch (cmd) {

    case frp_client_accessor_punch_start: {

        frp_client_punch_start_data ps;

        if (!Fundamental::io::from_json(payload, ps)) return;

        FINFO("recv accessor_punch_start conn={}", ps.connection_uuid);

        auto it = channels_.find(ps.connection_uuid);

        if (it == channels_.end()) return;

        auto& ch = it->second;



        // Create punch engine on provider side

        if (!ch->punch_engine()) {

            auto self = shared_from_this();

            frp_punch_engine::config pcfg;

            pcfg.executor                = signal_->get_executor();

            pcfg.connection_uuid         = ch->connection_uuid();

            pcfg.peer_uuid               = ch->peer_uuid();

            pcfg.traffic_secret          = signal_->config().traffic_secret;

            pcfg.public_server_host      = signal_->config().public_server_host;

            pcfg.public_server_udp_port  = signal_->config().public_server_udp_port;

            pcfg.my_nat_type             = signal_->probed_nat_type();

            pcfg.my_rtt_ms               = signal_->startup_rtt_ms();

            pcfg.punch_seq               = 1;



            auto signal_sender = [this, self, peer_uuid = ch->peer_uuid()](std::string json_payload) {

                signal_->send_p2p_command(peer_uuid, std::move(json_payload));

            };



            auto engine = frp_punch_engine::create(std::move(pcfg), signal_sender);

            engine->set_on_endpoint_ready([this, self, ch](std::string ip, std::uint16_t port) {

                FINFO("punch endpoint ready conn={} external={}:{}", ch->connection_uuid(), ip, port);

                // Send provider_p2p_handshake with real external port

                frp_client_p2p_handshake_data hs;

                hs.command = frp_client_provider_p2p_handshake;

                hs.connection_uuid = ch->connection_uuid();

                hs.internal_ip = ip;

                hs.internal_port = port;

                hs.external_ip = ip;

                hs.external_port = port;

                hs.rtt_ms = signal_->startup_rtt_ms();

                hs.nat_type = signal_->probed_nat_type();

                hs.punch_seq = 1;

                signal_->send_p2p_command(ch->peer_uuid(), Fundamental::io::to_json(hs));

            });

            engine->set_on_probe_match([self, ch](std::uint16_t local_port, std::uint16_t peer_port,

                                                          std::uint16_t target_port, std::uint16_t peer_external_port) {

                FINFO("probe match conn={} local={} peer={} tgt={} ext_peer={}",

                      ch->connection_uuid(), local_port, peer_port, target_port, peer_external_port);

                ch->punch_engine()->send_provider_probe_match(local_port, peer_port, target_port, peer_external_port);

            });

            engine->set_on_success([self, ch](frp_punch_engine::punch_result result) {

                FINFO("p2p success conn={} local_port={} peer_port={}",

                      ch->connection_uuid(), result.local_port, result.peer_port);

                ch->accept_p2p(std::move(result.socket), result.peer_endpoint);

                ch->set_p2p_active(true);

                ch->handshake_timer().cancel();

            });

            engine->set_on_failed([self, ch] {

                FWARN("p2p failed conn={}", ch->connection_uuid());

            });



            ch->punch_engine() = engine;

            engine->start();



            // P2P handshake timeout

            ch->handshake_timer().expires_after(std::chrono::seconds(30));

            ch->handshake_timer().async_wait([self, ch](const std::error_code& ec) {

                if (ec || ch->is_closed() || ch->is_p2p_active()) return;

                FWARN("p2p handshake timeout conn={}", ch->connection_uuid());

                if (ch->punch_engine()) { ch->punch_engine()->release(); ch->punch_engine().reset(); }

            });

        }

        break;

    }

    case frp_client_provider_p2p_handshake: {

        frp_client_p2p_handshake_data hs;

        if (!Fundamental::io::from_json(payload, hs)) return;

        FINFO("recv provider_p2p_handshake conn={} external_port={}", hs.connection_uuid, hs.external_port);

        auto it = channels_.find(hs.connection_uuid);

        if (it == channels_.end()) return;

        auto& ch = it->second;

        if (!ch->punch_engine()) return;

        // Store peer info (mirrors old: on_peer_info with req.external_ip:external_port)

        ch->punch_engine()->on_peer_info(

            frp_punch_engine::peer_info{hs.external_ip, hs.external_port, hs.nat_type, hs.rtt_ms});

        // If own probe done, send ack + start punch now; else overwrite on_endpoint_ready

        // (mirrors old: set_on_endpoint_ready at line 1067)

        if (ch->my_external_port() != 0) {

            frp_client_p2p_handshake_data ack;

            ack.command = frp_client_accessor_handshake_ack;

            ack.connection_uuid = ch->connection_uuid();

            ack.internal_ip = ""; ack.internal_port = 0;

            ack.external_ip = ch->my_external_ip(); ack.external_port = ch->my_external_port();

            ack.rtt_ms = signal_->startup_rtt_ms();

            ack.nat_type = signal_->probed_nat_type();

            ack.punch_seq = hs.punch_seq;

            signal_->send_p2p_command(ch->peer_uuid(), Fundamental::io::to_json(ack));

            // Start synchronized punch (mirrors old: start_punch_at at line 1120)

            std::int64_t deadline_us = std::chrono::duration_cast<std::chrono::microseconds>(

                std::chrono::steady_clock::now().time_since_epoch()).count() + 5000000;

            ch->punch_engine()->start_punch_at(deadline_us);

        } else {

            auto self = shared_from_this();

            ch->punch_engine()->set_on_endpoint_ready([this, self, ch, punch_seq = hs.punch_seq]

                (std::string ip, std::uint16_t port) {

                ch->my_external_ip() = ip;

                ch->my_external_port() = port;

                FINFO("accessor endpoint ready, sending handshake_ack conn={} external={}:{}",

                      ch->connection_uuid(), ip, port);

                frp_client_p2p_handshake_data ack;

                ack.command = frp_client_accessor_handshake_ack;

                ack.connection_uuid = ch->connection_uuid();

                ack.internal_ip = ""; ack.internal_port = 0;

                ack.external_ip = ip; ack.external_port = port;

                ack.rtt_ms = signal_->startup_rtt_ms();

                ack.nat_type = signal_->probed_nat_type();

                ack.punch_seq = punch_seq;

                signal_->send_p2p_command(ch->peer_uuid(), Fundamental::io::to_json(ack));

                // Start synchronized punch

                std::int64_t deadline_us = std::chrono::duration_cast<std::chrono::microseconds>(

                    std::chrono::steady_clock::now().time_since_epoch()).count() + 5000000;

                ch->punch_engine()->start_punch_at(deadline_us);

            });

        }

        break;

    }

    case frp_client_accessor_handshake_ack: {

        frp_client_p2p_handshake_data ack;

        if (!Fundamental::io::from_json(payload, ack)) return;

        FINFO("recv accessor_handshake_ack conn={} external_port={}", ack.connection_uuid, ack.external_port);

        // Mirror old line 1102: if port==0, provider probe failed, wait retry

        if (ack.external_port == 0) return;

        auto pit = channels_.find(ack.connection_uuid);

        if (pit == channels_.end() || !pit->second->punch_engine()) return;

        auto& ch = pit->second;

        // Mirror old line 1106: on_peer_info with accessor's port

        ch->punch_engine()->on_peer_info(

            frp_punch_engine::peer_info{ack.external_ip, ack.external_port, ack.nat_type, ack.rtt_ms});

        // Mirror old line 1120: start_punch_at with local deadline

        std::int64_t deadline_us = std::chrono::duration_cast<std::chrono::microseconds>(

            std::chrono::steady_clock::now().time_since_epoch()).count() + 5000000;

        ch->punch_engine()->start_punch_at(deadline_us);

        break;

    }

    case frp_client_provider_probe_match: {

        frp_client_punch_confirm_data pm;

        if (!Fundamental::io::from_json(payload, pm)) return;

        FINFO("recv provider_probe_match conn={} local={} peer={}", pm.connection_uuid, pm.local_port, pm.peer_port);

        // Accessor: delegate to punch engine → sends accessor_punch_confirm

        auto it = channels_.find(pm.connection_uuid);

        if (it != channels_.end() && it->second->punch_engine())

            it->second->punch_engine()->on_provider_probe_match(pm.peer_port, pm.local_port,

                                                               pm.external_peer_port, pm.external_local_port);

        break;

    }

    case frp_client_accessor_punch_confirm: {

        frp_client_punch_confirm_data pc;

        if (!Fundamental::io::from_json(payload, pc)) return;

        FINFO("recv accessor_punch_confirm conn={}", pc.connection_uuid);

        // Provider: delegate to punch engine → sends provider_confirm_ack

        auto pit = channels_.find(pc.connection_uuid);

        if (pit != channels_.end() && pit->second->punch_engine())

            pit->second->punch_engine()->on_accessor_punch_confirm(pc.peer_port, pc.local_port,

                                                                  pc.external_peer_port, pc.external_local_port);

        break;

    }

    case frp_client_provider_confirm_ack: {

        frp_client_punch_confirm_data ack;

        if (!Fundamental::io::from_json(payload, ack)) return;

        FINFO("recv provider_confirm_ack conn={}", ack.connection_uuid);

        // Accessor: delegate to punch engine → p2p ready, sends confirm_ok

        auto it = channels_.find(ack.connection_uuid);

        if (it != channels_.end() && it->second->punch_engine())

            it->second->punch_engine()->on_provider_confirm_ack(ack.peer_port, ack.local_port,

                                                               ack.external_peer_port, ack.external_local_port);

        break;

    }

    case frp_client_accessor_confirm_ok: {

        frp_client_punch_confirm_data ok;

        if (!Fundamental::io::from_json(payload, ok)) return;

        FINFO("p2p punch complete conn={}", ok.connection_uuid);

        // Provider: delegate to punch engine → p2p ready

        auto pit = channels_.find(ok.connection_uuid);

        if (pit != channels_.end() && pit->second->punch_engine())

            pit->second->punch_engine()->on_accessor_confirm_ok(ok.peer_port, ok.local_port,

                                                               ok.external_peer_port, ok.external_local_port);

        break;

    }

    default: break;

    }

}


void frp_accessor::maybe_start_p2p(const std::shared_ptr<relay_data_channel>& ch) {

    if (signal_->config().public_server_udp_port == 0) return;

    if (signal_->probed_nat_type() == frp_nat_type_disabled) return;



    FINFO("starting p2p punch for conn={}", ch->connection_uuid());



    // Create punch engine

    auto self = shared_from_this();

    frp_punch_engine::config pcfg;

    pcfg.executor                = signal_->get_executor();

    pcfg.connection_uuid         = ch->connection_uuid();

    pcfg.peer_uuid               = ch->peer_uuid();

    pcfg.traffic_secret          = signal_->config().traffic_secret;

    pcfg.public_server_host      = signal_->config().public_server_host;

    pcfg.public_server_udp_port  = signal_->config().public_server_udp_port;

    pcfg.my_nat_type             = signal_->probed_nat_type();

    pcfg.my_rtt_ms               = signal_->startup_rtt_ms();

    pcfg.punch_seq               = 1;



    auto signal_sender = [this, self, peer_uuid = ch->peer_uuid()](std::string json_payload) {

        signal_->send_p2p_command(peer_uuid, std::move(json_payload));

    };



    auto engine = frp_punch_engine::create(std::move(pcfg), signal_sender);

    engine->set_on_endpoint_ready([self, ch](std::string ip, std::uint16_t port) {

        ch->my_external_ip() = ip;

        ch->my_external_port() = port;

        FINFO("punch endpoint ready conn={} external={}:{}", ch->connection_uuid(), ip, port);

    });

    engine->set_on_probe_match([self, ch](std::uint16_t local_port, std::uint16_t peer_port,

                                                  std::uint16_t target_port, std::uint16_t peer_external_port) {

        FINFO("probe match conn={} local={} peer={} tgt={} ext_peer={}",

              ch->connection_uuid(), local_port, peer_port, target_port, peer_external_port);

        // Provider side: send probe_match via signal

        ch->punch_engine()->send_provider_probe_match(local_port, peer_port, target_port, peer_external_port);

    });

    engine->set_on_success([self, ch](frp_punch_engine::punch_result result) {

        FINFO("p2p success conn={} local_port={} peer_port={} peer={}:{}",

              ch->connection_uuid(), result.local_port, result.peer_port,

              result.peer_endpoint.address().to_string(), result.peer_endpoint.port());

        ch->accept_p2p(std::move(result.socket), result.peer_endpoint);

        ch->set_p2p_active(true);

        ch->handshake_timer().cancel();

    });

    engine->set_on_failed([self, ch] {

        FWARN("p2p failed conn={}", ch->connection_uuid());

    });



    ch->punch_engine() = engine;

    engine->start();



    // P2P handshake timeout: 30 seconds

    ch->handshake_timer().expires_after(std::chrono::seconds(30));

    ch->handshake_timer().async_wait([self, ch](const std::error_code& ec) {

        if (ec || ch->is_closed() || ch->is_p2p_active()) return;

        FWARN("p2p handshake timeout conn={}", ch->connection_uuid());

        if (ch->punch_engine()) { ch->punch_engine()->release(); ch->punch_engine().reset(); }

    });



    // Send accessor_punch_start to exchange endpoints

    frp_client_punch_start_data ps;

    ps.command = frp_client_accessor_punch_start;

    ps.connection_uuid = ch->connection_uuid();

    ps.deadline_us = std::chrono::duration_cast<std::chrono::microseconds>(

        std::chrono::steady_clock::now().time_since_epoch()).count() +

        signal_->server_clock_offset_us() + 5000000;

    signal_->send_p2p_command(ch->peer_uuid(), Fundamental::io::to_json(ps));

}


void frp_accessor::close() {
    resubscribe_timer_.cancel();
    resubscribe_attempts_ = 0;
    // 先搬出再关闭：close() → on_release_ → channels_.erase 会修改容器（规范 §4.3 容器移除先于回调）
    auto channels = std::move(channels_);
    channels_.clear();
    for (auto& [_, ch] : channels) close_data_channel(ch);
    for (auto& [_, lst] : listeners_) {
        std::error_code ec; lst->acceptor.close(ec);
        if (lst->udp_socket) { lst->udp_socket->close(ec); lst->udp_socket.reset(); }
    }
    listeners_.clear();
}

} // namespace network::proxy
