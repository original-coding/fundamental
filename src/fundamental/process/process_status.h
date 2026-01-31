#ifndef _HEAD_PROCESS_STATUS_MONITOR_INTERNAL_
#define _HEAD_PROCESS_STATUS_MONITOR_INTERNAL_
#include <atomic>
#include <cstdint>
#include <string>

namespace Fundamental {

struct ProcessStatus { // see 'man proc'
    // Peak virtual memory size.
    std::int64_t vmPeak = 0;
    // Virtual memory size
    std::int64_t vmSize = 0;
    // Resident set size.  Note that the value here is the sum of RssAnon, RssFile, and RssShmem.  This value is
    // inaccurate; see /proc/[pid]/statm above.
    std::int64_t vmRss = 0;
    // Peak resident set size ("high water mark").  This value is inaccurate; see /proc/[pid]/statm above.
    std::int64_t vmHWM = 0;
    //
    std::int64_t sampleTimeMsec = 0;
    void Sample();
    void Dump() const;
    void Dump(std::string &out) const;
    ProcessStatus Diff(const ProcessStatus &other);
    static void PrintCurrentStatus(const std::string &tag);
};

} // namespace fongwell

#endif //_HEAD_PROCESS_STATUS_MONITOR_INTERNAL_