

#include "ws_common.hpp"

#include <cstdlib>
#include <cstring>

#include "fundamental/basic/base64_utils.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/md5_utils.hpp"
#include "fundamental/basic/string_utils.hpp"
#include "fundamental/basic/url_utils.hpp"

namespace network
{
namespace websocket
{

std::string ws_utils::generateSessionKey(void* ptr) {
    auto value   = reinterpret_cast<std::intptr_t>(ptr);
    auto md5_str = Fundamental::CryptMD5(&value, sizeof(value));
    return Fundamental::Base64Encode(md5_str.data(), md5_str.size());
}

void ws_utils::ws_sha1_init(ws_sha1_ctx* context) {
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}
union char64long16 {
    unsigned char c[64];
    uint32_t l[16];
};
#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))
static uint32_t blk0(union char64long16* block, int i) {
/* Forrest: SHA expect BIG_ENDIAN, swap if LITTLE_ENDIAN */
#if BYTE_ORDER == LITTLE_ENDIAN
    block->l[i] = (rol(block->l[i], 24) & 0xFF00FF00) | (rol(block->l[i], 8) & 0x00FF00FF);
#endif
    return block->l[i];
}

/* Avoid redefine warning (ARM /usr/include/sys/ucontext.h define R0~R4) */
#undef blk
#undef R0
#undef R1
#undef R2
#undef R3
#undef R4

#define blk(i)                                                                                                         \
    (block->l[i & 15] =                                                                                                \
         rol(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15] ^ block->l[(i + 2) & 15] ^ block->l[i & 15], 1))

#define R0(v, w, x, y, z, i)                                                                                           \
    z += ((w & (x ^ y)) ^ y) + blk0(block, i) + 0x5A827999 + rol(v, 5);                                                \
    w = rol(w, 30);
#define R1(v, w, x, y, z, i)                                                                                           \
    z += ((w & (x ^ y)) ^ y) + blk(i) + 0x5A827999 + rol(v, 5);                                                        \
    w = rol(w, 30);
#define R2(v, w, x, y, z, i)                                                                                           \
    z += (w ^ x ^ y) + blk(i) + 0x6ED9EBA1 + rol(v, 5);                                                                \
    w = rol(w, 30);
#define R3(v, w, x, y, z, i)                                                                                           \
    z += (((w | x) & y) | (w & x)) + blk(i) + 0x8F1BBCDC + rol(v, 5);                                                  \
    w = rol(w, 30);
#define R4(v, w, x, y, z, i)                                                                                           \
    z += (w ^ x ^ y) + blk(i) + 0xCA62C1D6 + rol(v, 5);                                                                \
    w = rol(w, 30);

void ws_utils::ws_sha1_transform(uint32_t state[5], const unsigned char buffer[64]) {
    uint32_t a, b, c, d, e;
    union char64long16 block[1];

    memcpy(block, buffer, 64);
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    R0(a, b, c, d, e, 0);
    R0(e, a, b, c, d, 1);
    R0(d, e, a, b, c, 2);
    R0(c, d, e, a, b, 3);
    R0(b, c, d, e, a, 4);
    R0(a, b, c, d, e, 5);
    R0(e, a, b, c, d, 6);
    R0(d, e, a, b, c, 7);
    R0(c, d, e, a, b, 8);
    R0(b, c, d, e, a, 9);
    R0(a, b, c, d, e, 10);
    R0(e, a, b, c, d, 11);
    R0(d, e, a, b, c, 12);
    R0(c, d, e, a, b, 13);
    R0(b, c, d, e, a, 14);
    R0(a, b, c, d, e, 15);
    R1(e, a, b, c, d, 16);
    R1(d, e, a, b, c, 17);
    R1(c, d, e, a, b, 18);
    R1(b, c, d, e, a, 19);
    R2(a, b, c, d, e, 20);
    R2(e, a, b, c, d, 21);
    R2(d, e, a, b, c, 22);
    R2(c, d, e, a, b, 23);
    R2(b, c, d, e, a, 24);
    R2(a, b, c, d, e, 25);
    R2(e, a, b, c, d, 26);
    R2(d, e, a, b, c, 27);
    R2(c, d, e, a, b, 28);
    R2(b, c, d, e, a, 29);
    R2(a, b, c, d, e, 30);
    R2(e, a, b, c, d, 31);
    R2(d, e, a, b, c, 32);
    R2(c, d, e, a, b, 33);
    R2(b, c, d, e, a, 34);
    R2(a, b, c, d, e, 35);
    R2(e, a, b, c, d, 36);
    R2(d, e, a, b, c, 37);
    R2(c, d, e, a, b, 38);
    R2(b, c, d, e, a, 39);
    R3(a, b, c, d, e, 40);
    R3(e, a, b, c, d, 41);
    R3(d, e, a, b, c, 42);
    R3(c, d, e, a, b, 43);
    R3(b, c, d, e, a, 44);
    R3(a, b, c, d, e, 45);
    R3(e, a, b, c, d, 46);
    R3(d, e, a, b, c, 47);
    R3(c, d, e, a, b, 48);
    R3(b, c, d, e, a, 49);
    R3(a, b, c, d, e, 50);
    R3(e, a, b, c, d, 51);
    R3(d, e, a, b, c, 52);
    R3(c, d, e, a, b, 53);
    R3(b, c, d, e, a, 54);
    R3(a, b, c, d, e, 55);
    R3(e, a, b, c, d, 56);
    R3(d, e, a, b, c, 57);
    R3(c, d, e, a, b, 58);
    R3(b, c, d, e, a, 59);
    R4(a, b, c, d, e, 60);
    R4(e, a, b, c, d, 61);
    R4(d, e, a, b, c, 62);
    R4(c, d, e, a, b, 63);
    R4(b, c, d, e, a, 64);
    R4(a, b, c, d, e, 65);
    R4(e, a, b, c, d, 66);
    R4(d, e, a, b, c, 67);
    R4(c, d, e, a, b, 68);
    R4(b, c, d, e, a, 69);
    R4(a, b, c, d, e, 70);
    R4(e, a, b, c, d, 71);
    R4(d, e, a, b, c, 72);
    R4(c, d, e, a, b, 73);
    R4(b, c, d, e, a, 74);
    R4(a, b, c, d, e, 75);
    R4(e, a, b, c, d, 76);
    R4(d, e, a, b, c, 77);
    R4(c, d, e, a, b, 78);
    R4(b, c, d, e, a, 79);
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    /* Erase working structures. The order of operations is important,
     * used to ensure that compiler doesn't optimize those out. */
    memset(block, 0, sizeof(block));
    a = b = c = d = e = 0;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
}
void ws_utils::ws_sha1_update(ws_sha1_ctx* context, const unsigned char* data, size_t len) {
    size_t i, j;

    j = context->count[0];
    if ((context->count[0] += (uint32_t)len << 3) < j) context->count[1]++;
    context->count[1] += (uint32_t)(len >> 29);
    j = (j >> 3) & 63;
    if ((j + len) > 63) {
        memcpy(&context->buffer[j], data, (i = 64 - j));
        ws_sha1_transform(context->state, context->buffer);
        for (; i + 63 < len; i += 64) {
            ws_sha1_transform(context->state, &data[i]);
        }
        j = 0;
    } else
        i = 0;
    memcpy(&context->buffer[j], &data[i], len - i);
}

void ws_utils::ws_sha1_final(unsigned char digest[20], ws_sha1_ctx* context) {
    unsigned i;
    unsigned char finalcount[8], c;

    for (i = 0; i < 8; i++) {
        finalcount[i] = (unsigned char)((context->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    }
    c = 0200;
    ws_sha1_update(context, &c, 1);
    while ((context->count[0] & 504) != 448) {
        c = 0000;
        ws_sha1_update(context, &c, 1);
    }
    ws_sha1_update(context, finalcount, 8);
    for (i = 0; i < 20; i++) {
        digest[i] = (unsigned char)((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
    memset(context, '\0', sizeof(*context));
    memset(&finalcount, '\0', sizeof(finalcount));
}

void ws_utils::ws_hmac_sha1(const unsigned char* key,
                            size_t keylen,
                            const unsigned char* data,
                            size_t datalen,
                            unsigned char out[20]) {
    ws_sha1_ctx ctx;
    unsigned char buf1[64], buf2[64], tmp_key[20], i;

    if (keylen > sizeof(buf1)) {
        ws_sha1_init(&ctx);
        ws_sha1_update(&ctx, key, keylen);
        ws_sha1_final(tmp_key, &ctx);
        key    = tmp_key;
        keylen = sizeof(tmp_key);
    }

    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));
    memcpy(buf1, key, keylen);
    memcpy(buf2, key, keylen);

    for (i = 0; i < sizeof(buf1); i++) {
        buf1[i] ^= 0x36;
        buf2[i] ^= 0x5c;
    }

    ws_sha1_init(&ctx);
    ws_sha1_update(&ctx, buf1, sizeof(buf1));
    ws_sha1_update(&ctx, data, datalen);
    ws_sha1_final(out, &ctx);

    ws_sha1_init(&ctx);
    ws_sha1_update(&ctx, buf2, sizeof(buf2));
    ws_sha1_update(&ctx, out, 20);
    ws_sha1_final(out, &ctx);
}

std::string ws_utils::generateServerAcceptKey(const std::string& sesion_key) {
    unsigned char sha[20];
    ws_sha1_ctx sha_ctx;
    ws_sha1_init(&sha_ctx);
    ws_sha1_update(&sha_ctx, (unsigned char*)sesion_key.c_str(), sesion_key.size());
    ws_sha1_update(&sha_ctx, (unsigned char*)sha1_get_guid(), 36);
    ws_sha1_final(sha, &sha_ctx);
    return Fundamental::Base64Encode(sha, sizeof(sha));
}

std::tuple<http_handler_context::parse_status, std::size_t> http_handler_context::parse(const void* data,
                                                                                        std::size_t len) {
    auto status  = parse_status::parse_failed;
    auto ret_len = len;
    do {
        if (len == 0) {
            status = parse_status::parse_need_more_data;
            break;
        }
        if (len + parse_cache.size() >= kMaxHttpCacheSize) {
            FERR("http head size {} overflow {}", len + parse_cache.size(), kMaxHttpCacheSize);
            break;
        }
        auto old_pos = parse_cache.size();
        auto ptr     = static_cast<const std::uint8_t*>(data);
        parse_cache.insert(parse_cache.end(), ptr, ptr + len);
        auto pos = parse_cache.find(kHttpHeadEnd);
        if (pos == std::string::npos) {
            status = parse_status::parse_need_more_data;
            break;
        }
        pos += strlen(kHttpHeadEnd);
        ret_len       = pos - old_pos;
        auto line_len = strlen(kHttpLineSplit);

        auto kHeadersSplitLen = strlen(kHeaderSpace);
        parse_cache.resize(pos - line_len);
        // parse head

        std::size_t parse_pos = 0;
        auto next_line_pos    = parse_cache.find(kHttpLineSplit);
        if (next_line_pos == std::string::npos) {
            goto HTTP_PARSE_FAILED;
        }
        {
            auto head_pos = parse_cache.find(kHeadSpace, parse_pos);
            if (head_pos == std::string::npos) {
                goto HTTP_PARSE_FAILED;
            }
            head1     = parse_cache.substr(parse_pos, head_pos - parse_pos);
            parse_pos = head_pos + 1;
            head_pos  = parse_cache.find(kHeadSpace, parse_pos);
            if (head_pos == std::string::npos) {
                goto HTTP_PARSE_FAILED;
            }
            head2     = parse_cache.substr(parse_pos, head_pos - parse_pos);
            parse_pos = head_pos + 1;
            if (parse_pos >= next_line_pos) {
                goto HTTP_PARSE_FAILED;
            }
            head3     = parse_cache.substr(parse_pos, next_line_pos - parse_pos);
            parse_pos = next_line_pos + line_len;
        }
        // parse headers
        while (parse_pos < parse_cache.size()) {
            next_line_pos = parse_cache.find(kHttpLineSplit, parse_pos);
            if (next_line_pos == std::string::npos) {
                goto HTTP_PARSE_FAILED;
            }
            auto key_pos = parse_cache.find(kHeaderSpace, parse_pos);
            if (key_pos == std::string::npos) {
                goto HTTP_PARSE_FAILED;
            }
            auto value_pos = key_pos + kHeadersSplitLen;
            if (value_pos >= next_line_pos) {
                goto HTTP_PARSE_FAILED;
            }
            auto key   = parse_cache.substr(parse_pos, key_pos - parse_pos);
            auto value = parse_cache.substr(value_pos, next_line_pos - value_pos);
            headers.emplace(std::move(key), std::move(value));
            parse_pos = next_line_pos + line_len;
        }
        FDEBUG("\n{}", parse_cache);
        parse_cache.clear();
        parse_cache.shrink_to_fit();
        status = parse_status::parse_success;
        break;
    HTTP_PARSE_FAILED:
        FDEBUG("invalid http content data {}", parse_cache);
    } while (0);
    return std::make_tuple(status, ret_len);
}

std::string http_handler_context::encode() const {
    std::string ret;
    ret += head1;
    ret += kHeadSpace;
    ret += head2;
    ret += kHeadSpace;
    ret += head3;
    ret += kHttpLineSplit;
    for (auto& item : headers) {
        ret += item.first;
        ret += kHeaderSpace;
        ret += item.second;
        ret += kHttpLineSplit;
    }
    ret += kHttpLineSplit;
    return ret;
}

std::string http_handler_context::default_error_response() {
    return std::string(
        "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"WebSocket Access\"\r\nContent-Length: 0\r\n\r\n");
}

std::tuple<std::string, std::unordered_map<std::string, std::string>> http_handler_context::parse_query_params(
    const std::string& uri) {
    std::string request_uri = uri;
    std::unordered_map<std::string, std::string> query_params;
    auto indexQ = request_uri.find_first_of('?');
    if (indexQ != std::string::npos) {
        request_uri = uri.substr(0, indexQ);
    }
    auto queryStr_ = (indexQ != std::string::npos) ? uri.substr(request_uri.length() + 1) : std::string();

    if (!queryStr_.empty()) {
        auto vecStr = Fundamental::StringSplit(queryStr_, '&');
        for (auto& it : vecStr) {
            auto vecPair = Fundamental::StringSplit(it, '=');
            if (vecPair.size() == 2) {
                query_params[vecPair[0]] = Fundamental::UrlDecode(vecPair[1]);
            }
        }
    }
    return std::make_tuple(request_uri, query_params);
}

} // namespace websocket
} // namespace network