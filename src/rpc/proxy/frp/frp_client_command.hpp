#pragma once

#include "fundamental/basic/log.h"
#include "fundamental/rttr_handler/deserializer.h"
#include "fundamental/rttr_handler/serializer.h"

#include <rttr/registration>

namespace network::proxy
{

enum frp_client_command_type : std::uint8_t
{
    // relay channel establishment
    frp_client_open   = 100, // accessor -> provider: request service connection
    frp_client_accept = 101, // provider -> accessor: backend ready, accept
    frp_client_reject = 102, // provider -> accessor: backend failed, reject

    // P2P punch (transparent via frp_forward_command)
    frp_client_accessor_punch_start     = 103,
    frp_client_provider_p2p_handshake   = 104,
    frp_client_accessor_handshake_ack   = 105,
    frp_client_accessor_punch_confirm   = 106,
    frp_client_provider_confirm_ack     = 107,
    frp_client_accessor_confirm_ok      = 108,
    frp_client_provider_probe_match     = 109,
};

struct frp_client_command_base {
    std::uint8_t command = frp_client_open;
    virtual ~frp_client_command_base() = default;
    RTTR_ENABLE()
};

struct frp_client_open_data : frp_client_command_base {
    std::string accessor_uuid;
    std::string connection_uuid;
    std::string register_nonce;  // nonce for key verification
    std::string register_hash;   // SHA256(register_key + register_nonce)
    std::string service_name;
    std::uint8_t transport = 0;  // 0=tcp, 1=udp
    RTTR_ENABLE(frp_client_command_base)
};

struct frp_client_accept_data : frp_client_command_base {
    std::string connection_uuid;
    RTTR_ENABLE(frp_client_command_base)
};

struct frp_client_reject_data : frp_client_command_base {
    std::string connection_uuid;
    std::string reason;
    RTTR_ENABLE(frp_client_command_base)
};

// P2P command data structs
struct frp_client_punch_data : frp_client_command_base {
    std::string connection_uuid;
    RTTR_ENABLE(frp_client_command_base)
};

struct frp_client_punch_start_data : frp_client_punch_data {
    std::int64_t deadline_us = 0;
    RTTR_ENABLE(frp_client_punch_data)
};

struct frp_client_p2p_handshake_data : frp_client_punch_data {
    std::string internal_ip;
    std::uint16_t internal_port = 0;
    std::string external_ip;
    std::uint16_t external_port = 0;
    std::uint32_t rtt_ms        = 0;
    std::uint8_t nat_type       = 0;
    std::uint32_t punch_seq     = 0;
    RTTR_ENABLE(frp_client_punch_data)
};

struct frp_client_punch_confirm_data : frp_client_punch_data {
    std::uint16_t local_port          = 0;
    std::uint16_t peer_port           = 0;
    std::uint16_t external_local_port = 0;
    std::uint16_t external_peer_port  = 0;
    RTTR_ENABLE(frp_client_punch_data)
};

inline void __register_frp_client_command_type__() {
    static bool has_registered = false;
    if (has_registered) return;
    has_registered = true;

    rttr::registration::class_<frp_client_command_base>("network::proxy::frp_client_command_base")
        .constructor()(rttr::policy::ctor::as_object)
        .property("command", &frp_client_command_base::command);

    rttr::registration::class_<frp_client_open_data>("network::proxy::frp_client_open_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("command", &frp_client_open_data::command)
        .property("accessor_uuid", &frp_client_open_data::accessor_uuid)
        .property("connection_uuid", &frp_client_open_data::connection_uuid)
        .property("register_nonce", &frp_client_open_data::register_nonce)
        .property("register_hash", &frp_client_open_data::register_hash)
        .property("service_name", &frp_client_open_data::service_name)
        .property("transport", &frp_client_open_data::transport);

    rttr::registration::class_<frp_client_accept_data>("network::proxy::frp_client_accept_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("command", &frp_client_accept_data::command)
        .property("connection_uuid", &frp_client_accept_data::connection_uuid);

    rttr::registration::class_<frp_client_reject_data>("network::proxy::frp_client_reject_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("command", &frp_client_reject_data::command)
        .property("connection_uuid", &frp_client_reject_data::connection_uuid)
        .property("reason", &frp_client_reject_data::reason);

    rttr::registration::class_<frp_client_punch_data>("network::proxy::frp_client_punch_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("command", &frp_client_punch_data::command)
        .property("connection_uuid", &frp_client_punch_data::connection_uuid);

    rttr::registration::class_<frp_client_punch_start_data>("network::proxy::frp_client_punch_start_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("deadline_us", &frp_client_punch_start_data::deadline_us);

    rttr::registration::class_<frp_client_p2p_handshake_data>("network::proxy::frp_client_p2p_handshake_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("internal_ip", &frp_client_p2p_handshake_data::internal_ip)
        .property("internal_port", &frp_client_p2p_handshake_data::internal_port)
        .property("external_ip", &frp_client_p2p_handshake_data::external_ip)
        .property("external_port", &frp_client_p2p_handshake_data::external_port)
        .property("rtt_ms", &frp_client_p2p_handshake_data::rtt_ms)
        .property("nat_type", &frp_client_p2p_handshake_data::nat_type)
        .property("punch_seq", &frp_client_p2p_handshake_data::punch_seq);

    rttr::registration::class_<frp_client_punch_confirm_data>("network::proxy::frp_client_punch_confirm_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("local_port", &frp_client_punch_confirm_data::local_port)
        .property("peer_port", &frp_client_punch_confirm_data::peer_port)
        .property("external_local_port", &frp_client_punch_confirm_data::external_local_port)
        .property("external_peer_port", &frp_client_punch_confirm_data::external_peer_port);
}

} // namespace network::proxy
