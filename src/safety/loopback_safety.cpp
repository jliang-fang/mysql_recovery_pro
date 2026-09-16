#include "dbrecovery/safety/loopback_safety.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace dbrecovery {
namespace safety {

bool LoopbackSafety::is_loopback_device(const std::string& device_path) const {
    std::string dev_name = device_name_from_path(device_path);
    if (dev_name.empty()) {
        return false;
    }

    // A loop device is identified authoritatively by sysfs: the kernel only
    // creates /sys/block/<dev>/loop/ for actual loop devices (major 7).
    std::string backing_file_path = "/sys/block/" + dev_name + "/loop/backing_file";
    std::ifstream ifs(backing_file_path);
    if (ifs.is_open()) {
        std::string content;
        std::getline(ifs, content);
        ifs.close();
        return !content.empty();
    }

    return false;
}

std::string LoopbackSafety::find_backing_file(const std::string& device_path) const {
    std::string dev_name = device_name_from_path(device_path);
    if (dev_name.empty()) {
        return {};
    }

    // sysfs is the only authoritative source: the kernel records the backing
    // file path at /sys/block/<dev>/loop/backing_file. No shell fallbacks.
    std::string backing_file_path = "/sys/block/" + dev_name + "/loop/backing_file";
    return read_sysfs_file(backing_file_path);
}

bool LoopbackSafety::check_mounted(const std::string& device_path) const {
    return !get_mount_points(device_path).empty();
}

std::vector<std::string> LoopbackSafety::get_mount_points(const std::string& device_path) const {
    std::vector<std::string> mount_points;
    std::ifstream ifs("/proc/mounts");
    if (!ifs.is_open()) {
        return mount_points;
    }

    // Resolve the device path to a real path for comparison
    char resolved_path[4096] = {};
    std::string resolved_device;
    if (::realpath(device_path.c_str(), resolved_path) != nullptr) {
        resolved_device = resolved_path;
    } else {
        resolved_device = device_path;
    }

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;

        // /proc/mounts format: device mount_point fstype options dump pass
        std::istringstream iss(line);
        std::string dev, mount_point, fstype;
        iss >> dev >> mount_point >> fstype;

        if (dev.empty()) continue;

        // Check if the device matches
        if (dev == resolved_device || dev == device_path) {
            mount_points.push_back(mount_point);
        }
    }

    return mount_points;
}

bool LoopbackSafety::verify_read_only(const std::string& device_path) const {
    int fd = ::open(device_path.c_str(), O_RDONLY | O_EXCL);
    if (fd >= 0) {
        ::close(fd);
        return true;
    }
    return false;
}

int LoopbackSafety::safe_open(const std::string& device_path) const {
    // safe_open is not an authorization boundary: mounted devices and
    // unverified loopbacks must be refused outright, not warned about.
    // Authorization is exclusively SafetyGate::permits_read_only_raw_access.
    if (is_loopback_device(device_path)) {
        std::string backing = find_backing_file(device_path);
        if (backing.empty()) {
            std::cerr << "[LoopbackSafety] Refusing " << device_path
                      << ": loopback device with no discoverable backing file"
                      << std::endl;
            errno = EACCES;
            return -1;
        }
    }

    if (check_mounted(device_path)) {
        std::cerr << "[LoopbackSafety] Refusing " << device_path
                  << ": device is mounted" << std::endl;
        errno = EBUSY;
        return -1;
    }

    int fd = ::open(device_path.c_str(), O_RDONLY | O_EXCL | O_NOFOLLOW);
    if (fd < 0) {
        std::cerr << "[LoopbackSafety] Failed to open " << device_path
                  << " read-only exclusive: " << std::strerror(errno) << std::endl;
    }

    return fd;
}

std::string LoopbackSafety::device_name_from_path(const std::string& device_path) {
    auto pos = device_path.rfind('/');
    if (pos != std::string::npos && pos + 1 < device_path.size()) {
        return device_path.substr(pos + 1);
    }
    // If no slash, assume it is already a short name
    return device_path;
}

std::string LoopbackSafety::read_sysfs_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return {};
    }
    std::string content;
    std::getline(ifs, content);
    ifs.close();

    // Trim trailing whitespace
    while (!content.empty() && (content.back() == ' ' ||
                                 content.back() == '\t' ||
                                 content.back() == '\n' ||
                                 content.back() == '\r')) {
        content.pop_back();
    }

    return content;
}

} // namespace safety
} // namespace dbrecovery