// base64_utils.hpp
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Fundamental {
namespace internal {
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

enum Base64CoderType {
    kNormalBase64 = 0,
    kFSBase64     = 1,
};
constexpr internal::Base64Context kBase64Context[] = { { kBase64Char, kBase64DecodeTable.data(), '=' },
                                                       { kFSBase64Char, kFSBase64DecodeTable.data(), '+' } };
// Encode inputBuff to a base64 string.
template <Base64CoderType type = Base64CoderType::kNormalBase64>
inline std::string Base64Encode(const void* inputBuff, std::size_t buffSize) {
    std::string ret("");
    if (buffSize == 0) return ret;
    constexpr auto& context              = kBase64Context[type];
    auto buf                             = reinterpret_cast<const uint8_t*>(inputBuff);
    const std::size_t numOrig24BitValues = buffSize / 3;
    bool havePadding                     = buffSize > numOrig24BitValues * 3;      // if has remainder
    bool havePadding2                    = buffSize == numOrig24BitValues * 3 + 2; // if remainder = 2
    const std::size_t numResultBytes     = 4 * (numOrig24BitValues + havePadding); // final string's size
    ret.resize(numResultBytes);
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
    return ret;
}

// Decode a base64 string to binary buff
template <Base64CoderType type = Base64CoderType::kNormalBase64, typename T = std::string,
          typename = typename std::enable_if_t<
              std::disjunction_v<std::is_same<T, std::vector<std::uint8_t>>, std::is_same<T, std::string>>>>
inline bool Base64Decode(const std::string& originString, T& outContainer) {
    if (originString.empty()) return false;
    constexpr auto& context = kBase64Context[type];
    int k                   = 0;
    int paddingCount        = 0;
    auto buffSize           = originString.length();
    std::size_t retSize;
    const std::size_t jMax = buffSize - 3;
    retSize                = 3 * buffSize / 4;
    // std::string buff;
    outContainer.resize(retSize);
    // std::shared_ptr<std::uint8_t> retBuf(new std::uint8_t[retSize], std::default_delete<std::uint8_t[]>());
    for (std::size_t j = 0; j < jMax; j += 4) {
        char inTmp[4], outTmp[4];
        for (int i = 0; i < 4; ++i) {
            inTmp[i] = originString[i + j];
            if (inTmp[i] == context.paddingChar) ++paddingCount;
            outTmp[i] = context.decodeTable[(unsigned char)inTmp[i]];
            if ((outTmp[i] & 0x80) != 0)
                outTmp[i] = 0; // this happens only if there was an invalid character; pretend that it was 'A'
        }
        outContainer[k++] = (outTmp[0] << 2) | (outTmp[1] >> 4);
        outContainer[k++] = (outTmp[1] << 4) | (outTmp[2] >> 2);
        outContainer[k++] = (outTmp[2] << 6) | outTmp[3];
    }
    outContainer.resize(retSize - paddingCount);
    return true;
}

} // namespace Fundamental