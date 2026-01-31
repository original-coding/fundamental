
#pragma once

#include "meta_control.h"
#include <string>
#include <vector>
namespace Fundamental
{
using json = nlohmann::json;
namespace io
{
std::string to_json(const rttr::variant& var, const RttrSerializeOption& option = {});
std::string to_comment_json(const rttr::variant& var, const RttrSerializeOption& option = {});
json to_json_obj(const rttr::variant& var, const RttrSerializeOption& option = {});

template <typename DataType>
inline std::string EnumTypeToString(DataType type) {
    std::string ret;
    do {
        using raw_type_t = std::decay_t<DataType>;
        if constexpr (std::is_enum_v<raw_type_t>) {
            auto e = rttr::type::get<DataType>().get_enumeration();
            ret    = e.value_to_name(type).to_string();
            if (!ret.empty()) break;
        }
        rttr::variant v = type;
        if (v.can_convert<std::string>()) {
            ret = v.get_value<std::string>();
        } else {
            ret = std::to_string(type);
        }
    } while (0);
    return ret;
}

template <typename EnumType>
inline std::vector<std::string> GetEnumTypeOptionalValuesVec() {
    std::vector<std::string> ret;
    do {
        rttr::type t = rttr::type::template get<EnumType>();
        if (!t.is_valid() || !t.is_enumeration()) break;
        auto enum_value = t.get_enumeration();
        auto all_values = enum_value.get_names();
        for (const auto& value_name : all_values) {
            ret.emplace_back(value_name.to_string());
        }

    } while (0);
    return ret;
}

template <typename EnumType>
inline std::string GetEnumTypeOptionalValuesJsonString() {
    std::vector<std::string> vec = GetEnumTypeOptionalValuesVec<EnumType>();
    return to_json_obj(vec).dump(-1);
}
} // namespace io
} // namespace Fundamental