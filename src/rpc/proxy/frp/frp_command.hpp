#pragma once

#include "fundamental/rttr_handler/binary_packer.h"
#include "fundamental/rttr_handler/deserializer.h"
#include "fundamental/rttr_handler/serializer.h"
#include <rttr/registration>

namespace network
{
namespace proxy
{

enum frp_command_type : std::uint8_t
{
    frp_invalid_command = 0,
    // client to server
    frp_setup_request_command = 1,
    // server to client
    frp_setup_response_command,
    // server to client
    frp_accept_notify_command
};

struct frp_command_base {
    static constexpr std::uint32_t kMaxCommandPayloadLen = 4096;
    std::uint8_t command                                 = frp_invalid_command;
    virtual ~frp_command_base() {
    }
    RTTR_ENABLE()
};

struct frp_setup_request_data : frp_command_base {
    std::string frp_id;
    std::uint16_t proxy_port = 0;
    RTTR_ENABLE(frp_command_base)
};

struct frp_setup_response_data : frp_command_base {
    std::string frp_id;
    std::uint16_t proxy_port = 0;
    RTTR_ENABLE(frp_command_base)
};

struct frp_accept_notify_data : frp_command_base {
    RTTR_ENABLE(frp_command_base)
};

template <typename CommandData>
inline std::shared_ptr<std::string> packet_frp_command_data(const CommandData& data) {
    std::shared_ptr<std::string> ret = std::make_shared<std::string>();
    auto encode_data                 = Fundamental::io::to_json(data);
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
    using rttr::value;
    {
        using RegisterType = frp_command_base;
        rttr::registration::class_<RegisterType>("network::proxy::frp_command_base")
            .constructor()(rttr::policy::ctor::as_object)
            .property("command", &RegisterType::command);
    }
    {
        using RegisterType = frp_setup_request_data;
        rttr::registration::class_<RegisterType>("network::proxy::frp_setup_request_data")
            .constructor()(rttr::policy::ctor::as_object)
            .property("frp_id", &RegisterType::frp_id)
            .property("proxy_port", &RegisterType::proxy_port);
    }
    {
        using RegisterType = frp_setup_response_data;
        rttr::registration::class_<RegisterType>("network::proxy::frp_setup_response_data")
            .constructor()(rttr::policy::ctor::as_object)
            .property("frp_id", &RegisterType::frp_id)
            .property("proxy_port", &RegisterType::proxy_port);
    }
    {
        using RegisterType = frp_accept_notify_data;
        rttr::registration::class_<RegisterType>("network::proxy::frp_accept_notify_data")
            .constructor()(rttr::policy::ctor::as_object);
    }
}
} // namespace proxy
} // namespace network