#include "binary_packer.h"

namespace Fundamental::io
{
using rttr::instance;
using rttr::type;
using rttr::variant;
using rttr::variant_associative_view;
using rttr::variant_sequential_view;
static_assert(sizeof(float) == 4, "only support float size 4");
static_assert(sizeof(double) == 8, "only support double size 8");
namespace internal
{
void binary_pack_array(const variant_sequential_view& view, std::vector<std::uint8_t>& out, bool& type_flag);
void binary_pack_object_recursively(const rttr::variant& var, std::vector<std::uint8_t>& out, bool& type_flag);
void binary_pack_associative_container(const variant_associative_view& view,
                                       std::vector<std::uint8_t>& out,
                                       bool& type_flag);
bool binary_unpack_object_recursively(const std::uint8_t*& data,
                                      std::size_t& len,
                                      rttr::instance dst_obj,
                                      bool ignore_invalid_properties);

bool binary_unpack_basic_value(const std::uint8_t*& data, std::size_t& len, rttr::variant& var, PackerDataType type);
bool binary_unpack_array(const std::uint8_t*& data,
                         std::size_t& len,
                         rttr::variant& var,
                         bool ignore_invalid_properties);
bool binary_unpack_set(const std::uint8_t*& data, std::size_t& len, rttr::variant& var, bool ignore_invalid_properties);
bool binary_unpack_map(const std::uint8_t*& data, std::size_t& len, rttr::variant& var, bool ignore_invalid_properties);
bool binary_unpack_string(const std::uint8_t*& data, std::size_t& len, std::string& out_str);
bool binary_unpack_skip_buf(const std::uint8_t*& data, std::size_t& len, std::size_t skip_len);
// for float double data
template <typename T>
void pack_basic_fixed_value(std::vector<std::uint8_t>& out, const T& value) {
    constexpr std::size_t value_size = sizeof(value);
    auto offset                      = out.size();
    out.resize(offset + value_size);
    auto* write_ptr = out.data() + offset;
    if constexpr (value_size > 1 && Fundamental::NeedConvertEndian<Fundamental::Endian::LittleEndian>()) {
        const std::uint8_t* p_src = (const std::uint8_t*)(&value);
        for (std::size_t i = 0; i < value_size; ++i) {
            write_ptr[i] = p_src[value_size - 1 - i];
        }
    } else {
        std::memcpy(write_ptr, &value, value_size);
    }
}

template <typename T>
void pack_basic_varint_value(std::vector<std::uint8_t>& out, const T& value) {
    constexpr std::size_t max_value_size = Fundamental::VarintEncodeGuessMaxSize<T>();
    auto offset                          = out.size();
    out.resize(offset + max_value_size);
    auto* write_ptr = out.data() + offset;
    auto write_size = Fundamental::VarintEncode(value, write_ptr);
    if (write_size != max_value_size) out.resize(offset + write_size);
}

// update  object array map preallocate 4bytes
void pack_update_item_size(std::vector<std::uint8_t>& out, std::size_t offset) {
    FASSERT(offset + sizeof(BinaryPackSizeType) <= out.size());
    auto* write_ptr         = out.data() + offset;
    BinaryPackSizeType item_size = out.size() - offset - sizeof(BinaryPackSizeType);
    if constexpr (Fundamental::NeedConvertEndian<Fundamental::Endian::LittleEndian>()) {
        const std::uint8_t* p_src = (const std::uint8_t*)(&item_size);
        for (std::size_t i = 0; i < sizeof(BinaryPackSizeType); ++i) {
            write_ptr[i] = p_src[sizeof(BinaryPackSizeType) - 1 - i];
        }
    } else {
        std::memcpy(write_ptr, &item_size, sizeof(BinaryPackSizeType));
    }
}

void pack_append_data(std::vector<std::uint8_t>& out, const void* data, std::size_t len) {
    auto offset = out.size();
    out.resize(offset + len);
    std::memcpy(out.data() + offset, data, len);
}

template <typename T>
void binary_pack_string(std::vector<std::uint8_t>& out, const T& value) {
    BinaryPackSizeType size = value.size();
    pack_basic_varint_value(out, size);
    pack_append_data(out, value.data(), size);
}

bool binary_pack_basic_value(const type& t, const variant& var, std::vector<std::uint8_t>& out, bool& type_flag) {
    if (t.is_arithmetic()) {
        if (t == type::get<bool>()) {
            if (!type_flag) pack_basic_varint_value(out, bool_pack_data);
            pack_basic_varint_value(out, var.to_bool());
        } else if (t == type::get<char>()) {
            if (!type_flag) pack_basic_varint_value(out, char_pack_data);
            pack_basic_varint_value(out, var.to_int8());
        } else if (t == type::get<int8_t>()) {
            if (!type_flag) pack_basic_varint_value(out, int8_pack_data);
            pack_basic_varint_value(out, var.to_int8());
        } else if (t == type::get<int16_t>()) {
            if (!type_flag) pack_basic_varint_value(out, int16_pack_data);
            pack_basic_varint_value(out, var.to_int16());
        } else if (t == type::get<int32_t>()) {
            if (!type_flag) pack_basic_varint_value(out, int32_pack_data);
            pack_basic_varint_value(out, var.to_int32());
        } else if (t == type::get<int64_t>()) {
            if (!type_flag) pack_basic_varint_value(out, int64_pack_data);
            pack_basic_varint_value(out, var.to_int64());
        } else if (t == type::get<uint8_t>()) {
            if (!type_flag) pack_basic_varint_value(out, uint8_pack_data);
            pack_basic_varint_value(out, var.to_uint8());
        } else if (t == type::get<uint16_t>()) {
            if (!type_flag) pack_basic_varint_value(out, uint16_pack_data);
            pack_basic_varint_value(out, var.to_uint16());
        } else if (t == type::get<uint32_t>()) {
            if (!type_flag) pack_basic_varint_value(out, uint32_pack_data);
            pack_basic_varint_value(out, var.to_uint32());
        } else if (t == type::get<uint64_t>()) {
            if (!type_flag) pack_basic_varint_value(out, uint64_pack_data);
            pack_basic_varint_value(out, var.to_uint64());
        } else if (t == type::get<float>()) {
            if (!type_flag) pack_basic_varint_value(out, float_pack_data);
            pack_basic_fixed_value(out, var.to_float());
        } else if (t == type::get<double>()) {
            if (!type_flag) pack_basic_varint_value(out, double_pack_data);
            pack_basic_fixed_value(out, var.to_double());
        }
    } else if (t.is_enumeration()) {
        if (!type_flag) pack_basic_varint_value(out, enum_pack_data);
        bool flag        = false;
        std::string data = var.to_string(&flag);
        if (!flag) {
            throw std::invalid_argument(Fundamental::StringFormat(
                "enum type:{} convert to string failed,you should register the enum type value",
                std::string(var.get_type().get_name())));
        }
        binary_pack_string(out, data);
    } else if (t == type::get<std::string>()) {
        if (!type_flag) pack_basic_varint_value(out, string_pack_data);
        binary_pack_string(out, var.to_string());
    } else if (t == type::get<const char*>()) {
        if (!type_flag) pack_basic_varint_value(out, string_pack_data);
        auto str = var.get_value<const char*>();
        if (!str) throw std::invalid_argument("str can't be null");
        auto str_size = strlen(str);
        binary_pack_string(out, std::string(str, str_size));
    } else {
        return false;
    }
    type_flag = true;
    return true;
}
void binary_pack_array(const variant_sequential_view& view, std::vector<std::uint8_t>& out, bool& type_flag) {
    if (!type_flag) pack_basic_varint_value(out, array_pack_data);
    type_flag          = true;
    std::size_t offset = out.size();
    out.resize(offset + sizeof(BinaryPackSizeType) /*following data size*/);
    // write element nums
    BinaryPackSizeType nums = view.get_size();
    pack_basic_varint_value(out, nums);
    //
    bool flag = false;

    for (const auto& item : view) {
        variant wrapped_var = item.extract_wrapped_value();
        do_binary_pack(wrapped_var, out, flag);
    }
    // update data size
    pack_update_item_size(out, offset);
}
void binary_pack_associative_container(const variant_associative_view& view,
                                       std::vector<std::uint8_t>& out,
                                       bool& type_flag) {
    if (!type_flag) pack_basic_varint_value(out, view.is_key_only_type() ? set_pack_data : map_pack_data);
    type_flag          = true;
    std::size_t offset = out.size();
    out.resize(offset + sizeof(BinaryPackSizeType) /*following data size*/);
    // write element nums
    BinaryPackSizeType nums = view.get_size();
    pack_basic_varint_value(out, nums);
    bool key_flag   = false;
    bool value_flag = false;
    if (view.is_key_only_type()) {
        for (auto& item : view) {
            do_binary_pack(item.first, out, key_flag);
        }

    } else {
        for (auto& item : view) {
            do_binary_pack(item.first, out, key_flag);
            do_binary_pack(item.second, out, value_flag);
        }
    }
    // update data size
    pack_update_item_size(out, offset);
}

void binary_pack_object_recursively(const rttr::variant& var, std::vector<std::uint8_t>& out, bool& type_flag) {
    instance obj2(var);
    instance obj = obj2.get_type().get_raw_type().is_wrapper() ? obj2.get_wrapped_instance() : obj2;

    auto prop_list = obj.get_derived_type().get_properties();

    if (!type_flag) pack_basic_varint_value(out, prop_list.empty() ? custom_object_pack_data : object_pack_data);
    type_flag          = true;
    std::size_t offset = out.size();
    out.resize(offset + sizeof(BinaryPackSizeType) /*following data size*/);

    if (!prop_list.empty()) {
        for (auto& prop : prop_list) {

            variant prop_value = prop.get_value(obj);
            if (!prop_value) continue; // cannot serialize, because we cannot retrieve the value
            // write prop name
            const auto name = prop.get_name();
            binary_pack_string(out, name);
            bool flag = false;
            do_binary_pack(prop_value, out, flag);
        }
    } else {
        bool flag        = false;
        std::string data = var.to_string(&flag);
        if (!flag) {
            throw std::invalid_argument(Fundamental::StringFormat(
                "type:{} convert to string failed,you should register convert func "
                "std::string(const {} &,bool &) with type::register_converter_func",
                std::string(var.get_type().get_name()), std::string(var.get_type().get_name())));
        }
        binary_pack_string(out, data);
    }
    // update data size
    pack_update_item_size(out, offset);
}

void do_binary_pack(const rttr::variant& var, std::vector<std::uint8_t>& out, bool& type_flag) {

    // handle array
    // handle map
    // handle object

    auto value_type   = var.get_type();
    auto wrapped_type = value_type.is_wrapper() ? value_type.get_wrapped_type() : value_type;
    bool is_wrapper   = wrapped_type != value_type;
    // handle basic type
    if (binary_pack_basic_value(is_wrapper ? wrapped_type : value_type, is_wrapper ? var.extract_wrapped_value() : var,
                                out, type_flag)) {
    } else if (var.is_sequential_container()) {
        binary_pack_array(var.create_sequential_view(), out, type_flag);
    } else if (var.is_associative_container()) {
        binary_pack_associative_container(var.create_associative_view(), out, type_flag);
    } else {
        binary_pack_object_recursively(var, out, type_flag);
    };
}

bool binary_unpack_string(const std::uint8_t*& data, std::size_t& len, std::string& out_str) {
    BinaryPackSizeType str_size = 0;
    if (!unpack_basic_varint_value(data, len, str_size)) {
        return false;
    }
    if (len < str_size) {
        FERR("str buf is invalid need {} actual {}", str_size, len);
        return false;
    }
    out_str.resize(str_size);
    std::memcpy(out_str.data(), data, str_size);
    data += str_size;
    len -= str_size;
    return true;
}

bool binary_unpack_peek_chunk_size(const std::uint8_t*& data,
                                   std::size_t& len,
                                   BinaryPackSizeType& chunk_size,
                                   BinaryPackSizeType& total_size) {
    if (!unpack_basic_fixed_value(data, len, chunk_size)) {
        return false;
    }
    if (len < chunk_size) {
        FERR("chukn buffer is invalid");
        return false;
    }
    total_size = len;
    return true;
}

bool binary_unpack_peek_varint_chunk_size(const std::uint8_t*& data,
                                          std::size_t& len,
                                          BinaryPackSizeType& chunk_size,
                                          BinaryPackSizeType& total_size) {
    if (!unpack_basic_varint_value(data, len, chunk_size)) {
        return false;
    }
    if (len < chunk_size) {
        FERR("chukn buffer is invalid");
        return false;
    }
    total_size = len;
    return true;
}

bool do_binary_unpack(const std::uint8_t*& data,
                      std::size_t& len,
                      rttr::variant& var,
                      rttr::instance dst_obj,
                      PackerDataType& type,
                      bool ignore_invalid_properties) {
    if (type == unknown_pack_data) {
        if (!unpack_basic_varint_value(data, len, type)) return false;
    }
    switch (type) {
    case bool_pack_data:
    case char_pack_data:
    case int8_pack_data:
    case int16_pack_data:
    case int32_pack_data:
    case int64_pack_data:
    case uint8_pack_data:
    case uint16_pack_data:
    case uint32_pack_data:
    case double_pack_data:
    case float_pack_data:
    case uint64_pack_data:
        if (!var.get_type().is_arithmetic() && !var.get_type().get_wrapped_type().is_arithmetic()) return false;
         [[fallthrough]];
    case string_pack_data:
    case enum_pack_data: return binary_unpack_basic_value(data, len, var, type);
    case array_pack_data: return binary_unpack_array(data, len, var, ignore_invalid_properties);
    case set_pack_data: return binary_unpack_set(data, len, var, ignore_invalid_properties);
    case map_pack_data: return binary_unpack_map(data, len, var, ignore_invalid_properties);
    case custom_object_pack_data: {
        BinaryPackSizeType object_size = 0;
        BinaryPackSizeType total_size  = 0;
        if (!binary_unpack_peek_chunk_size(data, len, object_size, total_size)) return false;
        std::string description;
        if (!binary_unpack_string(data, len, description)) return false;
        // empty pack object
        rttr::variant temp_var = description;
        if (temp_var.convert(var.get_type().is_wrapper() ? var.get_type().get_wrapped_type() : var.get_type())) {
            var = temp_var;
            return true;
        } else {
            FERR("type:{} convert from string failed,you should register convert func "
                 "{} (const std::string &,bool &) with type::register_converter_func",
                 std::string(var.get_type().get_name()), std::string(var.get_type().get_name()));
            return false;
        }
    }
    case object_pack_data: return binary_unpack_object_recursively(data, len, dst_obj, ignore_invalid_properties);
    default: FERR("unsupported packed type:{}", static_cast<std::int32_t>(type)); break;
    }
    return false;
}

bool binary_unpack_object_recursively(const std::uint8_t*& data,
                                      std::size_t& len,
                                      rttr::instance dst_obj,
                                      bool ignore_invalid_properties) {
    BinaryPackSizeType object_size = 0;
    BinaryPackSizeType total_size  = 0;
    if (!binary_unpack_peek_chunk_size(data, len, object_size, total_size)) return false;
    instance obj         = dst_obj.get_type().get_raw_type().is_wrapper() ? dst_obj.get_wrapped_instance() : dst_obj;
    const auto prop_list = obj.get_derived_type().get_properties();
    std::map<std::string, decltype(*prop_list.begin())> all_properties;
    for (auto& prop : prop_list)
        all_properties.emplace(prop.get_name(), prop);
    while (len + object_size > total_size) {
        std::string prop_name;
        if (!binary_unpack_string(data, len, prop_name)) return false;
        auto iter = all_properties.find(prop_name);
        if (iter == all_properties.end()) {
            if (ignore_invalid_properties) {
                // skip value
                if (!binary_unpack_skip_item(data, len)) return false;
                continue;
            } else {
                return false;
            }
        }

        auto& prop      = iter->second;
        auto object_var = prop.get_value(obj);
        if (!object_var.is_valid()) return false;
        auto data_type = PackerDataType::unknown_pack_data;
        if (!do_binary_unpack(data, len, object_var, object_var, data_type, ignore_invalid_properties)) {
            FERR("unpack \"{}\" failed", prop_name);
            return false;
        }
        if (prop.get_type().is_enumeration() && !object_var.convert(prop.get_type())) {
            FERR("unpack convert enum prop \"{}\" value failed", prop_name);
            return false;
        }
        if (!prop.set_value(obj, object_var)) {
            FERR("unpack set \"{}\" value failed", prop_name);
            return false;
        }
    }

    return true;
}

bool binary_unpack_basic_value(const std::uint8_t*& data, std::size_t& len, rttr::variant& var, PackerDataType type) {

    switch (type) {
    case bool_pack_data: {
        bool v = false;
        if (!unpack_basic_varint_value(data, len, v)) return false;
        var = v;
        break;
    }
    case char_pack_data: {
        char v = 0;
        if (!unpack_basic_varint_value(data, len, v)) return false;
        var = v;
        break;
    }
    case int8_pack_data: {
        std::int8_t v = 0;
        if (!unpack_basic_varint_value(data, len, v)) return false;
        var = v;
        break;
    }
    case int16_pack_data: {
        std::int16_t v = 0;
        if (!unpack_basic_varint_value(data, len, v)) return false;
        var = v;
        break;
    }
    case int32_pack_data: {
        std::int32_t v = 0;
        if (!unpack_basic_varint_value(data, len, v)) return false;
        var = v;
        break;
    }
    case int64_pack_data: {
        std::int64_t v = 0;
        if (!unpack_basic_varint_value(data, len, v)) return false;
        var = v;
        break;
    }
    case uint8_pack_data: {
        std::uint8_t v = 0;
        if (!unpack_basic_varint_value(data, len, v)) return false;
        var = v;
        break;
    }
    case uint16_pack_data: {
        std::uint16_t v = 0;
        if (!unpack_basic_varint_value(data, len, v)) return false;
        var = v;
        break;
    }
    case uint32_pack_data: {
        std::uint32_t v = 0;
        if (!unpack_basic_varint_value(data, len, v)) return false;
        var = v;
        break;
    }
    case float_pack_data: {
        float v = 0;
        if (!unpack_basic_fixed_value(data, len, v)) return false;
        var = v;
        break;
    }
    case double_pack_data: {
        double v = 0;
        if (!unpack_basic_fixed_value(data, len, v)) return false;
        var = v;
        break;
    }
    case string_pack_data: {
        std::string v;
        if (!binary_unpack_string(data, len, v)) return false;
        var = v;
        break;
    }
    case uint64_pack_data: {
        std::uint64_t v = 0;
        if (!unpack_basic_varint_value(data, len, v)) return false;
        var = v;
        break;
    }
    case enum_pack_data: {
        auto enum_type       = var.get_type();
        bool is_enum         = enum_type.is_enumeration();
        bool is_wrapper_enum = enum_type.is_wrapper() && enum_type.get_wrapped_type().is_enumeration();
        if (!is_enum && !is_wrapper_enum) return false;
        std::string v;
        if (!binary_unpack_string(data, len, v)) return false;
        rttr::variant temp_var = v;
        if (!temp_var.convert(enum_type.is_wrapper() ? enum_type.get_wrapped_type() : var.get_type())) return false;
        var = temp_var;
        break;
    }
    default: FERR("unsupported packed type:{}", static_cast<std::int32_t>(type)); return false;
    }
    return true;
}

bool binary_unpack_array(const std::uint8_t*& data,
                         std::size_t& len,
                         rttr::variant& var,
                         bool ignore_invalid_properties) {
    auto type = var.get_type();
    if (!type.is_sequential_container() && !type.get_wrapped_type().is_sequential_container()) {
        FERR("invalid array value type {} {}", std::string(type.get_name()),
             std::string(type.get_wrapped_type().get_name()));
        return false;
    }
    auto view = var.create_sequential_view();
    view.clear();
    BinaryPackSizeType object_size = 0;
    BinaryPackSizeType total_size  = 0;
    if (!binary_unpack_peek_chunk_size(data, len, object_size, total_size)) return false;
    BinaryPackSizeType item_nums = 0;
    if (!unpack_basic_varint_value(data, len, item_nums)) return false;
    view.set_size(item_nums);
    PackerDataType data_type = PackerDataType::unknown_pack_data;
    for (size_t i = 0; i < item_nums; ++i) {
        auto v = view.get_value(i);
#ifdef DEBUG
        [[maybe_unused]] auto value_type = v.get_type();
#endif
        if (!do_binary_unpack(data, len, v, v, data_type, ignore_invalid_properties)) {
            FERR("unpack index {} failed", i);
            return false;
        }
        view.set_value(i, v);
    }
    return true;
}

bool binary_unpack_set(const std::uint8_t*& data,
                       std::size_t& len,
                       rttr::variant& var,
                       bool ignore_invalid_properties) {

    auto type = var.get_type();
    if (!type.is_associative_container() && !type.get_wrapped_type().is_associative_container()) {
        FERR("invalid set value type {}  {}", std::string(type.get_name()),
             std::string(type.get_wrapped_type().get_name()));
        return false;
    }
    auto view = var.create_associative_view();

    BinaryPackSizeType object_size = 0;
    BinaryPackSizeType total_size  = 0;
    if (!binary_unpack_peek_chunk_size(data, len, object_size, total_size)) return false;
    BinaryPackSizeType item_nums = 0;

    if (!unpack_basic_varint_value(data, len, item_nums)) return false;

    view.clear();
    if (!view.reserve_key()) {
        FERR("invalid set type {}", std::string(type.get_name()));
        return false;
    }
    auto iter = view.begin();
    if (iter == view.end()) {
        FERR("invalid set type call reserve_key {}", std::string(type.get_name()));
        return false;
    }
    auto item_var = iter.get_key();
    if (!item_var.convert(view.get_key_type())) {
        FERR("set  convert from {} to {} failed", std::string(item_var.get_type().get_name()),
             std::string(view.get_key_type().get_name()));
        return false;
    }
#ifdef DEBUG
    [[maybe_unused]] auto key_type = item_var.get_type();
#endif
    view.clear();
    PackerDataType data_type = PackerDataType::unknown_pack_data;
    for (size_t i = 0; i < item_nums; ++i) {
        if (!do_binary_unpack(data, len, item_var, item_var, data_type, ignore_invalid_properties)) return false;
        view.insert(item_var);
    }

    return true;
}

bool binary_unpack_map(const std::uint8_t*& data,
                       std::size_t& len,
                       rttr::variant& var,
                       bool ignore_invalid_properties) {
    auto type = var.get_type();
    if (!type.is_associative_container() && !type.get_wrapped_type().is_associative_container()) {
        FERR("invalid map value type {}", std::string(type.get_name()));
        return false;
    }
    auto view                 = var.create_associative_view();
    BinaryPackSizeType object_size = 0;
    BinaryPackSizeType total_size  = 0;
    if (!binary_unpack_peek_chunk_size(data, len, object_size, total_size)) return false;
    BinaryPackSizeType item_nums = 0;
    if (!unpack_basic_varint_value(data, len, item_nums)) return false;

    view.clear();
    if (!view.reserve_key_value()) {
        FERR("invalid map type {}", std::string(type.get_name()));
        return false;
    }
    auto iter = view.begin();
    if (iter == view.end()) {
        FERR("invalid map type call reserve_key_value {}", std::string(type.get_name()));
        return false;
    }
    auto item_key_var = iter.get_key();
    if (!item_key_var.convert(view.get_key_type())) {
        FERR("map key convert from {} to {} failed", std::string(item_key_var.get_type().get_name()),
             std::string(view.get_key_type().get_name()));
        return false;
    }
    auto item_value_var = iter.get_value();
    if (!item_value_var.convert(view.get_value_type())) {
        FERR("map value convert from {} to {} failed", std::string(item_value_var.get_type().get_name()),
             std::string(view.get_value_type().get_name()));
        return false;
    }
#ifdef DEBUG
    [[maybe_unused]] auto key_type   = item_key_var.get_type();
    [[maybe_unused]] auto value_type = item_value_var.get_type();
#endif
    view.clear();
    PackerDataType key_data_type   = PackerDataType::unknown_pack_data;
    PackerDataType value_data_type = PackerDataType::unknown_pack_data;
    for (size_t i = 0; i < item_nums; ++i) {
        if (!do_binary_unpack(data, len, item_key_var, item_key_var, key_data_type, ignore_invalid_properties))
            return false;
        if (!do_binary_unpack(data, len, item_value_var, item_value_var, value_data_type, ignore_invalid_properties))
            return false;
        view.insert(item_key_var, item_value_var);
    }
    return true;
}
bool binary_unpack_skip_item(const std::uint8_t*& data, std::size_t& len) {
    PackerDataType type = unknown_pack_data;
    if (!unpack_basic_varint_value(data, len, type)) return false;
    switch (type) {
    case bool_pack_data:
    case char_pack_data:
    case int8_pack_data:
    case uint8_pack_data: return binary_unpack_skip_buf(data, len, 1);
    case int16_pack_data:
    case uint16_pack_data:
    case int32_pack_data:
    case uint32_pack_data:
    case int64_pack_data:
    case uint64_pack_data: {
        if (len < 1) return false;
        std::size_t size = Fundamental::VarintDecodePeekSize(data);
        return binary_unpack_skip_buf(data, len, size);
    }
    case float_pack_data: return binary_unpack_skip_buf(data, len, 4);
    case double_pack_data: return binary_unpack_skip_buf(data, len, 8);
    case string_pack_data:        // perform as an object with BinaryPackSizeType size and data
    case enum_pack_data:          // perform as string
    case custom_object_pack_data: // perform as a string
    {
        BinaryPackSizeType object_size = 0;
        BinaryPackSizeType total_size  = 0;
        if (!binary_unpack_peek_varint_chunk_size(data, len, object_size, total_size)) return false;
        return binary_unpack_skip_buf(data, len, object_size);
    }
    case array_pack_data:
    case set_pack_data:
    case map_pack_data:
    case object_pack_data: {
        BinaryPackSizeType object_size = 0;
        BinaryPackSizeType total_size  = 0;
        if (!binary_unpack_peek_chunk_size(data, len, object_size, total_size)) return false;
        return binary_unpack_skip_buf(data, len, object_size);
    }
    default: break;
    }
    return false;
}
bool binary_unpack_skip_buf(const std::uint8_t*& data, std::size_t& len, std::size_t skip_len) {
    if (len < skip_len) {
        FERR("skip buf failed");
        return false;
    }
    data = data + skip_len;
    len -= skip_len;
    return true;
}
} // namespace internal

} // namespace Fundamental::io