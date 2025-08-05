

#include "fundamental/basic/log.h"
#include "fundamental/delay_queue/delay_queue.h"
#include "fundamental/process/process_status.h"
#include "fundamental/thread_pool/thread_pool.h"
#include "fundamental/tracker/memory_tracker.hpp"
#include <malloc.h>
int main(int argc, char** argv) {
    using Fundamental::ProcessStatus;
    ProcessStatus last_process_status;

    Fundamental::EnableMemoryProfiling();
    Fundamental::DelayQueue queue;
    auto task_func = [&]() {
        Fundamental::ThreadPool::LongTimePool().Enqueue([&]() {
            ProcessStatus current;
            current.Sample();
            ProcessStatus diff;
            {
                diff                = current.Diff(last_process_status);
                last_process_status = current;
            }
            std::string dump_info;
            if (diff.vmPeak > 0) {
                dump_info += Fundamental::StringFormat("VmPeak+{}\t", diff.vmPeak);
            }
            if (diff.vmHWM > 0) {
                dump_info += Fundamental::StringFormat("VmHWM+{}\t", diff.vmHWM);
            }
            if (diff.vmSize != 0) {
                dump_info +=
                    Fundamental::StringFormat("VmSize{}{}\t", (diff.vmSize > 0 ? '+' : '-'), std::abs(diff.vmSize));
            }
            if (diff.vmRss != 0) {
                dump_info +=
                    Fundamental::StringFormat("VmRSS{}{}\t", (diff.vmRss > 0 ? '+' : '-'), std::abs(diff.vmRss));
            }
            if (!dump_info.empty()) {
                FINFO("memory monitor changed for {} msec {}", diff.sampleTimeMsec, dump_info);
                current.Dump();
            }
#if TARGET_PLATFORM_LINUX
            // trim memory
            malloc_trim(0);
#endif // TARGET_PLATFORM_LINUX

            
        });
    };
    auto handle = queue.AddDelayTask(2000, task_func);
    last_process_status.Sample();
    queue.StartDelayTask(handle);

    std::size_t cnt = 10;
    while (cnt > 0) {
        queue.HandleEvent();
        std::this_thread::sleep_for(std::chrono::milliseconds(queue.GetNextTimeoutMsec()));
        // Fundamental::DumpMemorySnapShot(std::to_string(cnt) + ".heap");

        --cnt;
    }
    queue.RemoveDelayTask(handle);
    return 0;
}
