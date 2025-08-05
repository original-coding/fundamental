#include <array>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "serializer.h"
#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 26819 26437 26439 26495 26800 26498) // disable warning 4996
#endif
#include <rttr/registration>
#ifdef _MSC_VER
    #pragma warning(pop)
#endif
using namespace rttr;
RTTR_REGISTRATION {
    using namespace rttr;

    {
        RTTR_REGISTRATION_STANDARD_TYPE_VARIANTS(nlohmann::json);
    }
}

namespace
{

/////////////////////////////////////////////////////////////////////////////////////////
Fundamental::json to_json_recursively(const instance& obj, const Fundamental::RttrSerializeOption& option);

/////////////////////////////////////////////////////////////////////////////////////////

Fundamental::json write_variant(const variant& var, bool& flag, const Fundamental::RttrSerializeOption& option);
Fundamental::json write_variant(const variant& var, const Fundamental::RttrSerializeOption& option);
std::string write_variant_with_comment(const variant& var,
                                       bool& flag,
                                       std::string& indent_string,
                                       std::size_t current_indent,
                                       const Fundamental::RttrSerializeOption& option);
bool write_atomic_types_to_json(const type& t, const variant& var, Fundamental::json& json_obj) {
    if (t.is_arithmetic()) {
        if (t == type::get<bool>())
            json_obj = var.to_bool();
        else if (t == type::get<char>())
            json_obj = var.to_int8();
        else if (t == type::get<int8_t>())
            json_obj = var.to_int8();
        else if (t == type::get<int16_t>())
            json_obj = var.to_int16();
        else if (t == type::get<int32_t>())
            json_obj = var.to_int32();
        else if (t == type::get<int64_t>())
            json_obj = var.to_int64();
        else if (t == type::get<uint8_t>())
            json_obj = var.to_uint8();
        else if (t == type::get<uint16_t>())
            json_obj = var.to_uint16();
        else if (t == type::get<uint32_t>())
            json_obj = var.to_uint32();
        else if (t == type::get<uint64_t>())
            json_obj = var.to_uint64();
        else if (t == type::get<float>())
            json_obj = var.to_double();
        else if (t == type::get<double>())
            json_obj = var.to_double();

        return true;
    } else if (t.is_enumeration()) {
        bool ok     = false;
        auto result = var.to_string(&ok);
        if (ok) {
            json_obj = var.to_string();
        } else {
            ok         = false;
            auto value = var.to_uint64(&ok);
            if (ok)
                json_obj = value;
            else
                json_obj = Fundamental::json(); // Null
        }

        return true;
    } else if (t == type::get<std::string>()) {
        json_obj = var.to_string();
        return true;
    }

    return false;
}

/////////////////////////////////////////////////////////////////////////////////////////

static Fundamental::json write_array(const variant_sequential_view& view,
                                     const Fundamental::RttrSerializeOption& option) {
    Fundamental::json json_array = Fundamental::json::array();
    for (const auto& item : view) {
        if (item.is_sequential_container()) {
            Fundamental::json child = write_array(item.create_sequential_view(), option);
            json_array.push_back(child);
        } else {
            variant wrapped_var = item.extract_wrapped_value();
            type value_type     = wrapped_var.get_type();
            if (value_type.is_arithmetic() || value_type == type::get<std::string>() || value_type.is_enumeration()) {
                Fundamental::json child;
                write_atomic_types_to_json(value_type, wrapped_var, child);
                json_array.push_back(child);
            } else // object
            {
                Fundamental::json child = to_json_recursively(wrapped_var, option);
                json_array.push_back(child);
            }
        }
    }
    return json_array;
}

/////////////////////////////////////////////////////////////////////////////////////////

static Fundamental::json write_associative_container(const variant_associative_view& view,
                                                     const Fundamental::RttrSerializeOption& option) {
    static const std::string key_name("key");
    static const std::string value_name("value");

    Fundamental::json json_array = Fundamental::json::array();

    if (view.is_key_only_type()) {
        for (auto& item : view) {
            Fundamental::json child = write_variant(item.first.extract_wrapped_value(), option);
            json_array.push_back(child);
        }
    } else {
        for (auto& item : view) {
            Fundamental::json child = Fundamental::json::object();
            child[key_name]         = write_variant(item.first.extract_wrapped_value(), option);
            child[value_name]       = write_variant(item.second.extract_wrapped_value(), option);

            json_array.push_back(child);
        }
    }

    return json_array;
}

/////////////////////////////////////////////////////////////////////////////////////////
Fundamental::json write_variant(const variant& var, bool& flag, const Fundamental::RttrSerializeOption& option) {
    flag = true;

    Fundamental::json json_obj;
    auto value_type   = var.get_type();
    auto wrapped_type = value_type.is_wrapper() ? value_type.get_wrapped_type() : value_type;
    bool is_wrapper   = wrapped_type != value_type;

    if (write_atomic_types_to_json(is_wrapper ? wrapped_type : value_type,
                                   is_wrapper ? var.extract_wrapped_value() : var, json_obj)) {
    } else if (var.is_sequential_container()) {
        json_obj = write_array(var.create_sequential_view(), option);
    } else if (var.is_associative_container()) {
        json_obj = write_associative_container(var.create_associative_view(), option);
    } else if (var.is_type<nlohmann::json>()) {
        return var.get_value<nlohmann::json>();
    } else {

        decltype(wrapped_type.get_properties()) child_props =
            is_wrapper ? wrapped_type.get_properties() : value_type.get_properties();
        if (!child_props.empty()) {
            json_obj = to_json_recursively(var, option);
        } else {
            flag     = false;
            json_obj = var.to_string(&flag);
            if (!flag) {
                json_obj = nullptr;
            }
        }
    };

    return json_obj;
}

Fundamental::json write_variant(const variant& var, const Fundamental::RttrSerializeOption& option) {
    bool flag;
    return write_variant(var, flag, option);
}

std::string write_variant_with_comment(const variant& var,
                                       bool& flag,
                                       std::string& indent_string,
                                       std::size_t current_indent,
                                       const Fundamental::RttrSerializeOption& option) {

    std::string ret;
    bool new_flag  = true;
    bool new_flag2 = true;
    Fundamental::json json_obj;
    auto value_type   = var.get_type();
    auto wrapped_type = value_type.is_wrapper() ? value_type.get_wrapped_type() : value_type;
    bool is_wrapper   = wrapped_type != value_type;
    do {
        if (write_atomic_types_to_json(is_wrapper ? wrapped_type : value_type,
                                       is_wrapper ? var.extract_wrapped_value() : var, json_obj)) {
            ret += json_obj.dump();
        } else if (var.is_sequential_container()) {

            auto view        = var.create_sequential_view();
            std::size_t size = view.get_size();
            if (size == 0) {
                ret = "[]";
                break;
            }
            ret += "[\n";
            const auto new_indent = current_indent + 4;
            if (indent_string.size() < new_indent) {
                indent_string.resize(indent_string.size() * 2, ' ');
            }
            std::size_t index = 0;
            for (auto& item : view) {
                ret.insert(ret.size(), indent_string.data(), new_indent);
                variant wrapped_var = item.extract_wrapped_value();
                ret += write_variant_with_comment(wrapped_var, new_flag, indent_string, new_indent, option);
                if (index < size - 1) {
                    ret += ",\n";
                } else {
                    ret += "\n";
                }
                ++index;
            }

            ret.insert(ret.size(), indent_string.data(), current_indent);
            ret.push_back(']');

        } else if (var.is_associative_container()) {
            auto view        = var.create_associative_view();
            std::size_t size = view.get_size();
            if (size == 0) {
                ret = "[]";
                break;
            }
            ret += "[\n";
            const auto new_indent = current_indent + 4;
            if (indent_string.size() < new_indent) {
                indent_string.resize(indent_string.size() * 2, ' ');
            }
            std::size_t index = 0;
            for (auto& item : view) {
                ret.insert(ret.size(), indent_string.data(), new_indent);
                if (view.is_key_only_type()) {
                    ret += write_variant_with_comment(item.first.extract_wrapped_value(), new_flag, indent_string,
                                                      new_indent, option);
                } else {
                    ret.insert(ret.size(), indent_string.data(), new_indent);
                    ret += "{\n";
                    ret.insert(ret.size(), indent_string.data(), new_indent + 4);
                    ret += "\"key\": ";
                    ret += write_variant_with_comment(item.first.extract_wrapped_value(), new_flag, indent_string,
                                                      new_indent + 4, option);
                    ret += ",\n";
                    ret.insert(ret.size(), indent_string.data(), new_indent + 4);
                    ret += "\"value\": ";
                    ret += write_variant_with_comment(item.second.extract_wrapped_value(), new_flag2, indent_string,
                                                      new_indent + 4, option);
                    ret += "\n";
                    ret.insert(ret.size(), indent_string.data(), new_indent);
                    ret += "}";
                }
                if (index < size - 1) {
                    ret += ",\n";
                } else {
                    ret += "\n";
                }
                ++index;
            }
            ret.insert(ret.size(), indent_string.data(), current_indent);
            ret.push_back(']');
        } else if (var.is_type<nlohmann::json>()) {
            using serializer_t = ::nlohmann::detail::serializer<nlohmann::json>;
            using string_t     = std::string;
            string_t result;
            serializer_t s(::nlohmann::detail::output_adapter<char, string_t>(result), ' ',
                           ::nlohmann::detail::error_handler_t::strict);
            auto json_object = var.get_value<nlohmann::json>();
            s.dump(json_object, true, true, 4, static_cast<unsigned int>(4 + current_indent));
            ret = result;
        } else {
            std::size_t total_element_size = 0;
            instance obj2                  = var;
            instance obj = obj2.get_type().get_raw_type().is_wrapper() ? obj2.get_wrapped_instance() : obj2;

            auto prop_list = obj.get_derived_type().get_properties();
            for (auto& prop : prop_list) {
                if (!option.ValidateSerialize(prop)) continue;

                variant prop_value = prop.get_value(obj);
                if (!prop_value) continue; // cannot serialize, because we cannot retrieve the value
                ++total_element_size;
            }
            if (total_element_size == 0) {
                ret = "{}";
                break;
            }
            ret += "{\n";
            const auto new_indent = current_indent + 4;
            if (indent_string.size() < new_indent) {
                indent_string.resize(indent_string.size() * 2, ' ');
            }
            std::size_t index = 0;
            for (auto& prop : prop_list) {
                if (!option.ValidateSerialize(prop)) continue;

                variant prop_value = prop.get_value(obj);
                if (!prop_value) continue; // cannot serialize, because we cannot retrieve the value
                do {
                    if (!flag) break;
                    auto comment_value = prop.get_metadata(Fundamental::RttrMetaControlOption::CommentMetaDataKey());
                    if (!comment_value.is_valid()) break;
                    auto comment = comment_value.get_wrapped_value<std::string>();
                    if (comment.size() < 2) break;
                    if (std::strncmp(comment.data(), "//", 2) != 0) {
                        if (comment.size() < 4) break;
                        if (std::strncmp(comment.data(), "/*", 2) != 0) break;
                        if (std::strncmp(comment.data() + comment.size() - 2, "*/", 2) != 0) break;
                    }
                    for (auto& c : comment) {
                        if (!ret.empty() && *ret.rbegin() == '\n') {
                            ret.insert(ret.size(), indent_string.data(), new_indent);
                        }
                        ret += c;
                    }
                    ret += "\n";
                } while (0);

                ret.insert(ret.size(), indent_string.data(), new_indent);
                ret += "\"";
                ret += prop.get_name().to_string();
                ret += "\": ";

                ret += write_variant_with_comment(prop_value, new_flag, indent_string, new_indent, option);
                if (index < total_element_size - 1) {
                    ret += ",\n";
                } else {
                    ret += "\n";
                }
                ++index;
            }
            ret.insert(ret.size(), indent_string.data(), current_indent);
            ret.push_back('}');
            flag = false;
        };
    } while (0);

    return ret;
}

/////////////////////////////////////////////////////////////////////////////////////////

Fundamental::json to_json_recursively(const instance& obj2, const Fundamental::RttrSerializeOption& option) {
    Fundamental::json json_obj = Fundamental::json::object();

    instance obj = obj2.get_type().get_raw_type().is_wrapper() ? obj2.get_wrapped_instance() : obj2;

    auto prop_list = obj.get_derived_type().get_properties();
    for (auto& prop : prop_list) {
        if (!option.ValidateSerialize(prop)) continue;

        variant prop_value = prop.get_value(obj);
        if (!prop_value) continue; // cannot serialize, because we cannot retrieve the value

        const auto name = prop.get_name();
        bool flag;
        Fundamental::json child = write_variant(prop_value, flag, option);
        if (!flag) {
            std::cerr << "cannot serialize property: " << name << std::endl;
        }
        json_obj[name.to_string()] = child;
    }

    return json_obj;
}

} // namespace

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////

namespace Fundamental
{
namespace io
{

std::string to_json(const rttr::variant& var, const RttrSerializeOption& option) {
    if (var.is_type<nlohmann::json>()) {
        return var.get_value<nlohmann::json>().dump(4);
    }
    Fundamental::json json_obj = to_json_obj(var, option);
    return json_obj.dump(4);
}

std::string to_comment_json(const rttr::variant& var, const RttrSerializeOption& option) {
    if (var.is_type<nlohmann::json>()) {
        return var.get_value<nlohmann::json>().dump(4);
    }
    bool flag = true;
    std::string indent_str(512, ' ');
    return write_variant_with_comment(var, flag, indent_str, 0, option);
}

Fundamental::json to_json_obj(const rttr::variant& var, const RttrSerializeOption& option) {
    // optimisation for void data
    if (var.is_type<void>()) {
        return Fundamental::json(nullptr);
    }

    return write_variant(var, option);
}

} // end namespace io
} // namespace Fundamental