#pragma once
#include "http_definitions.hpp"
#include "http_router.hpp"

#include <array>
#include <optional>

#include "fundamental/algorithm/range_set.hpp"
#include "fundamental/basic/string_utils.hpp"
#include "fundamental/basic/url_utils.hpp"

namespace network
{
namespace http
{
class http_connection;
/// request from remote,owned by a http connection
class http_request {
    friend class http_connection;

public:
    http_request(bool is_header_casesensitive = false);

    // Gets header value with the name, name is case-insensitive.
    const std::string& get_header(const std::string& name) const;
    // Gets all headers_
    const std::vector<http_header>& get_all_headers() const;
    // Gets body content
    const std::string& get_body() const;
    // Gets client's ip. It's ipv6 string if it's ipv6 type
    const std::string& get_ip() const;
    // Gets client's port
    std::int32_t get_port() const;
    // Get Body content and remove it from request
    void peek_body(std::string& body);
    // Gets query param with the name
    const std::string& get_query_param(const std::string& name) const;
    // Gets all query params, store in outMap.
    void get_all_query_params(std::unordered_map<std::string, std::string>& outMap) const;
    // Gets query str
    const std::string& get_query_str() const {
        return queryStr_;
    }
    // get http request method_
    const std::string& get_method_str() const {
        return method_;
    }
    // get http request Uri
    const std::string& get_uri() const {
        return uri_;
    }
    const std::string& get_pattern() const {
        return pattern_;
    }
    decltype(auto) get_method() const {
        return method_filter_;
    }
    /// Reset to initial parser state.
    void reset_all();
    // utils
    // throw when range is invalid
    Fundamental::algorithm::range_set<std::size_t> get_bytes_range(std::size_t file_size) const;

private:
    void handler_parse_success() {
        auto indexQ = uri_.find_first_of('?');
        pattern_    = uri_;
        if (indexQ != std::string::npos) {
            pattern_ = uri_.substr(0, indexQ);
        }

        method_filter_ = http_router::from_method_string(method_);
        queryStr_      = (indexQ != std::string::npos) ? uri_.substr(pattern_.length() + 1) : std::string();

        // Set header key-value to map.
        for (auto& h : headers_) {
            if (!is_header_casesensitive_) Fundamental::StringToLower(h.name);
            headers_map_[h.name] = h.value;
        }

        // Set query param.
        if (!queryStr_.empty()) {
            auto vecStr = Fundamental::StringSplit(queryStr_, '&');
            for (auto& it : vecStr) {
                auto vecPair = Fundamental::StringSplit(it, '=');
                if (vecPair.size() == 2) {
                    query_params_map_[vecPair[0]] = Fundamental::UrlDecode(vecPair[1]);
                }
            }
        }
    }

    /// Parse some data. The std::optional<bool> return value is true when a complete request
    /// has been parsed, false if the data is invalid, std::nullopt when more
    /// data is required. The InputIterator return value indicates how much of the
    /// input has been consumed.
    template <typename InputIterator>
    std::tuple<std::optional<bool>, InputIterator> Parse(InputIterator begin,
                                                         InputIterator end,
                                                         std::size_t totalSize) {
        auto leftSize = totalSize;
        while (begin != end) {
            std::optional<bool> result = Consume(&begin, &leftSize);
            if (result.has_value() /*result || !result*/) {
                if (result.value()) {
                    handler_parse_success();
                }
                return std::make_tuple(result, begin);
            }
        }
        return std::make_tuple(std::nullopt, begin);
    }
    bool isDataTransmissionFinishWhenParseAllHeader() {
        if (contentLength_ != kInvalidHttpContentLength) return contentTransferred_ >= contentLength_;
        for (auto& it : headers_) {
            // It have not decided case-sensitive or not,
            // So just compare "Content-Length" and "content_-length" is fine.
            if (it.name == "content-length" || it.name == "Content-Length") {
                char* end      = nullptr;
                contentLength_ = std::strtoull(it.value.c_str(), &end, 10);
                content_.reserve(contentLength_);
                return contentTransferred_ >= contentLength_;
            }
        }
        contentLength_ = 0;
        return contentTransferred_ >= contentLength_;
        // return false;
    }
    /// Handle the next character of input.
    std::optional<bool> Consume(char** ppInput, std::size_t* pLeftSize);

    /// Check if a byte is an HTTP character.
    static bool IsChar(int c);

    /// Check if a byte is an HTTP control character.
    static bool IsCtl(int c);

    /// Check if a byte is defined as an HTTP tspecial character.
    static bool IsTspecial(int c);

    /// Check if a byte is a digit.
    static bool IsDigit(int c);

private:
    //
    const bool is_header_casesensitive_ = false;
    /// The current state of the parser.
    enum state
    {
        method_start,
        method,
        uri,
        http_version_h,
        http_version_t_1,
        http_version_t_2,
        http_version_p,
        http_version_slash,
        http_version_major_start,
        http_version_major,
        http_version_minor_start,
        http_version_minor,
        expecting_newline_1,
        header_line_start,
        header_lws,
        header_name,
        space_before_header_value,
        header_value,
        expecting_newline_2,
        expecting_newline_3,
        body // parse body state
    } state_;
    /// Buffer for incoming data.
    std::array<char, 8192> buffer_;

    std::string method_;
    std::string uri_;
    int http_version_major_;
    int http_version_minor_;
    std::vector<http_header> headers_;
    std::string content_;
    std::size_t contentTransferred_ = 0;
    std::size_t contentLength_      = kInvalidHttpContentLength;
    std::string ip_;        // remote client's ip,if it's ipv6,than it's ipv6 string
    std::int32_t port_ = 0; // remote client's port
    // generate helper data
    std::unordered_map<std::string, std::string> query_params_map_;
    std::unordered_map<std::string, std::string> headers_map_;
    std::string pattern_;
    std::string queryStr_;
    MethodFilter method_filter_ = MethodFilter::HttpNone;
    const std::string default_str_;
};
} // namespace http
} // namespace network