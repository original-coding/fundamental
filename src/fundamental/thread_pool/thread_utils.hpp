#pragma once

#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace Fundamental
{
namespace internal
{
// parse cpuset.cpus format(eg. "0-3,5")
inline int parse_cpu_set(const std::string& cpuset) {
    int count = 0;
    std::stringstream ss(cpuset);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t dash = item.find('-');
        if (dash != std::string::npos) {
            int start = std::stoi(item.substr(0, dash));
            int end   = std::stoi(item.substr(dash + 1));
            count += end - start + 1;
        } else {
            if (!item.empty()) {
                count++;
            }
        }
    }
    return count;
}

inline int get_cpu_count() {
    // 1. check cpuset
    std::ifstream cpuset_file("/sys/fs/cgroup/cpuset/cpuset.cpus");
    if (cpuset_file) {
        std::string cpuset;
        if (std::getline(cpuset_file, cpuset)) {
            if (!cpuset.empty() && cpuset != "0") {
                return parse_cpu_set(cpuset);
            }
        }
    }

    // 2. check CPU quota and period
    // 2.1 cgroup v1
    std::ifstream quota_file("/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us");
    std::ifstream period_file("/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us");
    if (quota_file && period_file) {
        int64_t quota;
        int64_t period;
        quota_file >> quota;
        period_file >> period;
        if (quota > 0 && period > 0) {
            return static_cast<int>((quota + period - 1) / period);
        }
    }

    // 2.2 cgroup v2
    std::ifstream v2_quota_file("/sys/fs/cgroup/cpu.max");
    if (v2_quota_file) {
        std::string line;
        if (std::getline(v2_quota_file, line)) {
            std::istringstream iss(line);
            std::string quota_str;
            std::string period_str;
            if (iss >> quota_str >> period_str) {
                if (quota_str != "max") {
                    try {
                        int64_t quota  = std::stoll(quota_str);
                        int64_t period = std::stoll(period_str);
                        if (period > 0) {
                            return static_cast<int>((quota + period - 1) / period);
                        }
                    } catch (...) {
                        // juast do nothing
                    }
                }
            }
        }
    }

    // 3. use hardware_concurrency
    return static_cast<std::int32_t>(std::thread::hardware_concurrency());
}

} // namespace internal

inline std::uint32_t hardware_concurrency() {
    static std::once_flag s_flag;
    static std::uint32_t c_cpu_cores = 0;
    std::call_once(s_flag, [&]() { c_cpu_cores = static_cast<std::uint32_t>(internal::get_cpu_count()); });
    return c_cpu_cores;
}

} // namespace Fundamental