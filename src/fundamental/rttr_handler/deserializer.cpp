#include "deserializer.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 26819 26437 26439 26495 26800 26498) // disable warning 4996
#endif
#include <rttr/registration>
#ifdef _MSC_VER
    #pragma warning(pop)
#endif
#include <string>
#include <vector>
using namespace rttr;
namespace Fundamental::io::internal
{
RTTR_REGISTRATION {
    using namespace rttr;

    {
        RTTR_REGISTRATION_STANDARD_TYPE_VARIANTS(nlohmann::json);
    }
}

variant extract_basic_types(const Fundamental::json& json_value);
void write_array(const Fundamental::json& json_array_value,
                 rttr::variant& var,
                 const Fundamental::RttrDeserializeOption& option);
void write_set(const Fundamental::json& json_array_value,
               rttr::variant& var,
               const Fundamental::RttrDeserializeOption& option);
void write_map(const Fundamental::json& json_array_value,
               rttr::variant& var,
               const Fundamental::RttrDeserializeOption& option);

/////////////////////////////////////////////////////////////////////////////////////////

std::string replace_env_vars(const std::string& input, bool& has_env_var, bool& has_null_env) {
    has_env_var = false;
    has_null_env = false;
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        if (input[i] == '$' && i + 1 < input.size()) {
            if (input[i + 1] == '{') {
                size_t end = input.find('}', i + 2);
                if (end != std::string::npos) {
                    has_env_var = true;
                    std::string var_name = input.substr(i + 2, end - i - 2);
                    const char* env_val = std::getenv(var_name.c_str());
                    if (env_val) result += env_val;
                    else has_null_env = true;
                    i = end + 1;
                    continue;
                }
            } else if (std::isalpha(static_cast<unsigned char>(input[i + 1])) || input[i + 1] == '_') {
                has_env_var = true;
                size_t j = i + 1;
                while (j < input.size() && (std::isalnum(static_cast<unsigned char>(input[j])) || input[j] == '_'))
                    ++j;
                std::string var_name = input.substr(i + 1, j - i - 1);
                const char* env_val = std::getenv(var_name.c_str());
                if (env_val) result += env_val;
                else has_null_env = true;
                i = j;
                continue;
            }
        }
        result += input[i];
        ++i;
    }
    return result;
}

void fromjson_recursively(const json& json_object,
                          rttr::variant& var,
                          rttr::instance obj2,
                          const RttrDeserializeOption& option) {
    if (!var.is_valid()) return;
    if (var.is_type<nlohmann::json>()) {
        var = json_object;
        return;
    }
    auto type            = var.get_type();
    auto wrapped_type    = type.get_wrapped_type();
    instance obj         = obj2.get_type().get_raw_type().is_wrapper() ? obj2.get_wrapped_instance() : obj2;
    const auto prop_list = obj.get_derived_type().get_properties();

    if (prop_list.empty()) {
        if (type.is_sequential_container() || wrapped_type.is_sequential_container()) { // handle array
            write_array(json_object, var, option);
        } else if (type.is_associative_container() || wrapped_type.is_associative_container()) {
            auto view = var.create_associative_view();
            if (view.is_key_only_type())
                write_set(json_object, var, option);
            else {
                write_map(json_object, var, option);
            }
        } else { // basic type
            auto target_type = var.get_type();
            if (target_type.is_wrapper()) target_type = target_type.get_wrapped_type();
            const auto& ref_type = target_type;

            bool handled = false;

            if (json_object.is_string() && option.apply_env_replace) {
                bool has_env_var = false;
                bool has_null_env = false;
                auto replaced = replace_env_vars(json_object.get<std::string>(), has_env_var, has_null_env);
                if (has_env_var) {
                    variant extracted_value = replaced;
                    if (has_null_env || !extracted_value.convert(ref_type)) {
                        if (option.env_default_value.is_valid()) {
                            extracted_value = option.env_default_value;
                            extracted_value.convert(ref_type);
                        }
                    }
                    var = extracted_value;
                    handled = true;
                }
            }

            if (!handled) {
                variant extracted_value = extract_basic_types(json_object);
                if (extracted_value.convert(ref_type))
                    var = extracted_value;
            }
        }

    } else {
        for (auto& prop : prop_list) {
            // ignore noserialized data
            if (!option.ValidateSerialize(prop)) continue;
            auto name = prop.get_name();
            auto iter = json_object.find(name.data());
            if (iter == json_object.end()) {
                continue;
            }
            const Fundamental::json& json_value = iter.value();

            auto child_option = option;
            if (option.enable_env_replace) {
                child_option.apply_env_replace = false;
                auto env_replace_meta = prop.get_metadata(RttrMetaControlOption::EnvReplaceMetaDataKey());
                if (env_replace_meta.is_valid()) {
                    child_option.apply_env_replace = true;
                    auto env_default_meta = prop.get_metadata(RttrMetaControlOption::EnvDefaultMetaDataKey());
                    if (env_default_meta.is_valid()) {
                        child_option.env_default_value = env_default_meta;
                    }
                }
            }

            variant var_tmp = prop.get_value(obj);
            fromjson_recursively(json_value, var_tmp, var_tmp, child_option);
            if (var_tmp.is_valid()) prop.set_value(obj, var_tmp);
        }
    }
}

