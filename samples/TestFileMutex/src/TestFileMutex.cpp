#include "fundamental/basic/log.h"
#include "fundamental/basic/mutext_utils.hpp"
int main(int argc, char* argv[]) {
    if (argc == 1) {
        printf("%s <lock_name>\n", argv[0]);
        return 1;
    }
    Fundamental::file_mutex mutex(argv[1]);
    auto ret = mutex.Lock();
    if (!ret) {
        FERR("Failed to lock file:{}", argv[1]);
        return 1;
    } else {
        FINFO("lock {} success", argv[1]);
    }
    FASSERT(mutex.IsLocked());
    mutex.Unlock();
    mutex.TryLock();
    getchar();
    mutex.Unlock();
    FINFO("unlock {} success", argv[1]);

    return 0;
}
