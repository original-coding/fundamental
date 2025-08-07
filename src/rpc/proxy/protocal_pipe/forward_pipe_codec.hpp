#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
namespace network
{
namespace forward
{
enum forward_option : std::uint32_t
{
    forward_disable_option,
    forward_optional_option,
    forward_required_option,
    forward_option_NUM
};

enum forward_parse_status
{
    forward_parse_need_more_data,
    forward_parse_success,
    forward_parse_failed,
};

enum forward_protocal_t
{
    forward_raw,
    forward_websocket,
    forward_add_server,
    forward_protocal_num,
};
//(total_len_str)(key)#(value_len_str)(value)(key)#(value_len_str)...
struct forward_context_interface {
    static constexpr std::int8_t kForwardMagicNum = '*';
    static constexpr std::size_t kMagicNumSize    = 1;
    static constexpr std::size_t kMaxPayloadSize  = 9999;
    static constexpr std::size_t kFrameSizeStrLen = 4;
    static constexpr std::size_t kMaxFrameSize    = kFrameSizeStrLen + kMaxPayloadSize + kMagicNumSize;
    static constexpr std::int8_t kSplitChar       = '#';
    inline static constexpr std::array<const char*, forward_option_NUM> kForwardOptionArray = { "disabled", "optional",
                                                                                                "required" };
    inline static const std::unordered_map<std::string_view, forward_option> kForwardOptionMap = []() {
        std::unordered_map<std::string_view, forward_option> map;
        for (std::size_t i = 0; i < kForwardOptionArray.size(); ++i) {
            map[kForwardOptionArray[i]] = static_cast<forward_option>(i);
        }
        return map;
    }();
    inline static constexpr std::array<const char*, forward_protocal_num> kForwardProtocalArray = { "raw", "websocket",
                                                                                                    "add_server" };
    inline static const std::unordered_map<std::string_view, forward_protocal_t> kForwardProtocalMap = []() {
        std::unordered_map<std::string_view, forward_protocal_t> map;
        for (std::size_t i = 0; i < kForwardProtocalArray.size(); ++i) {
            map[kForwardProtocalArray[i]] = static_cast<forward_protocal_t>(i);
        }
        return map;
    }();
    std::tuple<bool, std::string> encode() {
        bool ret = false;
        std::string ret_str;
        ret_str.push_back(kForwardMagicNum);
        do {
            auto [bEncodeSucess, encode_str] = encode_imp();
            if (!bEncodeSucess) break;
            auto [bSuccess, len_str] = format_len(encode_str.size());
            if (!bSuccess) break;
            ret = true;
            ret_str += len_str + encode_str;
        } while (0);
        return std::make_tuple(ret, ret_str);
    }
    // On success, it returns the number of bytes parsed this time; when more data is needed, it returns the number of
    // additional bytes required to be read.
    std::tuple<forward_parse_status, std::size_t> decode(const void* data, std::size_t len) {
        auto status  = forward_parse_status::forward_parse_failed;
        auto ret_len = len;
        do {
            if (len + parse_cache.size() > kMaxFrameSize) {
                break;
            }
            auto ptr = static_cast<const std::uint8_t*>(data);
            if (len > 0) parse_cache.insert(parse_cache.end(), ptr, ptr + len);
            if (parse_cache.size() < kFrameSizeStrLen + kMagicNumSize) {
                status  = forward_parse_status::forward_parse_need_more_data;
                ret_len = kFrameSizeStrLen + kMagicNumSize - parse_cache.size();
                break;
            }
            if (payload_size == 0) {
                if (parse_cache[0] != kForwardMagicNum) {
                    break;
                }
                auto [len_ret, parse_payload_size] = peek_len(parse_cache.data() + kMagicNumSize, kFrameSizeStrLen);
                // failed
                if (!len_ret || parse_payload_size > kMaxPayloadSize || parse_payload_size == 0) break;
                payload_size = parse_payload_size;
            }
            if (kMagicNumSize + kFrameSizeStrLen + payload_size > parse_cache.size()) {
                ret_len = kMagicNumSize + kFrameSizeStrLen + payload_size - parse_cache.size();
                status  = forward_parse_status::forward_parse_need_more_data;
                break;
            }
            ret_len = len - (parse_cache.size() - (kMagicNumSize + kFrameSizeStrLen + payload_size));
            parse_cache.resize(kMagicNumSize + kFrameSizeStrLen + payload_size);

            std::size_t parse_pos = kFrameSizeStrLen + kMagicNumSize;
            // parse headers
            while (parse_pos < parse_cache.size()) {
                auto key_pos = parse_cache.find(kSplitChar, parse_pos);
                if (key_pos == std::string::npos) {
                    goto FORWARD_PARSE_FAILED;
                }
                auto key             = parse_cache.substr(parse_pos, key_pos - parse_pos);
                auto value_start_pos = key_pos + 1;
                if (value_start_pos + kFrameSizeStrLen > parse_cache.size()) {
                    goto FORWARD_PARSE_FAILED;
                }
                auto [value_len_ret, value_len] = peek_len(parse_cache.data() + value_start_pos, kFrameSizeStrLen);
                if (!value_len_ret) {
                    goto FORWARD_PARSE_FAILED;
                }
                value_start_pos += kFrameSizeStrLen;
                if (value_start_pos + value_len > parse_cache.size()) {
                    goto FORWARD_PARSE_FAILED;
                }
                auto value = parse_cache.substr(value_start_pos, value_len);
                if (!decode_item(key, std::move(value))) {
                    goto FORWARD_PARSE_FAILED;
                }
                parse_pos = value_start_pos + value_len;
            }
            parse_cache.clear();
            parse_cache.shrink_to_fit();
            payload_size = 0;
            status       = forward_parse_status::forward_parse_success;
            break;

        } while (0);
    FORWARD_PARSE_FAILED:
        return std::make_tuple(status, ret_len);
    }
    //
protected:
    std::tuple<bool, std::string> format_len(std::size_t len) const {
        std::string ret_str = std::to_string(len);
        if (ret_str.size() < kFrameSizeStrLen) ret_str.insert(0, kFrameSizeStrLen - ret_str.size(), '0');
        return std::make_tuple(ret_str.size() == kFrameSizeStrLen, ret_str);
    }
    std::tuple<bool, std::size_t> peek_len(const void* data, std::size_t data_len) const {
        bool ret        = false;
        std::size_t len = 0;
        do {
            try {
                if (data_len < kFrameSizeStrLen) break;
                auto ptr = static_cast<const std::uint8_t*>(data);
                len      = std::stoul(std::string(ptr, ptr + kFrameSizeStrLen));
                ret      = true;
            } catch (...) {
            }
        } while (0);
        return std::make_tuple(ret, len);
    }
    std::tuple<bool, std::string> encode_item(std::string key, std::string value) const {
        bool ret            = false;
        std::string ret_str = std::move(key);
        do {
            if (ret_str.find(kSplitChar) != std::string::npos) {
                break;
            }
            ret_str += kSplitChar;
            auto [bSuccess, len_str] = format_len(value.size());
            if (!bSuccess) break;
            ret_str += len_str;
            ret_str += value;
            if (ret_str.size() > kMaxPayloadSize) break;
            ret = true;
        } while (0);
        return std::make_tuple(ret, ret_str);
    }
    virtual std::tuple<bool, std::string> encode_imp() const          = 0;
    virtual bool decode_item(std::string_view key, std::string value) = 0;
    std::string parse_cache;
    std::size_t payload_size = 0;
};

struct forward_request_context : forward_context_interface {
    enum value_array_t : std::uint8_t
    {
        socks5_option_v,
        ssl_option_v,
        forward_protocal_v,
        dst_host_v,
        dst_service_v,
        route_path_v,
        value_array_t_num
    };
    constexpr static std::array<const char*, value_array_t_num> kValueNameArray = { "socks5_option",    "ssl_option",
                                                                                    "forward_protocal", "dst_host",
                                                                                    "dst_service",      "route_path" };
    inline static const std::unordered_map<std::string_view, value_array_t> kValueNameMap = []() {
        std::unordered_map<std::string_view, value_array_t> map;
        for (std::size_t i = 0; i < kValueNameArray.size(); ++i) {
            map[kValueNameArray[i]] = static_cast<value_array_t>(i);
        }
        return map;
    }();
    std::tuple<bool, std::string> encode_imp() const override {
        bool ret = true;
        std::string ret_str;
        for (std::size_t i = 0; i < value_array_t_num && ret; ++i) {
            value_array_t t = static_cast<value_array_t>(i);
            switch (t) {
            case socks5_option_v: {
                auto [encode_ret, encode_str] = encode_item(kValueNameArray[i], kForwardOptionArray[socks5_option]);
                if (!encode_ret) {
                    ret = false;
                    break;
                }
                ret_str += encode_str;
            } break;
            case ssl_option_v: {
                auto [encode_ret, encode_str] = encode_item(kValueNameArray[i], kForwardOptionArray[ssl_option]);
                if (!encode_ret) {
                    ret = false;
                    break;
                }
                ret_str += encode_str;
                break;
            }
            case forward_protocal_v: {
                auto [encode_ret, encode_str] =
                    encode_item(kValueNameArray[i], kForwardProtocalArray[forward_protocal]);
                if (!encode_ret) {
                    ret = false;
                    break;
                }
                ret_str += encode_str;
                break;
            }
            case dst_host_v: {
                auto [encode_ret, encode_str] = encode_item(kValueNameArray[i], dst_host);
                if (!encode_ret) {
                    ret = false;
                    break;
                }
                ret_str += encode_str;
                break;
            }
            case dst_service_v: {
                auto [encode_ret, encode_str] = encode_item(kValueNameArray[i], dst_service);
                if (!encode_ret) {
                    ret = false;
                    break;
                }
                ret_str += encode_str;
                break;
            }
            case route_path_v: {
                auto [encode_ret, encode_str] = encode_item(kValueNameArray[i], route_path);
                if (!encode_ret) {
                    ret = false;
                    break;
                }
                ret_str += encode_str;
                break;
            }
            default: break;
            }
        }

        return std::make_tuple(ret, ret_str);
    }
    bool decode_item(std::string_view key, std::string value) override {
        auto iter_value = kValueNameMap.find(key);
        // ignore invalid option
        if (iter_value == kValueNameMap.end()) return true;
        switch (iter_value->second) {
        case socks5_option_v: {
            auto iter = kForwardOptionMap.find(value);
            if (iter == kForwardOptionMap.end()) return false;
            socks5_option = iter->second;
        } break;
        case ssl_option_v: {
            auto iter = kForwardOptionMap.find(value);
            if (iter == kForwardOptionMap.end()) return false;
            ssl_option = iter->second;
            break;
        }
        case forward_protocal_v: {
            auto iter = kForwardProtocalMap.find(value);
            if (iter == kForwardProtocalMap.end()) return false;
            forward_protocal = iter->second;
            break;
        }
        case dst_host_v: {
            dst_host = std::move(value);
            break;
        }
        case dst_service_v: {
            dst_service = std::move(value);
            break;
        }
        case route_path_v: {
            route_path = std::move(value);
            break;
        }
        default: break;
        }
        return true;
    }
    forward_option socks5_option        = forward_optional_option;
    forward_option ssl_option           = forward_optional_option;
    forward_protocal_t forward_protocal = forward_websocket;
    std::string dst_host;
    std::string dst_service;
    std::string route_path;
};

struct forward_response_context : forward_context_interface {
    constexpr static std::int32_t kSuccessCode = 0;
    constexpr static std::int32_t kFailedCode  = 1;
    enum value_array_t : std::uint8_t
    {
        code_v,
        msg_v,
        value_array_t_num
    };
    constexpr static std::array<const char*, value_array_t_num> kValueNameArray = { "code", "msg" };

