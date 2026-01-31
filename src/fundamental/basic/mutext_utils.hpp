#pragma once
#include "fundamental/basic/base64_utils.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Fundamental
{

class file_mutex {
    static constexpr const char* kLckFileExt = ".lck";
    static constexpr const char* kLckFileDir = "/tmp/";

public:
    file_mutex(std::string_view lock_name) {
        assert(lock_name.size() > 0);
        _lock_filename = std::string(kLckFileDir) +
                         Fundamental::Base64Encode<Base64CoderType::kFSBase64>(lock_name.data(), lock_name.size()) +
                         kLckFileExt;
    }

    ~file_mutex() {
        Unlock();
    }

    bool Lock() {
        if (IsLocked()) return true;
        fd = open(_lock_filename.c_str(), O_RDWR | O_CREAT | __O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd < 0) {
            return false;
        }

        while (flock(fd, LOCK_EX) != 0) {
            // maybe permission denied
            if (errno != EINTR) {
                close(fd);
                fd = -1;
                return false;
            }
        }

        return true;
    }
    bool TryLock() {
        if (IsLocked()) return true;
        fd = open(_lock_filename.c_str(), O_RDWR | O_CREAT | __O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd < 0) {
            return false;
        }
        auto ret = flock(fd, LOCK_EX | LOCK_NB);
        if (ret != 0) {
            close(fd);
            fd = -1;
            return false;
        }
        return true;
    }
    bool Unlock() {
        if (fd >= 0) {
            // Release the lock and close the file descriptor
            flock(fd, LOCK_UN);
            close(fd);
            fd = -1;
            // Remove the lock file
            std::remove(_lock_filename.c_str());
        }
        return true;
    }

    bool IsLocked() const {
        return fd >= 0;
    }

private:
    std::string _lock_filename;
    std::int32_t fd = -1;
};

}; // namespace Fundamental