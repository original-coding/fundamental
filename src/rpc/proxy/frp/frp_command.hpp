#pragma once

#include "fundamental/basic/endian_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/rttr_handler/deserializer.h"
#include "fundamental/rttr_handler/serializer.h"

#include <rttr/registration>

namespace network::proxy
{

enum frp_command_type : std::uint8_t
{
    frp_invalid_command = 0,
    frp_signal_open_command,
    frp_server_hello_command,

    frp_auth_request_command,
    frp_auth_response_command,

    frp_p2p_probe_command,
    frp_udp_echo_command,
    // Unified client commands (multi-key support)
    frp_register_services_command,
    frp_register_services_resp_command,
    frp_subscribe_services_command,
    frp_subscribe_services_resp_command,
    // Time sync (NTP-like, over UDP probe channel)
    frp_time_sync_request_command,
    frp_time_sync_response_command,

    frp_channel_open_command,
    frp_forward_command,
    frp_signal_ping_command,   // 信令通道保活探测（客户端 -> server）
    frp_signal_pong_command,   // 信令通道保活响应（server -> 客户端）
};

enum frp_nat_type : std::uint8_t
{
    frp_nat_type_disabled  = 0,
    frp_nat_type_symmetric = 1,
    frp_nat_type_cone      = 2,
};

struct frp_command_base {
    static constexpr std::uint32_t kMaxCommandPayloadLen = 64 * 1024;
    std::uint8_t command                                 = frp_invalid_command;
    virtual ~frp_command_base()                          = default;
    RTTR_ENABLE()
};

enum frp_service_type : std::uint8_t
{
    frp_service_tcp = 0,
    frp_service_udp = 1,
};

struct frp_signal_open_data : frp_command_base {
    RTTR_ENABLE(frp_command_base)
};

struct frp_server_hello_data : frp_command_base {
    std::string server_nonce;
    RTTR_ENABLE(frp_command_base)
};

struct frp_auth_request_data : frp_command_base {
    std::string digest;
    RTTR_ENABLE(frp_command_base)
};

struct frp_auth_response_data : frp_command_base {
    bool ok = false;
    std::string message;
    RTTR_ENABLE(frp_command_base)
};

struct frp_service_registration_data {
    std::string service_name;
    std::uint8_t service_type                = frp_service_tcp;
    bool enable_p2p                          = true;
    virtual ~frp_service_registration_data() = default;
    RTTR_ENABLE()
};

struct frp_visible_service_data {
    std::string service_name;
    std::string provider_uuid;
    std::uint8_t provider_nat_type        = frp_nat_type_disabled;
    std::uint32_t provider_startup_rtt_ms = 100;
    std::uint8_t service_type             = frp_service_tcp;
    bool enable_p2p                       = true;
    virtual ~frp_visible_service_data()   = default;
    RTTR_ENABLE()
};

struct frp_p2p_probe_data : frp_command_base {
    std::uint16_t local_port = 0;
    RTTR_ENABLE(frp_command_base)
};

struct frp_udp_echo_data : frp_command_base {
    std::string external_ip;
    std::uint16_t external_port = 0;
    RTTR_ENABLE(frp_command_base)
};

struct frp_time_sync_request_data : frp_command_base {
    std::uint32_t seq           = 0; // sequence number
    std::int64_t client_send_ts = 0; // client steady_clock::now() (us)
    RTTR_ENABLE(frp_command_base)
};

struct frp_time_sync_response_data : frp_command_base {
    std::uint32_t seq           = 0;
    std::int64_t client_send_ts = 0; // echoed from request
    std::int64_t server_recv_ts = 0; // server steady_clock::now() (us)
    std::int64_t server_send_ts = 0; // server steady_clock::now() (us)
    RTTR_ENABLE(frp_command_base)
};

// ---- unified client data structures ----

struct frp_service_group {
    std::string register_key;
    std::vector<frp_service_registration_data> services;
    virtual ~frp_service_group() = default;
    RTTR_ENABLE()
};

struct frp_register_services_data : frp_command_base {
    std::string uuid;
    std::uint8_t nat_type        = frp_nat_type_disabled;
    std::uint32_t startup_rtt_ms = 100;
    std::vector<frp_service_group> groups;
    RTTR_ENABLE(frp_command_base)
};

struct frp_register_services_resp_data : frp_command_base {
    bool ok = false;
    std::string message;
    RTTR_ENABLE(frp_command_base)
};

struct frp_subscribe_services_data : frp_command_base {
    std::vector<std::string> register_keys;
    RTTR_ENABLE(frp_command_base)
};

struct frp_subscribe_services_resp_data : frp_command_base {
    bool ok = false;
    std::string message;
    std::vector<frp_visible_service_data> services;
    RTTR_ENABLE(frp_command_base)
};

struct frp_forward_command_data : frp_command_base {
    std::string dst_uuid;
    std::string payload;
    RTTR_ENABLE(frp_command_base)
};

struct frp_channel_open_request_data : frp_forward_command_data {
    std::string from_uuid;
    std::string connection_uuid;
    std::int32_t status = 0;
    RTTR_ENABLE(frp_forward_command_data)
};

template <typename CommandData>
inline std::shared_ptr<std::string> packet_frp_command_data(const CommandData& data) {
    auto ret = std::make_shared<std::string>();
    std::string encode_data;
    try {
        encode_data = Fundamental::io::to_json(data);
    } catch (const std::exception& e) {
        FERR("packet_frp_command_data to_json failed: {}", e.what());
        return nullptr;
    }
    ret->resize(4 + encode_data.size());
    std::uint32_t data_size = static_cast<std::uint32_t>(encode_data.size());
    Fundamental::net_buffer_copy(&data_size, ret->data(), 4);
    std::memcpy(ret->data() + 4, encode_data.data(), encode_data.size());
    return ret;
}

inline void __register_frp_reflect_type__() {
    static bool has_registered = false;
    if (has_registered) return;
    has_registered = true;

    rttr::registration::class_<frp_command_base>("network::proxy::frp_command_base")
        .constructor()(rttr::policy::ctor::as_object)
        .property("command", &frp_command_base::command);

    rttr::registration::class_<frp_signal_open_data>("network::proxy::frp_signal_open_data")
        .constructor()(rttr::policy::ctor::as_object);

    rttr::registration::class_<frp_server_hello_data>("network::proxy::frp_server_hello_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("server_nonce", &frp_server_hello_data::server_nonce);

    rttr::registration::class_<frp_auth_request_data>("network::proxy::frp_auth_request_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("digest", &frp_auth_request_data::digest);

    rttr::registration::class_<frp_auth_response_data>("network::proxy::frp_auth_response_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("ok", &frp_auth_response_data::ok)
        .property("message", &frp_auth_response_data::message);

    rttr::registration::class_<frp_service_registration_data>("network::proxy::frp_service_registration_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("service_name", &frp_service_registration_data::service_name)
        .property("service_type", &frp_service_registration_data::service_type)
        .property("enable_p2p", &frp_service_registration_data::enable_p2p);

    rttr::registration::class_<frp_visible_service_data>("network::proxy::frp_visible_service_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("service_name", &frp_visible_service_data::service_name)
        .property("provider_uuid", &frp_visible_service_data::provider_uuid)
        .property("provider_nat_type", &frp_visible_service_data::provider_nat_type)
        .property("provider_startup_rtt_ms", &frp_visible_service_data::provider_startup_rtt_ms)
        .property("service_type", &frp_visible_service_data::service_type)
        .property("enable_p2p", &frp_visible_service_data::enable_p2p);

    rttr::registration::class_<frp_p2p_probe_data>("network::proxy::frp_p2p_probe_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("local_port", &frp_p2p_probe_data::local_port);

    rttr::registration::class_<frp_udp_echo_data>("network::proxy::frp_udp_echo_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("external_ip", &frp_udp_echo_data::external_ip)
        .property("external_port", &frp_udp_echo_data::external_port);

    rttr::registration::class_<frp_forward_command_data>("network::proxy::frp_forward_command_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("payload", &frp_forward_command_data::payload)
        .property("dst_uuid", &frp_forward_command_data::dst_uuid);

    rttr::registration::class_<frp_channel_open_request_data>("network::proxy::frp_channel_open_request_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("connection_uuid", &frp_channel_open_request_data::connection_uuid)
        .property("from_uuid", &frp_channel_open_request_data::from_uuid)
        .property("status", &frp_channel_open_request_data::status);

    rttr::registration::class_<frp_time_sync_request_data>("network::proxy::frp_time_sync_request_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("seq", &frp_time_sync_request_data::seq)
        .property("client_send_ts", &frp_time_sync_request_data::client_send_ts);

    rttr::registration::class_<frp_time_sync_response_data>("network::proxy::frp_time_sync_response_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("seq", &frp_time_sync_response_data::seq)
        .property("client_send_ts", &frp_time_sync_response_data::client_send_ts)
        .property("server_recv_ts", &frp_time_sync_response_data::server_recv_ts)
        .property("server_send_ts", &frp_time_sync_response_data::server_send_ts);

    rttr::registration::class_<frp_service_group>("network::proxy::frp_service_group")
        .constructor()(rttr::policy::ctor::as_object)
        .property("register_key", &frp_service_group::register_key)
        .property("services", &frp_service_group::services);

    rttr::registration::class_<frp_register_services_data>("network::proxy::frp_register_services_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("uuid", &frp_register_services_data::uuid)
        .property("nat_type", &frp_register_services_data::nat_type)
        .property("startup_rtt_ms", &frp_register_services_data::startup_rtt_ms)
        .property("groups", &frp_register_services_data::groups);

    rttr::registration::class_<frp_register_services_resp_data>("network::proxy::frp_register_services_resp_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("ok", &frp_register_services_resp_data::ok)
        .property("message", &frp_register_services_resp_data::message);

    rttr::registration::class_<frp_subscribe_services_data>("network::proxy::frp_subscribe_services_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("register_keys", &frp_subscribe_services_data::register_keys);

    rttr::registration::class_<frp_subscribe_services_resp_data>("network::proxy::frp_subscribe_services_resp_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("ok", &frp_subscribe_services_resp_data::ok)
        .property("message", &frp_subscribe_services_resp_data::message)
        .property("services", &frp_subscribe_services_resp_data::services);
}

} // namespace network::proxy
