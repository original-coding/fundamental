
#pragma once

#include "meta_control.h"
#include <string>
namespace Fundamental {
using json = nlohmann::json;
namespace io {
std::string to_json(const rttr::variant& var, const RttrSerializeOption& option = {});
std::string to_comment_json(const rttr::variant& var, const RttrSerializeOption& option = {});
json to_json_obj(const rttr::variant& var, const RttrSerializeOption& option = {});

template <typename DataType>
inline std::string EnumTypeToString(DataType type) {
    auto e   = rttr::type::get<DataType>().get_enumeration();
    auto ret = e.value_to_name(type).data();
    return ret ? ret : "";
}

template <typename EnumType, typename DataType>
inline std::string EnumTypeToString(DataType type) {
    auto e   = rttr::type::get<EnumType>().get_enumeration();
    auto ret = e.value_to_name(static_cast<EnumType>(type)).data();
    return ret ? ret : "";
}

} // namespace io
} // namespace Fundamental