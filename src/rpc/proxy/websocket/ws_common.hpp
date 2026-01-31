#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>
namespace network
{
namespace websocket
{
constexpr static std::uint8_t kWsMagicNum = 'G';

struct ws_utils {
    static std::string generateSessionKey(void* ptr = nullptr);
    static std::string generateServerAcceptKey(const std::string& sesion_key);
    // sha1 handle
    static const char* sha1_get_guid() {
        return "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    }
    typedef struct {
        uint32_t state[5];
        uint32_t count[2];
        unsigned char buffer[64];
    } ws_sha1_ctx;
    static void ws_sha1_init(ws_sha1_ctx*);
    static void ws_sha1_update(ws_sha1_ctx*, const unsigned char* data, size_t len);
    static void ws_sha1_final(unsigned char digest[20], ws_sha1_ctx*);
    static void ws_hmac_sha1(const unsigned char* key,
                             size_t key_len,
                             const unsigned char* text,
                             size_t text_len,
                             unsigned char out[20]);
    static void ws_sha1_transform(uint32_t state[5], const unsigned char buffer[64]);
};
/*
GET / HTTP/1.1
Host: example.com
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
*/
/*
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: [计算出的密钥]
*/

struct http_handler_context {
    constexpr static std::size_t kMaxHttpCacheSize       = 64 * 1024;
    constexpr static const char* kHeadSpace              = " ";
    constexpr static const char* kHeaderSpace            = ": ";
    constexpr static const char* kHttpLineSplit          = "\r\n";
    constexpr static const char* kHttpHeadEnd            = "\r\n\r\n";
    constexpr static const char* kHttpVersion            = "HTTP/1.1";
    constexpr static const char* kWebsocketMethod        = "GET";
    constexpr static const char* kWebsocketSuccessCode   = "101";
    constexpr static const char* kWebsocketSuccessStr    = "Switching Protocols";
    constexpr static const char* kHttpUpgradeStr         = "Upgrade";
    constexpr static const char* kHttpUpgradeValueStr    = "upgrade";
    constexpr static const char* kHttpWebsocketStr       = "websocket";
    constexpr static const char* kWsForwardDepthFlagName = "depth";
    constexpr static std::size_t kWsMaxForwardCntLimit   = 5;
    // ip[:port]
    constexpr static const char* kHttpHost                = "Host";
    constexpr static const char* kHttpConnection          = "Connection";
    constexpr static const char* kWebsocketVersion        = "13";
    constexpr static const char* kWebsocketRequestKey     = "Sec-WebSocket-Key";
    constexpr static const char* kWebsocketRequestVersion = "Sec-WebSocket-Version";
    constexpr static const char* kWebsocketResponseAccept = "Sec-WebSocket-Accept";
    enum parse_status
    {
        parse_need_more_data,
        parse_success,
        parse_failed,
    };
    /*
        This function will copy data from the input data. If parsing completes, it will return the number of unprocessed
       bytes.
    */
    std::tuple<parse_status, std::size_t> parse(const void* data, std::size_t len);
    std::string encode() const;
    static std::string default_error_response();
    static std::tuple<std::string, std::unordered_map<std::string, std::string>> parse_query_params(
        const std::string& uri);
    std::string head1;
    std::string head2;
    std::string head3;
    std::unordered_map<std::string, std::string> headers;
    std::string parse_cache;
};

} // namespace websocket
} // namespace network