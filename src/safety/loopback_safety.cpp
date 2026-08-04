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

    // Check if the device has a backing_file entry in sysfs
    std::string backing_file_path = "/sys/block/" + dev_name + "/loop/backing_file";
    std::ifstream ifs(backing_file_path);
    if (ifs.is_open()) {
        std::string content;
        std::getline(ifs, content);
        ifs.close();
        return !content.empty();
    }

    // Fallback: check if the device name starts with "loop"
    return (dev_name.compare(0, 4, "loop") == 0);
}

std::string LoopbackSafety::find_backing_file(const std::string& device_path) const {
    std::string dev_name = device_name_from_path(device_path);
    if (dev_name.empty()) {
        return {};
    }

    // Read /sys/block/<dev>/loop/backing_file
    std::string backing_file_path = "/sys/block/" + dev_name + "/loop/backing_file";
    std::string content = read_sysfs_file(backing_file_path);
    if (!content.empty()) {
        return content;
    }

    // Fallback: try running losetup -l and parse the output
    // This is used when sysfs is not available (e.g., in containers)
    FILE* fp = ::popen("/sbin/losetup -l 2>/dev/null || /usr/sbin/losetup -l 2>/dev/null || losetup -l 2>/dev/null", "r");
    if (!fp) {
        return {};
    }

    char buf[4096];
    while (::fgets(buf, sizeof(buf), fp) != nullptr) {
        std::string line(buf);
        // Look for a line containing our device name
        if (line.find(dev_name) != std::string::npos) {
            // Format: /dev/loop0: [xxxx]:xxxx (/path/to/backing/file)
            auto paren_open = line.find('(');
            auto paren_close = line.find(')');
            if (paren_open != std::string::npos && paren_close > paren_open) {
                std::string backing = line.substr(paren_open + 1,
                                                   paren_close - paren_open - 1);
                ::pclose(fp);
                return backing;
            }
        }
    }

    ::pclose(fp);
    return {};
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
    // Check if it is a loopback device and log the backing file
    if (is_loopback_device(device_path)) {
        std::string backing = find_backing_file(device_path);
        if (!backing.empty()) {
            std::clog << "[LoopbackSafety] Device " << device_path
                      << " is a loopback device backed by: " << backing << std::endl;
        } else {
            std::clog << "[LoopbackSafety] Device " << device_path
                      << " is a loopback device (backing file unknown)" << std::endl;
        }
    }

    // Check if the device is mounted and warn
    auto mount_points = get_mount_points(device_path);
    if (!mount_points.empty()) {
        std::clog << "[LoopbackSafety] WARNING: Device " << device_path
                  << " is mounted at:";
        for (const auto& mp : mount_points) {
            std::clog << " " << mp;
        }
        std::clog << std::endl;
    }

    // Open read-only (no O_EXCL to avoid failing on already-open devices)
    int fd = ::open(device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "[LoopbackSafety] Failed to open " << device_path
                  << " read-only: " << std::strerror(errno) << std::endl;
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