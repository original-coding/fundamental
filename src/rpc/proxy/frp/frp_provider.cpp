#include "frp_provider.hpp"
#include "frp_client.hpp"   // for relay_data_channel, frp_tcp_channel
#include "frp_signal_client.hpp"
#include "frp_common.hpp"
#include "frp_kcp_crypto.hpp"
#include "frp_punch_engine.hpp"
#include "fundamental/basic/log.h"

namespace {
std::string sha256_hex(std::string_view input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
    return Fundamental::Utils::BufferToHex(hash, SHA256_DIGEST_LENGTH);
}
} // namespace


namespace network::proxy
{

frp_provider::frp_provider(std::shared_ptr<frp_signal_client> signal) : signal_(std::move(signal)) {}

void frp_provider::set_service_map(std::unordered_map<std::string,
    std::unordered_map<std::string, frp_provider_service_config>> m) { services_by_key_ = std::move(m); }

void frp_provider::register_all_services() {

    frp_register_services_data req;

    req.command = frp_register_services_command;

    req.uuid = signal_->uuid();

    req.nat_type = (signal_->config().nat_type != frp_nat_type_disabled && signal_->probed_nat_type() != frp_nat_type_disabled)

                       ? signal_->probed_nat_type() : frp_nat_type_disabled;

    req.startup_rtt_ms = signal_->startup_rtt_ms();

    for (const auto& group : signal_->config().groups) {

        if (group.services.empty()) continue;

        frp_service_group g;

        g.register_key = group.register_key;

        for (const auto& svc : group.services) {

            frp_service_registration_data d;

            d.service_name = svc.service_name; d.service_type = svc.service_type; d.enable_p2p = svc.enable_p2p;

            g.services.push_back(std::move(d));

        }

        req.groups.push_back(std::move(g));

    }

    signal_->send_command(req);

}


void frp_provider::handle_client_open(const frp_client_open_data& data) {

    // Find register_key by matching hash

    std::string matched_key;

    for (const auto& [key, svcs] : services_by_key_) {

        if (sha256_hex(key + data.register_nonce) == data.register_hash) {

            matched_key = key;

            break;

        }

    }

    if (matched_key.empty()) {

        FWARN("register_key not matched");

        return;

    }

    auto kit = services_by_key_.find(matched_key);

    auto sit = kit->second.find(data.service_name);

    if (sit == kit->second.end()) {

        FWARN("unknown service={}", data.service_name);

        return;

    }

    auto ch = relay_data_channel::create(signal_->get_executor(),

                                         data.connection_uuid, data.accessor_uuid);

    ch->set_register_key(matched_key);

    ch->set_service_name(data.service_name);

    ch->set_transport(data.transport);

    ch->set_on_release([this, cid = data.connection_uuid]() { channels_.erase(cid); });

    ch->set_idle_timeout_seconds(signal_->config().data_channel_idle_timeout_seconds);

    channels_[data.connection_uuid] = ch;

    start_backend_connect(ch);

}


void frp_provider::start_backend_connect(const std::shared_ptr<relay_data_channel>& ch) {

    FINFO("start_backend_connect svc={} transport={}", ch->service_name(), static_cast<int>(ch->transport()));

    auto kit = services_by_key_.find(ch->register_key());

    if (kit == services_by_key_.end()) { FERR("register_key not found: {}", ch->register_key()); close_data_channel(ch); return; }

    auto it = kit->second.find(ch->service_name());

    if (it == kit->second.end()) { FERR("service not found: {}", ch->service_name()); close_data_channel(ch); return; }

    const auto& svc = it->second;

    auto self = shared_from_this();



    if (ch->transport() == frp_service_udp) {

        std::error_code resolve_ec;

        auto addr = asio::ip::make_address(svc.target_host, resolve_ec);

        asio::ip::udp::endpoint ep;

        if (!resolve_ec && !addr.is_v6()) {

            ep = asio::ip::udp::endpoint(addr, svc.target_port);

        } else {

            asio::ip::udp::resolver resolver(signal_->get_executor());

            auto eps = resolver.resolve(asio::ip::udp::v4(), svc.target_host,

                                        std::to_string(svc.target_port), resolve_ec);

            if (!resolve_ec && eps.begin() != eps.end()) ep = *eps.begin();

        }

        if (resolve_ec) {

            FWARN("udp resolve failed: {} err={}", ch->service_name(), resolve_ec.message());

            setup_data_channel(ch); return;

        }

        ch->backend_udp_socket() = std::make_unique<asio::ip::udp::socket>(signal_->get_executor());

        std::error_code oec;

        ch->backend_udp_socket()->open(asio::ip::udp::v4(), oec);

        if (!oec) ch->backend_udp_socket()->bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0), oec);

        if (oec) { FWARN("udp bind failed: {}", ch->service_name()); setup_data_channel(ch); return; }

        ch->backend_udp_target() = ep;

        ch->backend_connected() = true;

        FINFO("backend udp ready: {} conn={}", ch->service_name(), ch->connection_uuid());

        setup_data_channel(ch);

        start_backend_read_loop(ch);

