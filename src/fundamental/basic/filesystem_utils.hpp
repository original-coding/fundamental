#pragma once
#include "cxx_config_include.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace Fundamental::fs
{

inline void RemoveExpiredFiles(std::string_view dir_path,
                               std::string_view pattern,
                               std::int64_t expiredSec,
                               bool recursive = false) {
    std::regex filePattern(pattern.data(), pattern.length());
    auto now = std::chrono::system_clock::now();
    std_fs::path directory(dir_path);
    std::vector<std::string> subdirPaths;
    try {
        for (const auto& entry : std_fs::directory_iterator(directory)) {
            if (std_fs::is_regular_file(entry.status())) {
                const auto& path     = entry.path();
                const auto& filename = path.filename().string();
                if (std::regex_match(filename, filePattern)) {
                    auto ftime    = std_fs::last_write_time(path);
                    auto fileTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - std_fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                    auto fileAge = std::chrono::duration_cast<std::chrono::seconds>(now - fileTime).count();
                    if (fileAge > expiredSec) {
                        std_fs::remove(path);
                    }
                }
            }
            if (recursive && std_fs::is_directory(entry.status())) {
                subdirPaths.push_back(entry.path().string());
            }
        }
    } catch (const std::exception& e) {
#ifdef DEBUG
        std::cerr << "RemoveExpiredFiles " << dir_path << " catch error:" << e.what() << std::endl;
#endif
    }

    for (auto& item : subdirPaths)
        RemoveExpiredFiles(item, pattern, expiredSec, true);
}
template <typename T,
          typename = typename std::enable_if_t<
              std::disjunction_v<std::is_same<T, std::vector<std::uint8_t>>, std::is_same<T, std::string>>>>
inline bool ReadFile(std::string_view path,
                     T& output,
                     std::uint64_t offset        = 0,
                     std::uint64_t max_read_size = std::numeric_limits<std::uint64_t>::max()) {
    // Always read as binary.
    std::ifstream file(std::string(path), std::ios::binary);
    if (file) {
        // Get the lengthInBytes in bytes
        file.seekg(0, file.end);
        auto lengthInBytes = static_cast<decltype(max_read_size)>(file.tellg());
        if (offset >= lengthInBytes) {
            return false;
        }
        file.seekg(offset);
        if (lengthInBytes > max_read_size) lengthInBytes = max_read_size;
        output.resize(lengthInBytes);
        file.read(reinterpret_cast<char*>(output.data()), lengthInBytes);
        auto read_cnt = static_cast<decltype(lengthInBytes)>(file.gcount());
        output.resize(read_cnt);
        file.close();
        return true;
    } else {
#ifdef DEBUG
        std::cerr << "can't read from " << path << std::endl;
#endif
    }

    return false;
}

inline bool WriteFile(std::string_view path, const void* data, std::size_t len, bool override = true) {
    if (path.empty()) return false;
    // App mode means append mode, trunc mode means overwrite
    std::ios_base::openmode mode = override ? std::ios::trunc : std::ios::app;
    mode |= (std::ios::binary | std::ios::out);

    std::ofstream file(std::string(path), mode);
    if (file) {
        file.write(reinterpret_cast<const char*>(data), len);
        file.flush();
        file.close();
        return true;
    } else {
#ifdef DEBUG
        std::cerr << "can't write to " << path << std::endl;
#endif
    }

    return false;
}

inline bool SwitchToProgramDir(const std::string& argv0) {
    try {
        std_fs::current_path(std_fs::path(argv0).parent_path());
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

inline bool CreateHardLinkPair(std::filesystem::path p1,
                               std::filesystem::path p2,
                               bool create_when_not_existed = false) {
    bool p1_existed = std::filesystem::exists(p1);
    bool p2_existed = std::filesystem::exists(p2);
    try {
        if (!p1_existed && !p2_existed) {
            if (!create_when_not_existed) {
                return false;
            }
            std::ofstream file(p2, std::ios::out);
            p2_existed = true;
        }
        if (!p1_existed) {
            std::filesystem::create_hard_link(p2, p1);
        } else if (!p2_existed) {
            std::filesystem::create_hard_link(p1, p2);
        }
        return std::filesystem::equivalent(p1, p2);
    } catch (...) {
        return false;
    }
    return true;
}
} // namespace Fundamental::fs