variant extract_basic_types(const Fundamental::json& json_value) {
    switch (json_value.type()) {
    case Fundamental::json::value_t::string: {
        return json_value.get<std::string>();
        break;
    }
    case Fundamental::json::value_t::null: {
        break;
    }
    case Fundamental::json::value_t::boolean: {
        return json_value.get<bool>();
        break;
    }
    case Fundamental::json::value_t::number_integer: {
        return json_value.get<int64_t>();
        break;
    }
    case Fundamental::json::value_t::number_float: {
        return json_value.get<double>();
        break;
    }
    case Fundamental::json::value_t::number_unsigned: {
        return json_value.get<uint64_t>();
        break;
    }
    // we handle only the basic types here
    case Fundamental::json::value_t::object:
    case Fundamental::json::value_t::array: {
        return variant();
        break;
    }
    default: break;
    }

    return variant();
}

void write_array(const Fundamental::json& json_array_value,
                 rttr::variant& var,
                 const Fundamental::RttrDeserializeOption& option) {
    if (!json_array_value.is_array()) return;

    auto view = var.create_sequential_view();
    view.clear();
    view.set_size(json_array_value.size());

    for (size_t i = 0; i < json_array_value.size(); ++i) {
        auto& json_index_value = json_array_value[i];
        variant var_tmp        = view.get_value(i);
        fromjson_recursively(json_index_value, var_tmp, var_tmp, option);
        view.set_value(i, var_tmp);
    }
}

void write_set(const Fundamental::json& json_array_value,
               rttr::variant& var,
               const Fundamental::RttrDeserializeOption& option) {
    if (!json_array_value.is_array()) return;

    auto view = var.create_associative_view();
    view.clear();
    if (!view.reserve_key()) {
        return;
    }
    auto iter = view.begin();
    if (iter == view.end()) {
        return;
    }
    auto item_var = iter.get_key();
    if (!item_var.convert(view.get_key_type())) {
        return;
    }
    view.clear();

    for (size_t i = 0; i < json_array_value.size(); ++i) {
        auto& json_index_value = json_array_value[i];
        fromjson_recursively(json_index_value, item_var, item_var, option);
        view.insert(item_var);
    }
}
void write_map(const Fundamental::json& json_array_value,
               rttr::variant& var,
               const Fundamental::RttrDeserializeOption& option) {
    if (!json_array_value.is_array()) return;

    auto view = var.create_associative_view();

    view.clear();
    if (!view.reserve_key_value()) {
        return;
    }
    auto iter = view.begin();
    if (iter == view.end()) {
        return;
    }
    auto item_key_var = iter.get_key();
    if (!item_key_var.convert(view.get_key_type())) {
        return;
    }
    auto item_value_var = iter.get_value();
    if (!item_value_var.convert(view.get_value_type())) {
        return;
    }
    view.clear();
    for (size_t i = 0; i < json_array_value.size(); ++i) {
        auto& json_index_value = json_array_value[i];
        if (json_index_value.is_object()) // a key-value associative view
        {
            auto key_itr   = json_index_value.find("key");
            auto value_itr = json_index_value.find("value");
            if (key_itr != json_index_value.end() && value_itr != json_index_value.end()) {
                fromjson_recursively(*key_itr, item_key_var, item_key_var, option);
                fromjson_recursively(*value_itr, item_value_var, item_value_var, option);
                view.insert(item_key_var, item_value_var);
            }
        }
    }
}
} // namespace Fundamental::io::internal