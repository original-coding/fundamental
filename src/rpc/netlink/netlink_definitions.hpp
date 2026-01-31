#pragma once
#include <cstdint>
#include <rttr/registration>
#include <string>

namespace network
{
namespace link
{
using LinkNumberType                           = std::uint64_t;
inline constexpr LinkNumberType kMaxLinkNumber = 4096;

enum LinkNetworkType
{
    InSameInstance,
    InSameMachine,
    InSameCluster,
    InExternalAccess
};

struct PeerNetworkInfo {
    std::string host;
    std::string service;
    std::string master_host;
    std::string master_service;
    std::string external_host;
    std::string external_service;
    std::string route_path;
};

struct ClusterPlayer {
    // can't be empty
    std::string master_id;
    // can't be empty
    std::string player_id;
    // can't be empty
    std::string machine_id;
    PeerNetworkInfo network_info;
    LinkNumberType player_no  = 0;
    LinkNetworkType link_type = InSameInstance;
    bool using_localhost() const {
        return link_type == InSameInstance || link_type == InSameMachine;
    }
    bool using_cluster() const {
        return link_type == InSameCluster;
    }
    bool using_external() const {
        return link_type == InExternalAccess;
    }
};

enum LinkCommandEnumType : std::int32_t
{
    InvalidLinkCommandType,
    LinkCommandType,
    LinkCheckCommandType,
};

struct LinkCommandBaseData {
    virtual ~LinkCommandBaseData() {
    }
    std::int32_t type = InvalidLinkCommandType;
    // src player_no
    LinkNumberType player_no = 0;
    RTTR_ENABLE()
};

struct LinkCommandData : LinkCommandBaseData {
    std::string key;
    std::string data;
    RTTR_ENABLE(LinkCommandBaseData)
};

struct LinkCheckCommandData : LinkCommandBaseData {
    constexpr static std::string_view kSuccessCheckRet = "1";
    std::string key;
    RTTR_ENABLE(LinkCommandBaseData)
};

inline void __register_netlink_reflect_type__() {
    static bool has_registered = false;
    if (has_registered) return;
    has_registered = true;
    using rttr::value;
    {
        using RegisterType = LinkNetworkType;
        rttr::registration::enumeration<RegisterType>("network::link::LinkNetworkType")(
            value("instance", RegisterType::InSameInstance), value("machine", RegisterType::InSameMachine),
            value("cluster", RegisterType::InSameCluster), value("external", RegisterType::InExternalAccess));
    }
    {
        using RegisterType = LinkCommandEnumType;
        rttr::registration::enumeration<RegisterType>("network::link::LinkCommandEnumType")(
            value("invalid", RegisterType::InvalidLinkCommandType), value("check", RegisterType::LinkCheckCommandType),
            value("link", RegisterType::LinkCommandType));
    }
    {
        using RegisterType = PeerNetworkInfo;
        rttr::registration::class_<RegisterType>("network::link::PeerNetworkInfo")
            .constructor()(rttr::policy::ctor::as_object)
            .property("host", &RegisterType::host)
            .property("service", &RegisterType::service)
            .property("external_host", &RegisterType::external_host)
            .property("external_service", &RegisterType::external_service)
            .property("master_host", &RegisterType::master_host)
            .property("master_service", &RegisterType::master_service)
            .property("route_path", &RegisterType::route_path);
    }
    {
        using RegisterType = ClusterPlayer;
        rttr::registration::class_<RegisterType>("network::link::ClusterPlayer")
            .constructor()(rttr::policy::ctor::as_object)
            .property("master_id", &RegisterType::master_id)
            .property("machine_id", &RegisterType::machine_id)
            .property("player_id", &RegisterType::player_id)
            .property("network_info", &RegisterType::network_info)
            .property("player_no", &RegisterType::player_no)
            .property("link_type", &RegisterType::link_type);
    }

    {
        using RegisterType = LinkCommandBaseData;
        rttr::registration::class_<RegisterType>("network::link::LinkCommandBaseData")
            .constructor()(rttr::policy::ctor::as_object)
            .property("type", &RegisterType::type)
            .property("player_no", &RegisterType::player_no);
    }
    {
        using RegisterType = LinkCommandData;
        rttr::registration::class_<RegisterType>("network::link::LinkCommandData")
            .constructor()(rttr::policy::ctor::as_object)
            .property("key", &RegisterType::key)
            .property("data", &RegisterType::data);
    }
    {
        using RegisterType = LinkCheckCommandData;
        rttr::registration::class_<RegisterType>("network::link::LinkCheckCommandData")
            .constructor()(rttr::policy::ctor::as_object)
            .property("key", &RegisterType::key);
    }
}
} // namespace link
} // namespace network