        return;

    }



    auto resolver = std::make_shared<asio::ip::tcp::resolver>(signal_->get_executor());

    resolver->async_resolve(svc.target_host, std::to_string(svc.target_port),

        [this, self, ch, resolver](const std::error_code& ec, const asio::ip::tcp::resolver::results_type& eps) {

            if (!signal_->is_reference_valid() || ch->is_closed()) return;

            if (ec || eps.empty()) { FWARN("backend resolve failed: {}", ec.message()); setup_data_channel(ch); return; }

            asio::async_connect(ch->backend_socket(), eps,

                [this, self, ch](const std::error_code& ec2, const asio::ip::tcp::endpoint&) {

                    if (!signal_->is_reference_valid() || ch->is_closed()) return;

                    if (ec2) { FWARN("backend connect failed: {}", ec2.message()); setup_data_channel(ch); return; }

                    ch->backend_connected() = true;

                    FINFO("backend connected: {} conn={}", ch->service_name(), ch->connection_uuid());

                    setup_data_channel(ch);

                    start_backend_read_loop(ch);

                });

        });

}


void frp_provider::start_backend_read_loop(const std::shared_ptr<relay_data_channel>& ch) {

    if (!ch || !ch->backend_connected() || ch->is_closed()) return;

    auto self = shared_from_this();

    if (ch->transport() == frp_service_udp && ch->backend_udp_socket()) {

        ch->backend_udp_socket()->async_receive_from(

            asio::buffer(ch->backend_read_buf().data(), ch->backend_read_buf().size()), ch->backend_udp_target(),

            [this, self, ch](const std::error_code& ec, std::size_t n) {

                if (!signal_->is_reference_valid() || ch->is_closed()) return;

                if (ec) { close_data_channel(ch); return; } // 3.8: 错误收敛到关闭序列，禁止无限重挂自旋

                if (n > 0) {
                    if (ch->has_kcp()) {
                        ch->send_bytes(ch->backend_read_buf().data(), n);
                    } else {
                        // KCP 未就绪：缓冲，init_kcp 后统一补发（禁止裸写进 KCP 流）
                        ch->pending_writes().emplace_back(
                            std::make_shared<std::string>(ch->backend_read_buf().data(), n));
                    }
                }

                if (!ch->is_closed()) start_backend_read_loop(ch);

            });

    } else {

        ch->backend_socket().async_read_some(

            asio::buffer(ch->backend_read_buf().data(), ch->backend_read_buf().size()),

            [this, self, ch](const std::error_code& ec, std::size_t n) {

                if (!signal_->is_reference_valid() || ch->is_closed()) return;

                if (ec) { close_data_channel(ch); return; } // 3.8: 错误收敛到关闭序列，禁止无限重挂自旋

                if (n > 0) {
                    if (ch->has_kcp()) {
                        ch->send_bytes(ch->backend_read_buf().data(), n);
                    } else {
                        // KCP 未就绪：缓冲，init_kcp 后统一补发（禁止裸写进 KCP 流）
                        ch->pending_writes().emplace_back(
                            std::make_shared<std::string>(ch->backend_read_buf().data(), n));
                    }
                }

                if (!ch->is_closed()) start_backend_read_loop(ch);

            });

    }

}


void frp_provider::setup_data_channel(const std::shared_ptr<relay_data_channel>& ch) {

    // Build accept/reject for signal channel delivery

    std::string client_payload;

    if (ch->backend_connected()) {

        frp_client_accept_data a; a.command = frp_client_accept;

        a.connection_uuid = ch->connection_uuid();

        client_payload = Fundamental::io::to_json(a);

    } else {

        frp_client_reject_data r; r.command = frp_client_reject;

        r.connection_uuid = ch->connection_uuid();

        r.reason = "backend failed";

        client_payload = Fundamental::io::to_json(r);

    }



    // data channel: payload carries accept/reject, server routes it to accessor's signal channel

    frp_channel_open_request_data req;

    req.command = frp_channel_open_command;

    req.from_uuid = signal_->uuid();

    req.dst_uuid = ch->peer_uuid();

    req.connection_uuid = ch->connection_uuid();

    req.status = 1;

    req.payload = std::move(client_payload);

    auto packet = packet_frp_command_data(req);

    if (!packet) { close_data_channel(ch); return; }



    auto self = shared_from_this();

    auto tcp = frp_tcp_channel::make_shared(

        signal_->get_executor(),

        signal_->config().public_server_host, std::to_string(signal_->config().public_server_tcp_port));

    tcp->enable_ssl(to_network_config(signal_->config().ssl));

    tcp->notify_connect_result.Connect(shared_from_this(),

        [this, self, ch, packet = std::move(packet)]

        (Fundamental::error_code ec, std::shared_ptr<frp_tcp_channel> t) mutable {

            if (!signal_->is_reference_valid() || ch->is_closed()) return;

            if (ec || !t) { close_data_channel(ch); return; }

            ch->attach_tcp(std::move(t));

            ch->tcp()->async_write_framed(packet);

            if (!ch->backend_connected()) { close_data_channel(ch); return; }

            ch->set_traffic_secret(signal_->config().traffic_secret);

            ch->init_kcp();

            // KCP 就绪前读到的后端数据（如 SSH banner 建连即发）已缓冲到 pending_writes，

            // 在这里经 KCP 补发——裸写会污染 KCP 流，直接丢弃会让对端永远等不到数据。

            for (auto& pw : ch->pending_writes()) ch->send_bytes(pw->data(), pw->size());

            ch->pending_writes().clear();

            start_data_forward_read_loop(ch);

        });

    tcp->start_async_connect();

}


