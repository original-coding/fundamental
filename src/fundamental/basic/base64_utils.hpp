// base64_utils.hpp
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Fundamental
{
namespace internal
{
struct Base64Context {
    const char* encodeChatTable     = nullptr;
    const std::uint8_t* decodeTable = nullptr;
    char paddingChar                = '=';
};
} // namespace internal

static constexpr char kBase64Char[]   = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static constexpr char kFSBase64Char[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

inline constexpr std::array<std::uint8_t, 256> generateBase64DecodeTable(const char (&base64Chars)[65]) {
    std::array<std::uint8_t, 256> decodeTable {};
    for (std::size_t i = 0; i < 256; ++i) {
        decodeTable[i] = 0x80;
    }
    // why we can't use __builtin_memset here?
    // which will cause compile error ‘__builtin_memset(((void*)(& decodeTable.std::array<unsigned char,
    // 256>::_M_elems)), 128, 256)' is not a constant expression
    for (std::size_t i = 0; i < 64; ++i) {
        decodeTable[base64Chars[i]] = static_cast<std::uint8_t>(i);
    }
    return decodeTable;
}

static constexpr auto kBase64DecodeTable   = generateBase64DecodeTable(kBase64Char);
static constexpr auto kFSBase64DecodeTable = generateBase64DecodeTable(kFSBase64Char);

enum Base64CoderType
{
    kNormalBase64 = 0,
    kFSBase64     = 1,
};
constexpr internal::Base64Context kBase64Context[] = { { kBase64Char, kBase64DecodeTable.data(), '=' },
                                                       { kFSBase64Char, kFSBase64DecodeTable.data(), '+' } };
// Encode inputBuff to a base64 string.
template <Base64CoderType type = Base64CoderType::kNormalBase64>
inline bool Base64Encode(const void* inputBuff,
                         std::size_t buffSize,
                         void* output_buf,
                         std::size_t output_buf_size,
                         std::size_t& out_size) {
    do {
        out_size = 0;
        if (buffSize == 0) break;
        constexpr auto& context              = kBase64Context[type];
        auto buf                             = reinterpret_cast<const uint8_t*>(inputBuff);
        const std::size_t numOrig24BitValues = buffSize / 3;
        bool havePadding                     = buffSize > numOrig24BitValues * 3;      // if has remainder
        bool havePadding2                    = buffSize == numOrig24BitValues * 3 + 2; // if remainder = 2
        const std::size_t numResultBytes     = 4 * (numOrig24BitValues + havePadding); // final string's size
        if (output_buf_size < numResultBytes) return false;
        out_size = numResultBytes;
        auto ret = static_cast<char*>(output_buf);
        std::size_t i;
        for (i = 0; i < numOrig24BitValues; ++i) {
            ret[4 * i + 0] = context.encodeChatTable[(buf[3 * i] >> 2) & 0x3F];
            ret[4 * i + 1] = context.encodeChatTable[(((buf[3 * i] & 0x3) << 4) | (buf[3 * i + 1] >> 4)) & 0x3F];
            ret[4 * i + 2] = context.encodeChatTable[((buf[3 * i + 1] << 2) | (buf[3 * i + 2] >> 6)) & 0x3F];
            ret[4 * i + 3] = context.encodeChatTable[buf[3 * i + 2] & 0x3F];
        }

        // remainder is 1 need append two '='
        // remainder is 2 need append one '='
        if (havePadding) {
            ret[4 * i + 0] = context.encodeChatTable[(buf[3 * i] >> 2) & 0x3F];
            if (havePadding2) {
                ret[4 * i + 1] = context.encodeChatTable[(((buf[3 * i] & 0x3) << 4) | (buf[3 * i + 1] >> 4)) & 0x3F];
                ret[4 * i + 2] = context.encodeChatTable[(buf[3 * i + 1] << 2) & 0x3F];
            } else {
                ret[4 * i + 1] = context.encodeChatTable[((buf[3 * i] & 0x3) << 4) & 0x3F];
                ret[4 * i + 2] = context.paddingChar;
            }
            ret[4 * i + 3] = context.paddingChar;
        }
    } while (0);
    return true;
}

// Encode inputBuff to a base64 string.
template <Base64CoderType type = Base64CoderType::kNormalBase64>
inline std::string Base64Encode(const void* inputBuff, std::size_t buffSize) {
    std::string ret;
    ret.resize((buffSize + 2) / 3 * 4);
    std::size_t final_size = 0;
    Base64Encode<type>(inputBuff, buffSize, ret.data(), ret.size(), final_size);
    return ret;
}

// Decode a base64 string to  buff
template <Base64CoderType type = Base64CoderType::kNormalBase64>
inline bool Base64Decode(const void* inputBuff,
                         std::size_t buffSize,
                         void* output_buf,
                         std::size_t output_buf_size,
                         std::size_t& out_size) {
    if (buffSize == 0 || (buffSize % 4) != 0) return false;
    const char* p_inputBuff  = static_cast<const char*>(inputBuff);
    constexpr auto& context  = kBase64Context[type];
    std::size_t paddingCount = 0;
    out_size                 = 3 * buffSize / 4;
    {
        if (p_inputBuff[buffSize - 1] == context.paddingChar) {
            ++paddingCount;
            if (p_inputBuff[buffSize - 2] == context.paddingChar) ++paddingCount;
        }
    }
    out_size -= paddingCount;
    // assert return buffer size
    if (output_buf_size < out_size) return false;
    int k         = 0;
    std::size_t j = 0;
    char inTmp[4], outTmp[4];
    char* p_output_buf = static_cast<char*>(output_buf);
    if (buffSize > 4) { //
        const std::size_t jMax = buffSize - 7;
        for (j = 0; j < jMax; j += 4) {
            for (int i = 0; i < 4; ++i) {
                inTmp[i]  = p_inputBuff[i + j];
                outTmp[i] = context.decodeTable[(unsigned char)inTmp[i]];
                if ((outTmp[i] & 0x80) != 0) {
                    // find invalid character
                    return false;
                }
            }
            p_output_buf[k++] = (outTmp[0] << 2) | (outTmp[1] >> 4);
            p_output_buf[k++] = (outTmp[1] << 4) | (outTmp[2] >> 2);
            p_output_buf[k++] = (outTmp[2] << 6) | outTmp[3];
        }
    }
    for (std::size_t i = 0; i < 4 - paddingCount; ++i) {
        inTmp[i]  = p_inputBuff[i + j];
        outTmp[i] = context.decodeTable[(unsigned char)inTmp[i]];
        if ((outTmp[i] & 0x80) != 0) {
            // find invalid character
            return false;
        }
    }
    p_output_buf[k++] = (outTmp[0] << 2) | (outTmp[1] >> 4);
    // paddingCount = 0 or 1
    if (paddingCount < 2) {
        p_output_buf[k++] = (outTmp[1] << 4) | (outTmp[2] >> 2);
    }
    // paddingCount = 0
    if (paddingCount == 0) {
        p_output_buf[k++] = (outTmp[2] << 6) | outTmp[3];
    }

    return true;
}

// Decode a base64 string to binary buff
template <Base64CoderType type = Base64CoderType::kNormalBase64,
          typename T           = std::string,
          typename             = typename std::enable_if_t<
                          std::disjunction_v<std::is_same<T, std::vector<std::uint8_t>>, std::is_same<T, std::string>>>>
inline bool Base64Decode(const std::string& originString, T& outContainer) {
    if (originString.empty()) return false;
    std::size_t retSize = 3 * originString.size() / 4;
    outContainer.resize(retSize);
    if (!Base64Decode<type>(originString.data(), originString.size(), outContainer.data(), outContainer.size(),
                            retSize)) {
        return false;
    }
    outContainer.resize(retSize);
    return true;
}

} // namespace Fundamental