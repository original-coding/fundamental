#pragma once

#include "fundamental/basic/endian_utils.hpp"
#include "fundamental/rttr_handler/deserializer.h"
#include "fundamental/rttr_handler/serializer.h"

#include <rttr/registration>

namespace network::proxy
{

enum frp_runtime_command_type : std::uint8_t
{
    frp_runtime_invalid_command = 0,
    frp_runtime_signal_open_command,
    frp_runtime_data_open_command,
    frp_runtime_server_hello_command,
    frp_runtime_auth_request_command,
    frp_runtime_auth_response_command,
    frp_runtime_join_request_command,
    frp_runtime_join_response_command,
    frp_runtime_register_services_request_command,
    frp_runtime_register_services_response_command,
    frp_runtime_fetch_services_request_command,
    frp_runtime_fetch_services_response_command,
    frp_runtime_create_flow_request_command,
    frp_runtime_create_flow_response_command,
    frp_runtime_prepare_flow_command,
    frp_runtime_p2p_probe_command,
    frp_runtime_flow_p2p_peer_command,
    frp_runtime_flow_endpoint_ready_command,
    frp_runtime_flow_transport_ready_command,
    frp_runtime_p2p_upgrade_request_command,
    frp_runtime_flow_ready_command,
    frp_runtime_flow_failed_command,
    frp_runtime_flow_data_command,
    frp_runtime_flow_closed_command,
    frp_runtime_ping_request_command,
    frp_runtime_ping_response_command
};

enum frp_runtime_role_type : std::uint8_t
{
    frp_runtime_invalid_role = 0,
    frp_runtime_provider_role,
    frp_runtime_accessor_role
};

enum frp_runtime_nat_type : std::uint8_t
{
    frp_runtime_nat_type_disabled  = 0,
    frp_runtime_nat_type_symmetric = 1,
    frp_runtime_nat_type_full      = 2,
};

struct frp_runtime_command_base {
    static constexpr std::uint32_t kMaxCommandPayloadLen = 64 * 1024;
    std::uint8_t command                                 = frp_runtime_invalid_command;
    virtual ~frp_runtime_command_base()                 = default;
    RTTR_ENABLE()
};

enum frp_runtime_flow_result_type : std::uint8_t
{
    frp_runtime_flow_result_invalid = 0,
    frp_runtime_flow_result_accepted,
    frp_runtime_flow_result_p2p_unavailable,
    frp_runtime_flow_result_rejected
};

enum frp_runtime_transport_type : std::uint8_t
{
    frp_runtime_transport_invalid = 0,
    frp_runtime_transport_tcp_relay
};

enum frp_runtime_flow_failed_reason_type : std::uint8_t
{
    frp_runtime_flow_failed_invalid = 0,
    frp_runtime_flow_failed_relay_channel_open_failed,
    frp_runtime_flow_failed_flow_endpoint_probe_timeout,
    frp_runtime_flow_failed_punch_timeout,
    frp_runtime_flow_failed_backend_connect_failed
};

struct frp_runtime_signal_open_data : frp_runtime_command_base {
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_data_open_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    std::string uuid;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_server_hello_data : frp_runtime_command_base {
    std::string server_nonce;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_auth_request_data : frp_runtime_command_base {
    std::string digest;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_auth_response_data : frp_runtime_command_base {
    bool ok = false;
    std::string message;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_join_request_data : frp_runtime_command_base {
    std::uint8_t role     = frp_runtime_invalid_role;
    std::string uuid;
    std::string register_key;
    std::uint8_t nat_type = frp_runtime_nat_type_disabled;
    std::uint32_t startup_rtt_ms = 0;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_join_response_data : frp_runtime_command_base {
    bool ok = false;
    std::string message;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_service_registration_data {
    std::string service_name;
    virtual ~frp_runtime_service_registration_data() = default;
    RTTR_ENABLE()
};

struct frp_runtime_register_services_request_data : frp_runtime_command_base {
    std::vector<frp_runtime_service_registration_data> services;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_register_services_response_data : frp_runtime_command_base {
    bool ok = false;
    std::string message;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_fetch_services_request_data : frp_runtime_command_base {
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_visible_service_data {
    std::string service_name;
    std::string provider_uuid;
    std::uint8_t provider_nat_type = frp_runtime_nat_type_disabled;
    std::uint32_t provider_startup_rtt_ms = 100;
    virtual ~frp_runtime_visible_service_data() = default;
    RTTR_ENABLE()
};

struct frp_runtime_fetch_services_response_data : frp_runtime_command_base {
    bool ok = false;
    std::string message;
    std::vector<frp_runtime_visible_service_data> services;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_create_flow_request_data : frp_runtime_command_base {
    std::string service_name;
    std::uint8_t transport = frp_runtime_transport_invalid;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_create_flow_response_data : frp_runtime_command_base {
    std::uint8_t result = frp_runtime_flow_result_invalid;
    std::uint32_t flow_id = 0;
    std::string provider_uuid;
    std::string message;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_prepare_flow_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    std::string accessor_uuid;
    std::string service_name;
    std::uint8_t transport = frp_runtime_transport_invalid;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_p2p_probe_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    std::string uuid;
    std::string local_ip;
    std::uint16_t local_port = 0;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_flow_p2p_peer_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    std::string peer_host;
    std::uint16_t peer_port = 0;
    bool use_local_candidate = false;
    std::uint8_t peer_nat_type = frp_runtime_nat_type_disabled;
    std::uint32_t peer_startup_rtt_ms = 0;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_flow_endpoint_ready_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    std::string external_ip;
    std::uint16_t external_port = 0;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_p2p_upgrade_request_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_flow_transport_ready_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_flow_ready_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_flow_failed_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    std::uint8_t reason = frp_runtime_flow_failed_invalid;
    std::string message;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_flow_data_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    std::string payload_base64;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_flow_closed_data : frp_runtime_command_base {
    std::uint32_t flow_id = 0;
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_ping_request_data : frp_runtime_command_base {
    RTTR_ENABLE(frp_runtime_command_base)
};

struct frp_runtime_ping_response_data : frp_runtime_command_base {
    RTTR_ENABLE(frp_runtime_command_base)
};

template <typename CommandData>
inline std::shared_ptr<std::string> packet_frp_runtime_command_data(const CommandData& data) {
    auto ret         = std::make_shared<std::string>();
    auto encode_data = Fundamental::io::to_json(data);
    ret->resize(4 + encode_data.size());
    std::uint32_t data_size = static_cast<std::uint32_t>(encode_data.size());
    Fundamental::net_buffer_copy(&data_size, ret->data(), 4);
    std::memcpy(ret->data() + 4, encode_data.data(), encode_data.size());
    return ret;
}

inline void __register_frp_runtime_reflect_type__() {
    static bool has_registered = false;
    if (has_registered) return;
    has_registered = true;

    rttr::registration::class_<frp_runtime_command_base>("network::proxy::frp_runtime_command_base")
        .constructor()(rttr::policy::ctor::as_object)
        .property("command", &frp_runtime_command_base::command);

    rttr::registration::class_<frp_runtime_signal_open_data>("network::proxy::frp_runtime_signal_open_data")
        .constructor()(rttr::policy::ctor::as_object);

    rttr::registration::class_<frp_runtime_data_open_data>("network::proxy::frp_runtime_data_open_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_data_open_data::flow_id)
        .property("uuid", &frp_runtime_data_open_data::uuid);

    rttr::registration::class_<frp_runtime_server_hello_data>("network::proxy::frp_runtime_server_hello_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("server_nonce", &frp_runtime_server_hello_data::server_nonce);

    rttr::registration::class_<frp_runtime_auth_request_data>("network::proxy::frp_runtime_auth_request_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("digest", &frp_runtime_auth_request_data::digest);

    rttr::registration::class_<frp_runtime_auth_response_data>("network::proxy::frp_runtime_auth_response_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("ok", &frp_runtime_auth_response_data::ok)
        .property("message", &frp_runtime_auth_response_data::message);

    rttr::registration::class_<frp_runtime_join_request_data>("network::proxy::frp_runtime_join_request_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("role", &frp_runtime_join_request_data::role)
        .property("uuid", &frp_runtime_join_request_data::uuid)
        .property("register_key", &frp_runtime_join_request_data::register_key)
        .property("nat_type", &frp_runtime_join_request_data::nat_type)
        .property("startup_rtt_ms", &frp_runtime_join_request_data::startup_rtt_ms);

    rttr::registration::class_<frp_runtime_join_response_data>("network::proxy::frp_runtime_join_response_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("ok", &frp_runtime_join_response_data::ok)
        .property("message", &frp_runtime_join_response_data::message);

    rttr::registration::class_<frp_runtime_service_registration_data>(
        "network::proxy::frp_runtime_service_registration_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("service_name", &frp_runtime_service_registration_data::service_name);

    rttr::registration::class_<frp_runtime_register_services_request_data>(
        "network::proxy::frp_runtime_register_services_request_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("services", &frp_runtime_register_services_request_data::services);

    rttr::registration::class_<frp_runtime_register_services_response_data>(
        "network::proxy::frp_runtime_register_services_response_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("ok", &frp_runtime_register_services_response_data::ok)
        .property("message", &frp_runtime_register_services_response_data::message);

    rttr::registration::class_<frp_runtime_fetch_services_request_data>(
        "network::proxy::frp_runtime_fetch_services_request_data")
        .constructor()(rttr::policy::ctor::as_object);

    rttr::registration::class_<frp_runtime_visible_service_data>("network::proxy::frp_runtime_visible_service_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("service_name", &frp_runtime_visible_service_data::service_name)
        .property("provider_uuid", &frp_runtime_visible_service_data::provider_uuid)
        .property("provider_nat_type", &frp_runtime_visible_service_data::provider_nat_type)
        .property("provider_startup_rtt_ms", &frp_runtime_visible_service_data::provider_startup_rtt_ms);

    rttr::registration::class_<frp_runtime_fetch_services_response_data>(
        "network::proxy::frp_runtime_fetch_services_response_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("ok", &frp_runtime_fetch_services_response_data::ok)
        .property("message", &frp_runtime_fetch_services_response_data::message)
        .property("services", &frp_runtime_fetch_services_response_data::services);

    rttr::registration::class_<frp_runtime_create_flow_request_data>(
        "network::proxy::frp_runtime_create_flow_request_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("service_name", &frp_runtime_create_flow_request_data::service_name)
        .property("transport", &frp_runtime_create_flow_request_data::transport);

    rttr::registration::class_<frp_runtime_create_flow_response_data>(
        "network::proxy::frp_runtime_create_flow_response_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("result", &frp_runtime_create_flow_response_data::result)
        .property("flow_id", &frp_runtime_create_flow_response_data::flow_id)
        .property("provider_uuid", &frp_runtime_create_flow_response_data::provider_uuid)
        .property("message", &frp_runtime_create_flow_response_data::message);

    rttr::registration::class_<frp_runtime_prepare_flow_data>("network::proxy::frp_runtime_prepare_flow_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_prepare_flow_data::flow_id)
        .property("accessor_uuid", &frp_runtime_prepare_flow_data::accessor_uuid)
        .property("service_name", &frp_runtime_prepare_flow_data::service_name)
        .property("transport", &frp_runtime_prepare_flow_data::transport);

    rttr::registration::class_<frp_runtime_p2p_probe_data>("network::proxy::frp_runtime_p2p_probe_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_p2p_probe_data::flow_id)
        .property("uuid", &frp_runtime_p2p_probe_data::uuid)
        .property("local_ip", &frp_runtime_p2p_probe_data::local_ip)
        .property("local_port", &frp_runtime_p2p_probe_data::local_port);

    rttr::registration::class_<frp_runtime_flow_p2p_peer_data>("network::proxy::frp_runtime_flow_p2p_peer_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_flow_p2p_peer_data::flow_id)
        .property("peer_host", &frp_runtime_flow_p2p_peer_data::peer_host)
        .property("peer_port", &frp_runtime_flow_p2p_peer_data::peer_port)
        .property("use_local_candidate", &frp_runtime_flow_p2p_peer_data::use_local_candidate)
        .property("peer_nat_type", &frp_runtime_flow_p2p_peer_data::peer_nat_type)
        .property("peer_startup_rtt_ms", &frp_runtime_flow_p2p_peer_data::peer_startup_rtt_ms);

    rttr::registration::class_<frp_runtime_flow_endpoint_ready_data>(
        "network::proxy::frp_runtime_flow_endpoint_ready_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_flow_endpoint_ready_data::flow_id)
        .property("external_ip", &frp_runtime_flow_endpoint_ready_data::external_ip)
        .property("external_port", &frp_runtime_flow_endpoint_ready_data::external_port);

    rttr::registration::class_<frp_runtime_flow_transport_ready_data>(
        "network::proxy::frp_runtime_flow_transport_ready_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_flow_transport_ready_data::flow_id);

    rttr::registration::class_<frp_runtime_p2p_upgrade_request_data>(
        "network::proxy::frp_runtime_p2p_upgrade_request_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_p2p_upgrade_request_data::flow_id);

    rttr::registration::class_<frp_runtime_flow_ready_data>("network::proxy::frp_runtime_flow_ready_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_flow_ready_data::flow_id);

    rttr::registration::class_<frp_runtime_flow_failed_data>("network::proxy::frp_runtime_flow_failed_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_flow_failed_data::flow_id)
        .property("reason", &frp_runtime_flow_failed_data::reason)
        .property("message", &frp_runtime_flow_failed_data::message);

    rttr::registration::class_<frp_runtime_flow_data_data>("network::proxy::frp_runtime_flow_data_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_flow_data_data::flow_id)
        .property("payload_base64", &frp_runtime_flow_data_data::payload_base64);

    rttr::registration::class_<frp_runtime_flow_closed_data>("network::proxy::frp_runtime_flow_closed_data")
        .constructor()(rttr::policy::ctor::as_object)
        .property("flow_id", &frp_runtime_flow_closed_data::flow_id);

    rttr::registration::class_<frp_runtime_ping_request_data>("network::proxy::frp_runtime_ping_request_data")
        .constructor()(rttr::policy::ctor::as_object);

    rttr::registration::class_<frp_runtime_ping_response_data>("network::proxy::frp_runtime_ping_response_data")
        .constructor()(rttr::policy::ctor::as_object);
}

} // namespace network::proxy
