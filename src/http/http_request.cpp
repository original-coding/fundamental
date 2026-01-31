#include "http_request.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/string_utils.hpp"
namespace network::http
{

http_request::http_request(bool is_header_casesensitive) :
is_header_casesensitive_(is_header_casesensitive), state_(method_start) {
}

void http_request::reset_all() {
    state_ = method_start;
}

Fundamental::algorithm::range_set<std::size_t> http_request::get_bytes_range(std::size_t file_size) const {
    auto range_str = get_header("Range");
    Fundamental::algorithm::range_set<std::size_t> ret;
    constexpr std::size_t kDefaultStart = 0;
    do {

        if (range_str.empty()) break;
        auto ret_1 = Fundamental::StringSplit(range_str, '=');
        if (ret_1.size() != 2) break;
        if (ret_1[0] != "bytes") {
            throw std::invalid_argument(Fundamental::StringFormat("range only support bytes type"));
        }
        auto ret_ranges = Fundamental::StringSplit(ret_1[1], ',');
        if (ret_ranges.empty()) break;
        for (auto& r : ret_ranges) {
            auto s = r;
            Fundamental::StringTrimStart(s);
            Fundamental::StringTrimEnd(s);
            if (s.empty()) continue;
            auto split_range = Fundamental::StringSplit(s, '-');
            if (split_range.size() > 2) {
                throw std::invalid_argument(Fundamental::StringFormat("invalid range"));
            }
            std::vector<std::size_t> interge_ranges;
            for (auto& i : split_range) {
                try {
                    interge_ranges.emplace_back(std::stoull(i));
                } catch (const std::exception& e) {
                    throw std::invalid_argument(Fundamental::StringFormat("invalid range {}", e.what()));
                }
            }
            auto start = kDefaultStart;
            auto end   = file_size;
            if (s[0] == '-') {
                start = file_size >= interge_ranges[0] ? (file_size - interge_ranges[0]) : 0;
            } else if (s.back() == '-') {
                start = interge_ranges[0];
            } else {
                if (interge_ranges.size() != 2) {
                    throw std::invalid_argument(Fundamental::StringFormat("invalid range format"));
                }
                start = interge_ranges[0];
                if (interge_ranges[1] >= file_size) {
                    throw std::invalid_argument(Fundamental::StringFormat("invalid range overflow"));
                }
                end = interge_ranges[1] + 1;
            }
            if (start >= end) {
                throw std::invalid_argument(Fundamental::StringFormat("invalid range overflow"));
            }
            ret.range_emplace(start, end);
        }
    } while (0);
    if (ret.size() > 1) {
        throw std::invalid_argument(Fundamental::StringFormat("invalid ranges,only support single range"));
    }
    return ret;
}

std::optional<bool> http_request::Consume(char** ppInput, std::size_t* pLeftSize) {
    char input = *(*ppInput);
    if (state_ != body) { // Not body state,comsume one by one
        ++(*ppInput);
        --(*pLeftSize);
    }
    switch (state_) {
    case state::method_start:
        if (!IsChar(input) || IsCtl(input) || IsTspecial(input)) {
            return false;
        } else {
            state_ = state::method;
            method_.push_back(input);
            return std::nullopt;
        }
    case state::method:
        if (input == ' ') {
            state_ = state::uri;
            return std::nullopt;
        } else if (!IsChar(input) || IsCtl(input) || IsTspecial(input)) {
            return false;
        } else {
            method_.push_back(input);
            return std::nullopt;
        }
    case state::uri:
        if (input == ' ') {
            state_ = http_version_h;
            return std::nullopt;
        } else if (IsCtl(input)) {
            return false;
        } else {
            uri_.push_back(input);
            return std::nullopt;
        }
    case state::http_version_h:
        if (input == 'H') {
            state_ = state::http_version_t_1;
            return std::nullopt;
        } else {
            return false;
        }
    case state::http_version_t_1:
        if (input == 'T') {
            state_ = state::http_version_t_2;
            return std::nullopt;
        } else {
            return false;
        }
    case state::http_version_t_2:
        if (input == 'T') {
            state_ = state::http_version_p;
            return std::nullopt;
        } else {
            return false;
        }
    case state::http_version_p:
        if (input == 'P') {
            state_ = state::http_version_slash;
            return std::nullopt;
        } else {
            return false;
        }
    case state::http_version_slash:
        if (input == '/') {
            http_version_major_ = 0;
            http_version_minor_ = 0;
            state_              = state::http_version_major_start;
            return std::nullopt;
        } else {
            return false;
        }
    case state::http_version_major_start:
        if (IsDigit(input)) {
            http_version_major_ = http_version_major_ * 10 + input - '0';
            state_              = state::http_version_major;
            return std::nullopt;
        } else {
            return false;
        }
    case state::http_version_major:
        if (input == '.') {
            state_ = state::http_version_minor_start;
            return std::nullopt;
        } else if (IsDigit(input)) {
            http_version_major_ = http_version_major_ * 10 + input - '0';
            return std::nullopt;
        } else {
            return false;
        }
    case state::http_version_minor_start:
        if (IsDigit(input)) {
            http_version_minor_ = http_version_minor_ * 10 + input - '0';
            state_              = state::http_version_minor;
            return std::nullopt;
        } else {
            return false;
        }
    case state::http_version_minor:
        if (input == '\r') {
            state_ = state::expecting_newline_1;
            return std::nullopt;
        } else if (IsDigit(input)) {
            http_version_minor_ = http_version_minor_ * 10 + input - '0';
            return std::nullopt;
        } else {
            return false;
        }
    case state::expecting_newline_1:
        if (input == '\n') {
            state_ = state::header_line_start;
            return std::nullopt;
        } else {
            return false;
        }
    case state::header_line_start:
        if (input == '\r') {
            state_ = state::expecting_newline_3;
            return std::nullopt;
        } else if (!headers_.empty() && (input == ' ' || input == '\t')) {
            state_ = state::header_lws;
            return std::nullopt;
        } else if (!IsChar(input) || IsCtl(input) || IsTspecial(input)) {
            return false;
        } else {
            headers_.emplace_back();
            headers_.back().name.push_back(input);
            state_ = header_name;
            return std::nullopt;
        }
    case state::header_lws:
        if (input == '\r') {
            state_ = state::expecting_newline_2;
            return std::nullopt;
        } else if (input == ' ' || input == '\t') {
            return std::nullopt;
        } else if (IsCtl(input)) {
            return false;
        } else {
            state_ = state::header_value;
            headers_.back().value.push_back(input);
            return std::nullopt;
        }
    case state::header_name:
        if (input == ':') {
            state_ = state::space_before_header_value;
            return std::nullopt;
        } else if (!IsChar(input) || IsCtl(input) || IsTspecial(input)) {
            return false;
        } else {
            headers_.back().name.push_back(input);
            return std::nullopt;
        }
    case state::space_before_header_value:
        if (input == ' ') {
            state_ = state::header_value;
            return std::nullopt;
        } else {
            return false;
        }
    case state::header_value:
        if (input == '\r') {
            state_ = state::expecting_newline_2;
            return std::nullopt;
        } else if (IsCtl(input)) {
            return false;
        } else {
            headers_.back().value.push_back(input);
            return std::nullopt;
        }
    case state::expecting_newline_2:
        if (input == '\n') {
            state_ = state::header_line_start;
            return std::nullopt;
        } else {
            return false;
        }
    case state::expecting_newline_3: {
        contentTransferred_ = 0;
        auto result         = (input == '\n');
        if (!result) return false;
        if (isDataTransmissionFinishWhenParseAllHeader()) return result;
        // Ready to append body data
        state_ = state::body;
        return std::nullopt;
    }
    case state::body: {
        // Consume all size
        contentTransferred_ += (*pLeftSize);
        content_.append(*ppInput, *pLeftSize);
        (*ppInput) += (*pLeftSize);
        *pLeftSize = 0;
        if (isDataTransmissionFinishWhenParseAllHeader()) return true;
        return std::nullopt;
    }
    default: return false;
    }
}

bool http_request::IsChar(int c) {
    return c >= 0 && c <= 127;
}

bool http_request::IsCtl(int c) {
    return (c >= 0 && c <= 31) || (c == 127);
}

bool http_request::IsTspecial(int c) {
    switch (c) {
    case '(':
    case ')':
    case '<':
    case '>':
    case '@':
    case ',':
    case ';':
    case ':':
    case '\\':
    case '"':
    case '/':
    case '[':
    case ']':
    case '?':
    case '=':
    case '{':
    case '}':
    case ' ':
    case '\t': return true;
    default: return false;
    }
}

bool http_request::IsDigit(int c) {
    return c >= '0' && c <= '9';
}

const std::string& http_request::get_header(const std::string& name) const {
    auto it = headers_map_.end();
    if (!is_header_casesensitive_) {
        std::string lowerStr(name);
        Fundamental::StringToLower(lowerStr);
        it = headers_map_.find(lowerStr);
    } else {
        it = headers_map_.find(name);
    }
    if (it != headers_map_.end()) return it->second;

    return default_str_;
}

const std::vector<http_header>& http_request::get_all_headers() const {
    return headers_;
}

const std::string& http_request::get_query_param(const std::string& name) const {
    auto it = query_params_map_.find(name);
    if (it != query_params_map_.end()) return it->second;
    return default_str_;
}

void http_request::get_all_query_params(std::unordered_map<std::string, std::string>& outMap) const {
    outMap.clear();
    for (auto& it : query_params_map_) {
        outMap[it.first] = it.second;
    }
}

const std::string& http_request::get_body() const {
    return content_;
}

const std::string& http_request::get_ip() const {
    return ip_;
}

std::int32_t http_request::get_port() const {
    return port_;
}

void http_request::peek_body(std::string& body) {
    body = std::move(content_);
}
} // namespace network::http