#include "dbrecovery/safety/loopback_safety.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <climits>
#include <deque>
#include <filesystem>
#include <unordered_set>
#include <unistd.h>
#include <fcntl.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

using dbrecovery::safety::BlockDeviceId;
using dbrecovery::safety::MountTopologyReport;

std::string device_key(const BlockDeviceId& device) {
    return std::to_string(device.major) + ":" + std::to_string(device.minor);
}

bool parse_device_id(const std::string& value, BlockDeviceId* device) {
    const auto separator = value.find(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= value.size() ||
        value.find(':', separator + 1) != std::string::npos) {
        return false;
    }

    try {
        size_t major_length = 0;
        size_t minor_length = 0;
        const unsigned long parsed_major = std::stoul(value, &major_length, 10);
        const unsigned long parsed_minor =
            std::stoul(value.substr(separator + 1), &minor_length, 10);
        if (major_length != separator ||
            minor_length != value.size() - separator - 1 ||
            parsed_major > UINT_MAX || parsed_minor > UINT_MAX) {
            return false;
        }
        device->major = static_cast<unsigned int>(parsed_major);
        device->minor = static_cast<unsigned int>(parsed_minor);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string unescape_mountinfo_path(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\\' && index + 3 < value.size() &&
            value[index + 1] >= '0' && value[index + 1] <= '7' &&
            value[index + 2] >= '0' && value[index + 2] <= '7' &&
            value[index + 3] >= '0' && value[index + 3] <= '7') {
            const unsigned char escaped = static_cast<unsigned char>(
                (value[index + 1] - '0') * 64 + (value[index + 2] - '0') * 8 +
                (value[index + 3] - '0'));
            result.push_back(static_cast<char>(escaped));
            index += 3;
            continue;
        }
        result.push_back(value[index]);
    }
    return result;
}

bool is_related(const std::vector<BlockDeviceId>& devices, const BlockDeviceId& candidate) {
    for (const auto& device : devices) {
        if (device == candidate) return true;
    }
    return false;
}

} // namespace

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

MountTopologyReport LoopbackSafety::inspect_mount_topology(
    const struct stat& device_stat) const {
    MountTopologyReport report;
    if (!S_ISBLK(device_stat.st_mode)) {
        report.error = "admitted descriptor is not a block device";
        return report;
    }

    return inspect_mount_topology_for_paths(
        BlockDeviceId{::major(device_stat.st_rdev), ::minor(device_stat.st_rdev)},
        "/proc/self/mountinfo", "/sys/dev/block");
}