void frp_provider::start_data_forward_read_loop(const std::shared_ptr<relay_data_channel>& ch) {

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


void frp_provider::close_data_channel(const std::shared_ptr<relay_data_channel>& ch) {

    if (!ch || ch->is_closed()) return;

    FINFO("close_data_channel conn={}", ch->connection_uuid());

    ch->close();

}


void frp_provider::on_client_command(std::uint8_t cmd, std::string payload) {
    frp_client_command_base ccmd;
    if (!Fundamental::io::from_json(payload, ccmd)) return;
    switch (ccmd.command) {
    case frp_client_open: {
        frp_client_open_data od;
        if (!Fundamental::io::from_json(payload, od)) break;
        FINFO("recv open from={} svc={} conn={}", od.accessor_uuid, od.service_name, od.connection_uuid);
        handle_client_open(od);
        break;
    }
    case frp_client_accessor_punch_start:
    case frp_client_accessor_handshake_ack:
    case frp_client_accessor_punch_confirm:
    case frp_client_accessor_confirm_ok:
        handle_p2p_message(ccmd.command, std::move(payload));
        break;
    default: break;
    }
}

void frp_provider::handle_p2p_message(std::uint8_t cmd, std::string payload) {

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

            engine->set_on_probe_match([this, self, ch](std::uint16_t local_port, std::uint16_t peer_port,

                                                          std::uint16_t target_port, std::uint16_t peer_external_port) {

                FINFO("probe match conn={} local={} peer={} tgt={} ext_peer={}",

                      ch->connection_uuid(), local_port, peer_port, target_port, peer_external_port);

                ch->punch_engine()->send_provider_probe_match(local_port, peer_port, target_port, peer_external_port);

            });

            engine->set_on_success([this, self, ch](frp_punch_engine::punch_result result) {

                FINFO("p2p success conn={} local_port={} peer_port={}",

                      ch->connection_uuid(), result.local_port, result.peer_port);

                ch->accept_p2p(std::move(result.socket), result.peer_endpoint);

                ch->set_p2p_active(true);

                ch->handshake_timer().cancel();

            });

            engine->set_on_failed([this, self, ch] {

                FWARN("p2p failed conn={}", ch->connection_uuid());

            });



            ch->punch_engine() = engine;

            engine->start();



            // P2P handshake timeout

            ch->handshake_timer().expires_after(std::chrono::seconds(30));

            ch->handshake_timer().async_wait([this, self, ch](const std::error_code& ec) {

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


void frp_provider::maybe_start_p2p(const std::shared_ptr<relay_data_channel>& ch) {

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

    engine->set_on_endpoint_ready([this, self, ch](std::string ip, std::uint16_t port) {

        ch->my_external_ip() = ip;

        ch->my_external_port() = port;

        FINFO("punch endpoint ready conn={} external={}:{}", ch->connection_uuid(), ip, port);

    });

    engine->set_on_probe_match([this, self, ch](std::uint16_t local_port, std::uint16_t peer_port,

                                                  std::uint16_t target_port, std::uint16_t peer_external_port) {

        FINFO("probe match conn={} local={} peer={} tgt={} ext_peer={}",

              ch->connection_uuid(), local_port, peer_port, target_port, peer_external_port);

        // Provider side: send probe_match via signal

        ch->punch_engine()->send_provider_probe_match(local_port, peer_port, target_port, peer_external_port);

    });

    engine->set_on_success([this, self, ch](frp_punch_engine::punch_result result) {

        FINFO("p2p success conn={} local_port={} peer_port={} peer={}:{}",

              ch->connection_uuid(), result.local_port, result.peer_port,

              result.peer_endpoint.address().to_string(), result.peer_endpoint.port());

        ch->accept_p2p(std::move(result.socket), result.peer_endpoint);

        ch->set_p2p_active(true);

        ch->handshake_timer().cancel();

    });

    engine->set_on_failed([this, self, ch] {

        FWARN("p2p failed conn={}", ch->connection_uuid());

    });



    ch->punch_engine() = engine;

    engine->start();



    // P2P handshake timeout: 30 seconds

    ch->handshake_timer().expires_after(std::chrono::seconds(30));

    ch->handshake_timer().async_wait([this, self, ch](const std::error_code& ec) {

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


void frp_provider::close() {
    // 先搬出再关闭：close() → on_release_ → channels_.erase 会修改容器（规范 §4.3 容器移除先于回调）
    auto channels = std::move(channels_);
    channels_.clear();
    for (auto& [_, ch] : channels) close_data_channel(ch);
}

} // namespace network::proxy