    inline static const std::unordered_map<std::string_view, value_array_t> kValueNameMap = []() {
        std::unordered_map<std::string_view, value_array_t> map;
        for (std::size_t i = 0; i < kValueNameArray.size(); ++i) {
            map[kValueNameArray[i]] = static_cast<value_array_t>(i);
        }
        return map;
    }();
    std::tuple<bool, std::string> encode_imp() const override {
        bool ret = true;
        std::string ret_str;
        for (std::size_t i = 0; i < value_array_t_num && ret; ++i) {
            value_array_t t = static_cast<value_array_t>(i);
            switch (t) {
            case code_v: {
                auto [encode_ret, encode_str] = encode_item(kValueNameArray[i], std::to_string(code));
                if (!encode_ret) {
                    ret = false;
                    break;
                }
                ret_str += encode_str;
            } break;
            case msg_v: {
                auto [encode_ret, encode_str] = encode_item(kValueNameArray[i], msg);
                if (!encode_ret) {
                    ret = false;
                    break;
                }
                ret_str += encode_str;
                break;
            }
            default: break;
            }
        }

        return std::make_tuple(ret, ret_str);
    }
    bool decode_item(std::string_view key, std::string value) override {
        auto iter = kValueNameMap.find(key);
        // ignore invalid option
        if (iter == kValueNameMap.end()) return true;
        switch (iter->second) {
        case code_v: {
            try {
                code = std::stoi(value);
            } catch (...) {
                return false;
            }

        } break;
        case msg_v: {
            msg = std::move(value);
            break;
        }
        default: break;
        }
        return true;
    }
    std::int32_t code = kSuccessCode;
    std::string msg;
};

} // namespace forward
} // namespace network