#include "dbrecovery/safety/write_protect.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h> // BLKROGET

namespace dbrecovery {
namespace safety {

WriteProtectGuard::WriteProtectGuard(const std::string& device_path)
    : device_path_(device_path) {
    // Extract the short device name for sysfs lookups
    auto pos = device_path_.rfind('/');
    if (pos != std::string::npos && pos + 1 < device_path_.size()) {
        device_name_ = device_path_.substr(pos + 1);
    } else {
        device_name_ = device_path_;
    }

    // Try to open with O_RDONLY | O_EXCL first
    fd_ = ::open(device_path_.c_str(), O_RDONLY | O_EXCL);
    if (fd_ >= 0) {
        exclusive_ = true;
        log("Opened " + device_path_ + " with O_RDONLY | O_EXCL (exclusive access)");
    } else {
        // If O_EXCL fails, try O_RDONLY only
        log("O_RDONLY | O_EXCL failed for " + device_path_ + ": " +
            std::strerror(errno) + " - falling back to O_RDONLY");
        fd_ = ::open(device_path_.c_str(), O_RDONLY);
        if (fd_ < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "Failed to open " + device_path_ + " read-only");
        }
        exclusive_ = false;
        log("Opened " + device_path_ + " with O_RDONLY (shared access)");
    }

    // Verify write-protect status
    if (!is_write_protected()) {
        // Check sysfs as a secondary verification
        int sysfs_ro = query_sysfs_read_only();
        if (sysfs_ro == 0) {
            log("WARNING: Device " + device_path_ + " is NOT write-protected!");
        } else if (sysfs_ro > 0) {
            log("Device " + device_path_ + " is write-protected (sysfs confirmed)");
        } else {
            log("WARNING: Device " + device_path_ + " write-protect status unknown");
        }
    } else {
        log("Device " + device_path_ + " is write-protected (BLKROGET confirmed)");
    }
}

WriteProtectGuard::~WriteProtectGuard() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        log("Closed " + device_path_);
    }
}

WriteProtectGuard::WriteProtectGuard(WriteProtectGuard&& other) noexcept
    : device_path_(std::move(other.device_path_))
    , device_name_(std::move(other.device_name_))
    , fd_(other.fd_)
    , exclusive_(other.exclusive_) {
    other.fd_ = -1;
    other.exclusive_ = false;
}

WriteProtectGuard& WriteProtectGuard::operator=(WriteProtectGuard&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        device_path_ = std::move(other.device_path_);
        device_name_ = std::move(other.device_name_);
        fd_ = other.fd_;
        exclusive_ = other.exclusive_;
        other.fd_ = -1;
        other.exclusive_ = false;
    }
    return *this;
}

bool WriteProtectGuard::is_write_protected() const {
    if (fd_ < 0) {
        return false;
    }

    int ro = 0;
    int ret = ::ioctl(fd_, BLKROGET, &ro);
    if (ret != 0) {
        log("BLKROGET ioctl failed: " + std::string(std::strerror(errno)));
        return false;
    }

    return (ro != 0);
}

bool WriteProtectGuard::force_read_only() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    // Re-open without O_EXCL
    fd_ = ::open(device_path_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        log("force_read_only: failed to open " + device_path_ + ": " +
            std::strerror(errno));
        return false;
    }

    exclusive_ = false;
    log("force_read_only: re-opened " + device_path_ + " with O_RDONLY");
    return true;
}

int WriteProtectGuard::query_sysfs_read_only() const {
    std::string ro_path = "/sys/block/" + device_name_ + "/ro";
    std::ifstream ifs(ro_path);
    if (!ifs.is_open()) {
        return -1;
    }

    std::string content;
    std::getline(ifs, content);
    ifs.close();

    if (content.empty()) {
        return -1;
    }

    try {
        return std::stoi(content);
    } catch (...) {
        return -1;
    }
}

void WriteProtectGuard::log(const std::string& message) {
    std::clog << "[WriteProtectGuard] " << message << std::endl;
}

} // namespace safety
} // namespace dbrecovery