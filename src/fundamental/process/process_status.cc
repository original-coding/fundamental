#include "process_status.h"
#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <list>

namespace Fundamental
{
namespace details
{
static std::int64_t ParseKB(const std::string& str) {
    std::int64_t kb = 0;
    try {
        kb = std::stoll(str.substr(str.find_first_of("0123456789")));
    } catch (...) {
    }
    return kb;
}
} // namespace details

void ProcessStatus::Sample() {
    std::ifstream status("/proc/self/status");
    std::string line;
    std::int32_t targetItemNums = 4;
    sampleTimeMsec = Fundamental::Timer::GetTimeNow<std::chrono::milliseconds, std::chrono::steady_clock>();
    while (targetItemNums > 0 && std::getline(status, line)) {
        if (0 == strncmp(line.c_str(), "VmSize", 6)) {
            vmSize = details::ParseKB(line);
            --targetItemNums;
        } else if (0 == strncmp(line.c_str(), "VmRSS", 5)) {
            vmRss = details::ParseKB(line);
            --targetItemNums;
        } else if (0 == strncmp(line.c_str(), "VmPeak", 6)) {
            vmPeak = details::ParseKB(line);
            --targetItemNums;
        } else if (0 == strncmp(line.c_str(), "VmHWM", 5)) {
            vmHWM = details::ParseKB(line);
            --targetItemNums;
        }
    }
}

void ProcessStatus::Dump() const {
    std::string out;
    Dump(out);
    FDEBUG("[monitor]:{}", out);
}

void ProcessStatus::Dump(std::string& out) const {
    std::stringstream f;
    f << "MemInfo[KB]-> VmPeak:" << vmPeak << ",VmSize:" << vmSize << ",VmHWM:" << vmHWM << ",VmRSS:" << vmRss;
    out = f.str();
}

ProcessStatus ProcessStatus::Diff(const ProcessStatus& other) {
    ProcessStatus ret;
    ret.vmPeak=vmPeak-other.vmPeak;
    ret.vmHWM=vmHWM-other.vmHWM;
    ret.vmSize=vmSize-other.vmSize;
    ret.vmRss=vmRss-other.vmRss;
    ret.sampleTimeMsec=sampleTimeMsec-other.sampleTimeMsec;
    return ret;
}

void ProcessStatus::PrintCurrentStatus(const std::string& tag) {
    ProcessStatus status;
    status.Sample();
    std::string status_str;
    status.Dump(status_str);
    FINFO("[{} monitor] {}", tag, status_str);
}


} // namespace fongwell
