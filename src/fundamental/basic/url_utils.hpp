#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace Fundamental
{

static constexpr std::uint16_t HIGH_SURROGATE_START_CODEPOINT = ((std::uint16_t)0xD800);
static constexpr std::uint16_t HIGH_SURROGATE_END_CODEPOINT   = ((std::uint16_t)0xDBFF);
static constexpr std::uint16_t LOW_SURROGATE_START_CODEPOINT  = ((std::uint16_t)0xDC00);
static constexpr std::uint16_t LOW_SURROGATE_END_CODEPOINT    = ((std::uint16_t)0xDFFF);
static constexpr char UNICODE_BOGUS_CHAR_CODEPOINT            = '?';
inline std::int32_t HexDigit(char ch) {
    std::int32_t Result = 0;

    if (ch >= '0' && ch <= '9') {
        Result = ch - '0';
    } else if (ch >= 'a' && ch <= 'f') {
        Result = ch + 10 - 'a';
    } else if (ch >= 'A' && ch <= 'F') {
        Result = ch + 10 - 'A';
    } else {
        Result = 0;
    }

    return Result;
}

/** Is the provided Codepoint within the range of valid codepoints? */
inline constexpr bool IsValidCodepoint(const std::uint32_t Codepoint) {
    if ((Codepoint > 0x10FFFF) ||                       // No Unicode codepoints above 10FFFFh, (for now!)
        (Codepoint == 0xFFFE) || (Codepoint == 0xFFFF)) // illegal values.
    {
        return false;
    }
    return true;
}

/** Is the provided Codepoint within the range of the high-surrogates? */
inline bool IsHighSurrogate(const std::uint32_t Codepoint) {
    return Codepoint >= HIGH_SURROGATE_START_CODEPOINT && Codepoint <= HIGH_SURROGATE_END_CODEPOINT;
}

/** Is the provided Codepoint within the range of the low-surrogates? */
inline bool IsLowSurrogate(const std::uint32_t Codepoint) {
    return Codepoint >= LOW_SURROGATE_START_CODEPOINT && Codepoint <= LOW_SURROGATE_END_CODEPOINT;
}

template <typename BufferType, typename ToType>
std::int32_t Utf8FromCodepoint(std::uint32_t Codepoint,
                               BufferType OutputIterator,
                               std::uint32_t OutputIteratorByteSizeRemaining) {
    // Ensure we have at least one character in size to write
    if (OutputIteratorByteSizeRemaining < sizeof(ToType)) {
        return 0;
    }

    const BufferType OutputIteratorStartPosition = OutputIterator;

    if (!IsValidCodepoint(Codepoint)) {
        Codepoint = UNICODE_BOGUS_CHAR_CODEPOINT;
    } else if (IsHighSurrogate(Codepoint) ||
               IsLowSurrogate(
                   Codepoint)) // UTF-8 Characters are not allowed to encode codepoints in the surrogate pair range
    {
        Codepoint = UNICODE_BOGUS_CHAR_CODEPOINT;
    }

    // Do the encoding...
    if (Codepoint < 0x80) {
        *(OutputIterator++) = (ToType)Codepoint;
    } else if (Codepoint < 0x800) {
        if (OutputIteratorByteSizeRemaining >= 2) {
            *(OutputIterator++) = (ToType)((Codepoint >> 6) | 128 | 64);
            *(OutputIterator++) = (ToType)((Codepoint & 0x3F) | 128);
        }
    } else if (Codepoint < 0x10000) {
        if (OutputIteratorByteSizeRemaining >= 3) {
            *(OutputIterator++) = (ToType)((Codepoint >> 12) | 128 | 64 | 32);
            *(OutputIterator++) = (ToType)(((Codepoint >> 6) & 0x3F) | 128);
            *(OutputIterator++) = (ToType)((Codepoint & 0x3F) | 128);
        }
    } else {
        if (OutputIteratorByteSizeRemaining >= 4) {
            *(OutputIterator++) = (ToType)((Codepoint >> 18) | 128 | 64 | 32 | 16);
            *(OutputIterator++) = (ToType)(((Codepoint >> 12) & 0x3F) | 128);
            *(OutputIterator++) = (ToType)(((Codepoint >> 6) & 0x3F) | 128);
            *(OutputIterator++) = (ToType)((Codepoint & 0x3F) | 128);
        }
    }

    return static_cast<std::int32_t>(OutputIterator - OutputIteratorStartPosition);
}

inline std::string UrlEncode(const std::string& src) {
    std::string dst;
    dst.reserve(src.size());
    size_t length = src.length();
    for (size_t i = 0; i < length; i++) {
        if (isalnum((unsigned char)src[i]) || (src[i] == '-') || (src[i] == '_') || (src[i] == '.') || (src[i] == '~'))
            dst += src[i];
        else if (src[i] == ' ')
            dst += "%20";
        else {
            dst += '%';
            unsigned char x = (unsigned char)src[i] >> 4;
            x               = x > 9 ? x + 55 : x + 48;
            dst += x;
            x = (unsigned char)src[i] % 16;
            x = x > 9 ? x + 55 : x + 48;
            dst += x;
        }
    }
    return dst;
}

inline std::string UrlDecode(const std::string& EncodedString) {
    std::string Data;
    Data.reserve(EncodedString.size());

    for (std::size_t CharIdx = 0; CharIdx < EncodedString.size();) {
        if (EncodedString[CharIdx] == '%') {
            std::int32_t Value = 0;
            if (EncodedString[CharIdx + 1] == 'u') {
                if (CharIdx + 6 <= EncodedString.size()) {
                    // Treat all %uXXXX as code point
                    Value = HexDigit(EncodedString[CharIdx + 2]) << 12;
                    Value += HexDigit(EncodedString[CharIdx + 3]) << 8;
                    Value += HexDigit(EncodedString[CharIdx + 4]) << 4;
                    Value += HexDigit(EncodedString[CharIdx + 5]);
                    CharIdx += 6;

                    char Buffer[8]                  = { 0 };
                    char* BufferPtr                 = Buffer;
                    const std::int32_t Len          = 8;
                    const std::int32_t WrittenChars = Utf8FromCodepoint<char*, char>(Value, BufferPtr, Len);

                    Data.insert(Data.end(), Buffer, Buffer + WrittenChars);
                } else {
                    // Not enough in the buffer for valid decoding, skip it
                    CharIdx++;
                    continue;
                }
            } else if (CharIdx + 3 <= EncodedString.size()) {
                // Treat all %XX as straight byte
                Value = HexDigit(EncodedString[CharIdx + 1]) << 4;
                Value += HexDigit(EncodedString[CharIdx + 2]);
                CharIdx += 3;
                Data.push_back((std::uint8_t)(Value));
            } else {
                // Not enough in the buffer for valid decoding, skip it
                CharIdx++;
                continue;
            }
        } else {
            // Non escaped characters
            Data.push_back(EncodedString[CharIdx]);
            CharIdx++;
        }
    }

    return Data;
}
inline std::string CorrectApiSlash(std::string_view api) {
    if (api.empty()) return "/";
    if (api[0] != '/') return std::string("/") + std::string(api);
    return std::string(api);
}
} // namespace Fundamental