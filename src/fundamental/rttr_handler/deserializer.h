
#pragma once
#include "meta_control.h"
#include <string>
namespace Fundamental
{
using json = nlohmann::json;
namespace io
{
namespace internal
{
void fromjson_recursively(const json& json_object,
                          rttr::variant& var,
                          rttr::instance obj,
                          const RttrDeserializeOption& option);
} // namespace internal

/*
 *1. all struct need register a default constructor
 */
template <typename T>
bool from_json_obj(const json& json_object, T& out, const RttrDeserializeOption& option = {}) {
    rttr::variant var(out);
    internal::fromjson_recursively(json_object, var, out, option);
    // non-object types require an extra copy
    if (!json_object.is_object()) {
        if (var.can_convert<T>()) {
            out = var.get_value<T>();
        } else {
            return false;
        }
    }
    return true;
}

template <typename T>
bool from_json(const void* data, std::size_t dataLen, T& out, const RttrDeserializeOption& option = {}) {
    if (!data) return false;
    Fundamental::json json_obj;
    try {
        const char* pData = reinterpret_cast<const char*>(data);
        json_obj          = Fundamental::json::parse(pData, pData + dataLen);
    } catch (...) {
        return false;
    }
    return from_json_obj(json_obj, out, option);
}

template <typename T>
bool from_json(const std::string& jsonStr, T& out, const RttrDeserializeOption& option = {}) {
    return from_json(jsonStr.data(), jsonStr.size(), out, option);
}

template <typename DataType>
inline bool EnumTypeFromString(const std::string& str, DataType& type) {
    auto e   = rttr::type::get<DataType>().get_enumeration();
    auto ret = e.name_to_value(str);
    if (!ret.is_valid()) return false;
    type = ret.template get_value<DataType>();
    return true;
}
template <typename EnumType, typename DataType>
inline bool EnumTypeFromString(const std::string& str, DataType& type) {
    auto e   = rttr::type::get<EnumType>().get_enumeration();
    auto ret = e.name_to_value(str);
    if (!ret.is_valid()) return false;
    type = static_cast<DataType>(ret.template get_value<EnumType>());
    return true;
}
} // namespace io
} // namespace Fundamental