MountTopologyReport LoopbackSafety::inspect_mount_topology_for_paths(
    const BlockDeviceId& device,
    const std::string& mountinfo_path,
    const std::string& sysfs_dev_block_root) const {
    MountTopologyReport report;
    namespace fs = std::filesystem;

    const fs::path dev_block_root(sysfs_dev_block_root);
    std::error_code error;
    if (!fs::is_directory(dev_block_root, error) || error) {
        report.error = "sysfs device identity root is unavailable";
        return report;
    }

    std::deque<BlockDeviceId> pending;
    std::unordered_set<std::string> visited;

    auto add_device = [&](const BlockDeviceId& related) {
        const auto key = device_key(related);
        if (visited.insert(key).second) {
            report.related_devices.push_back(related);
            pending.push_back(related);
        }
    };
    add_device(device);

    while (!pending.empty()) {
        const BlockDeviceId current = pending.front();
        pending.pop_front();
        const fs::path node = dev_block_root / device_key(current);
        const fs::path canonical_node = fs::canonical(node, error);
        if (error || !fs::is_directory(canonical_node, error) || error) {
            report.error = "could not resolve sysfs device " + device_key(current);
            return report;
        }

        const std::string node_device = read_sysfs_file((canonical_node / "dev").string());
        BlockDeviceId parsed_node;
        if (!parse_device_id(node_device, &parsed_node) || !(parsed_node == current)) {
            report.error = "sysfs identity mismatch for device " + device_key(current);
            return report;
        }

        const fs::path parent = canonical_node.parent_path();
        const std::string parent_device = read_sysfs_file((parent / "dev").string());
        BlockDeviceId parsed_parent;
        if (!parent_device.empty() && parse_device_id(parent_device, &parsed_parent)) {
            std::error_code parent_error;
            if (fs::exists(dev_block_root / device_key(parsed_parent), parent_error) &&
                !parent_error) {
                add_device(parsed_parent);
            }
        } else if (!parent_device.empty()) {
            report.error = "invalid sysfs parent identity for " + device_key(current);
            return report;
        }

        fs::directory_iterator children(canonical_node, error);
        if (error) {
            report.error = "could not enumerate sysfs child partitions for device " +
                           device_key(current);
            return report;
        }
        for (const auto& entry : children) {
            const fs::file_status entry_status = entry.symlink_status(error);
            if (error) {
                report.error = "could not inspect sysfs child entry for device " +
                               device_key(current);
                return report;
            }
            if (!fs::is_directory(entry_status) && !fs::is_symlink(entry_status)) {
                continue;
            }

            const fs::path child = fs::canonical(entry.path(), error);
            if (error || !fs::is_directory(child, error) || error) {
                report.error = "could not resolve sysfs child partition for device " +
                               device_key(current);
                return report;
            }
            if (!fs::exists(child / "partition", error) || error) {
                if (error) {
                    report.error = "could not inspect sysfs child partition for device " +
                                   device_key(current);
                    return report;
                }
                continue;
            }

            const std::string child_device = read_sysfs_file((child / "dev").string());
            BlockDeviceId parsed_child;
            if (!parse_device_id(child_device, &parsed_child)) {
                report.error = "invalid sysfs child partition identity for device " +
                               device_key(current);
                return report;
            }
            add_device(parsed_child);
        }

        for (const char* relation : {"slaves", "holders"}) {
            const fs::path relation_dir = canonical_node / relation;
            if (!fs::is_directory(relation_dir, error) || error) {
                report.error = "missing sysfs " + std::string(relation) +
                               " topology for device " + device_key(current);
                return report;
            }

            fs::directory_iterator iterator(relation_dir, error);
            if (error) {
                report.error = "could not enumerate sysfs " + std::string(relation) +
                               " topology for device " + device_key(current);
                return report;
            }
            for (const auto& entry : iterator) {
                const fs::path target = fs::canonical(entry.path(), error);
                if (error || !fs::is_directory(target, error) || error) {
                    report.error = "could not resolve sysfs " + std::string(relation) +
                                   " relation for device " + device_key(current);
                    return report;
                }
                const std::string target_device = read_sysfs_file((target / "dev").string());
                BlockDeviceId parsed_target;
                if (!parse_device_id(target_device, &parsed_target)) {
                    report.error = "invalid sysfs " + std::string(relation) +
                                   " identity for device " + device_key(current);
                    return report;
                }
                add_device(parsed_target);
            }
        }
    }

    std::ifstream mountinfo(mountinfo_path);
    if (!mountinfo.is_open()) {
        report.error = "mountinfo is unavailable";
        return report;
    }

    std::string line;
    while (std::getline(mountinfo, line)) {
        if (line.empty()) {
            report.error = "empty mountinfo entry";
            return report;
        }

        std::istringstream fields(line);
        std::string mount_id;
        std::string parent_id;
        std::string device_id;
        std::string root;
        std::string mount_point;
        std::string options;
        if (!(fields >> mount_id >> parent_id >> device_id >> root >> mount_point >> options)) {
            report.error = "malformed mountinfo entry";
            return report;
        }

        BlockDeviceId mounted_device;
        if (!parse_device_id(device_id, &mounted_device)) {
            report.error = "malformed mountinfo device identity";
            return report;
        }

        std::string field;
        bool separator_found = false;
        while (fields >> field) {
            if (field == "-") {
                separator_found = true;
                break;
            }
        }
        std::string filesystem_type;
        std::string mount_source;
        std::string super_options;
        if (!separator_found || !(fields >> filesystem_type >> mount_source >> super_options)) {
            report.error = "malformed mountinfo separator";
            return report;
        }

        if (is_related(report.related_devices, mounted_device)) {
            report.mount_points.push_back(unescape_mountinfo_path(mount_point));
        }
    }
    if (mountinfo.bad()) {
        report.error = "could not read mountinfo";
        return report;
    }

    report.complete = true;
    return report;
}

bool LoopbackSafety::check_mounted(const std::string& device_path) const {
    struct stat device_stat {};
    if (::stat(device_path.c_str(), &device_stat) != 0 || !S_ISBLK(device_stat.st_mode)) {
        return true;
    }
    const auto report = inspect_mount_topology(device_stat);
    return !report.complete || report.mounted();
}

std::vector<std::string> LoopbackSafety::get_mount_points(const std::string& device_path) const {
    struct stat device_stat {};
    if (::stat(device_path.c_str(), &device_stat) != 0 || !S_ISBLK(device_stat.st_mode)) {
        return {};
    }
    const auto report = inspect_mount_topology(device_stat);
    return report.complete ? report.mount_points : std::vector<std::string>{};
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