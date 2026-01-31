#pragma once

#include <cctype>
#include <cwchar>
#include <iomanip>
#include <sstream>

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <stdlib.h>
#include <string>
#include <time.h>
#include <utility>
#include <vector>

namespace Fundamental
{
inline constexpr std::size_t StringHash(const char* str, std::size_t seed) {
    if (!str) return seed;
    std::size_t i = 0;
    while (std::size_t ch = static_cast<std::size_t>(str[i])) {
        seed = seed * 65599 + ch;
        ++i;
    }

    return seed;
}

/*
 * compute str hash helper
 */
template <typename First, typename... Rest>
inline constexpr std::size_t StringsHash(std::size_t seed, First first, Rest... rest) {
    seed = StringHash(first, seed);
    // Recusively iterate all levels
    if constexpr (sizeof...(Rest) > 0) {
        seed = StringsHash<Rest...>(seed, std::forward<Rest>(rest)...);
    }
    return seed;
}

// Type trait used for determine if T is c string or std::string
template <typename T>
struct IsStringTypeTrait : public std::disjunction<std::is_same<char*, typename std::decay_t<T>>,
                                                   std::is_same<const char*, typename std::decay_t<T>>,
                                                   std::is_same<std::string, typename std::decay_t<T>>> {};

// Helper to check if char ptr is empty
template <typename CharType>
static bool isCharPtrEmpty(const CharType* str) {
    if (str == nullptr) return true;

    if constexpr (std::is_same_v<CharType, char>) {
        if (std::strlen(str) == 0 || !str[0]) return true;
    } else if constexpr (std::is_same_v<CharType, wchar_t>) {
        if (std::wcslen(str) == 0 || !str[0]) return true;
    } else {
        // static_assert(false, "Char Type is not char or wchar_t.");
    }

    return false;
}

// Split a string into chunks at a separation character.
inline std::vector<std::string> StringSplit(const std::string& input, char sep) {
    std::vector<std::string> chunks;

    size_t offset(0);
    size_t pos(0);
    std::string chunk;
    while (pos != std::string::npos) {
        pos = input.find(sep, offset);

        if (pos == std::string::npos)
            chunk = input.substr(offset);
        else
            chunk = input.substr(offset, pos - offset);

        if (!chunk.empty()) chunks.push_back(chunk);
        offset = pos + 1;
    }
    return chunks;
}

// Split a string into chunks at a separation substring.
inline std::vector<std::string> StringSplit(const std::string& input, const std::string& sep) {
    std::vector<std::string> chunks;

    size_t offset(0);
    while (true) {
        size_t pos = input.find(sep, offset);

        if (pos == std::string::npos) {
            chunks.push_back(input.substr(offset));
            break;
        }

        chunks.push_back(input.substr(offset, pos - offset));
        offset = pos + sep.length();
    }
    return chunks;
}

inline std::vector<std::string> StringSplitIntoVector(const std::string& input,
                                                      const std::vector<std::string>& delim,
                                                      bool bCullEmpty = true) {
    std::vector<std::string> subStrs;
    if (input.size()) {
        std::size_t subStrBeginIndex = 0;
        for (std::size_t i = 0; i < input.size();) {
            std::size_t subStrEndIndex = std::string::npos;
            std::size_t delimLength    = 0;

            for (std::size_t delimIndex = 0; delimIndex < delim.size(); ++delimIndex) {
                delimLength = delim[delimIndex].size();
                if (std::strncmp(input.data() + i, delim[delimIndex].data(), delim[delimIndex].size()) == 0) {
                    subStrEndIndex = i;
                    break;
                }
            }

            if (subStrEndIndex != std::string::npos) {
                const std::size_t subStrLength = subStrEndIndex - subStrBeginIndex;
                if (!bCullEmpty || subStrLength > 0) {
                    subStrs.emplace_back(input.substr(subStrBeginIndex, subStrLength));
                }

                // Next substring begins at the end of the discovered delimiter.
                subStrBeginIndex = subStrEndIndex + delimLength;
                i                = subStrBeginIndex;
            } else {
                ++i;
            }
        }

        const size_t subStrLength = input.size() - subStrBeginIndex;
        if (!bCullEmpty || subStrLength > 0) {
            subStrs.emplace_back(input.substr(subStrBeginIndex, subStrLength));
        }
    }

    return subStrs;
}

// Checks if a string starts with a given prefix.
inline bool StringStartWith(const std::string& s, const std::string& potential_start) {
    size_t n = potential_start.size();

    if (s.size() < n) return false;

    for (size_t i = 0; i < n; ++i)
        if (s[i] != potential_start[i]) return false;

    return true;
}

// Checks if a string ends with a given suffix.
inline bool StringEndWith(const std::string& s, const std::string& potential_end) {
    size_t n  = potential_end.size();
    size_t sn = s.size();

    if (sn < n) return false;

    for (size_t i = 0; i < n; ++i)
        if (s[sn - i - 1] != potential_end[n - i - 1]) return false;

    return true;
}

inline void StringTrimStart(std::string& input) {
    std::size_t pos = 0;
    while (pos < input.size() && std::isspace(input[pos])) {
        ++pos;
    }

    if (pos == 0) return;

    input.erase(input.begin(), input.begin() + pos);
}

inline void StringTrimEnd(std::string& input) {
    if (input.empty()) return;
    std::size_t endIndex = input.size() - 1;
    while (endIndex > 0 && std::isspace(input[endIndex])) {
        --endIndex;
    }
    if (endIndex == input.size() - 1) return;
    input.erase(input.begin() + endIndex + 1, input.end());
}

inline void StringTrimStartAndEnd(std::string& input) {
    StringTrimEnd(input);
    StringTrimStart(input);
}

inline void StringToLower(std::string& input) {
    std::transform(input.begin(), input.end(), input.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
}

inline void StringToUpper(std::string& input) {
    std::transform(input.begin(), input.end(), input.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
}

/// replaces substrings within a given string.
inline std::string StringReplace(const std::string& input, const std::string& old, const std::string& with) {
    if (input.empty()) return input;

    std::string result(input);
    size_t offset(0);
    while (true) {
        size_t pos = result.find(old, offset);
        if (pos == std::string::npos) break;

        result.replace(pos, old.length(), with);
        offset = pos + with.length();
    }
    return result;
}

// Replaces characters within a given string.
inline std::string StringReplace(const std::string& input, char old, char with) {
    // added this function for consistency with replace substring
    std::string output(input);
    std::replace(output.begin(), output.end(), old, with);
    return output;
}

// Removes leading and trailing quotes if there are some.
// Returns true when it was a non-quoted string or valid quoted string before.
// Returns false for single quotes and when a quote was only found at one end.
inline bool StringRemoveQuotes(std::string& s) {
    size_t l = s.length();
    if (l == 0) return true;

    bool leading = s[0] == '\"';
    if (l == 1) return !leading; // one single quote

    bool trailing = s[l - 1] == '\"';
    if (leading != trailing) // quote one one side only
        return false;

    if (leading) s = s.substr(1, l - 2); // remove quotes on both sides
    return true;
}

inline bool StringRemoveFromEnd(std::string& s, const std::string& rmStr) {
    if (rmStr.empty()) return false;

    if (StringEndWith(s, rmStr)) {
        s.erase(s.size() - rmStr.size(), rmStr.size());
        return true;
    }

    return false;
}

inline bool StringIsNumeric(const std::string& input) {
    if (input.empty()) return false;

    std::size_t index = 0;
    if (input[index] == '-' || input[index] == '+') {
        if (input.size() == 1) return false;

        ++index;
    }

    bool bHasDot = false;
    while (index < input.size()) {
        if (input[index] == '.') {
            if (bHasDot) {
                return false;
            }
            bHasDot = true;
        } else if (!std::isdigit(input[index])) {
            return false;
        }

        ++index;
    }

    return true;
}

} // namespace Fundamental