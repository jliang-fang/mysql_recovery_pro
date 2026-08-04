#include "dbrecovery/safety/device_identity.hpp"

#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dirent.h>

// NVMe ioctl header -- may not be available on all systems
#if defined(__linux__) && defined(HAVE_LINUX_NVME_IOCTL_H)
#include <linux/nvme_ioctl.h>
#endif

namespace dbrecovery {
namespace safety {

// When the kernel header is not available, define our own structures for the
// NVMe identify ioctl. This ensures the code compiles everywhere; the ioctl
// will simply fail at runtime if the kernel does not support it.
#ifndef HAVE_LINUX_NVME_IOCTL_H

#ifndef NVME_IOCTL_ADMIN_CMD
#define NVME_IOCTL_ADMIN_CMD _IOWR('N', 0x41, struct nvme_admin_cmd)
#endif

struct nvme_admin_cmd {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t rsvd1;
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint32_t metadata;
    uint64_t addr;
    uint32_t metadata_len;
    uint32_t data_len;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
    uint32_t timeout_ms;
    uint32_t result;
};

// Identify command CNS value: 01h = Identify Controller
static constexpr uint8_t NVME_IDENTIFY_CNS = 0x01;

#endif // HAVE_LINUX_NVME_IOCTL_H

DeviceIdentity::DeviceIdentity() {
    major_map_ = load_major_map();
}

DeviceIdentityInfo DeviceIdentity::verify_identity(const std::string& device_path) {
    DeviceIdentityInfo info;
    info.device_path = device_path;
    info.device_type = detect_device_type(device_path);

    // Try NVMe ioctl first for the most authoritative identity data
    if (info.device_type == "nvme") {
        int fd = ::open(device_path.c_str(), O_RDONLY);
        if (fd >= 0) {
            if (read_nvme_identity(fd, info)) {
                info.confidence = 0.95;
            }
            ::close(fd);
        }
    }

    // Fallback to sysfs if NVMe identity was not obtained or confidence is low
    if (info.confidence < 0.5) {
        if (read_sysfs_identity(device_path, info)) {
            // Sysfs is less authoritative than NVMe ioctl
            if (info.confidence < 0.5) {
                info.confidence = 0.7;
            }
        }
    }

    // If no identity data could be read, set low confidence
    if (info.model_number.empty() && info.serial_number.empty()) {
        info.confidence = 0.1;
    }

    return info;
}

bool DeviceIdentity::is_nvme(const std::string& device_path) const {
    return detect_device_type(device_path) == "nvme";
}

bool DeviceIdentity::is_loop(const std::string& device_path) const {
    return detect_device_type(device_path) == "loop";
}

bool DeviceIdentity::is_device_mapper(const std::string& device_path) const {
    return detect_device_type(device_path) == "device_mapper";
}

std::string DeviceIdentity::detect_device_type(const std::string& device_path) const {
    struct stat st;
    if (::stat(device_path.c_str(), &st) != 0) {
        return "unknown";
    }
    if (!S_ISBLK(st.st_mode)) {
        return "unknown";
    }

    uint32_t major_num = major(st.st_rdev);

    // Loop devices always have major number 7
    if (major_num == 7) {
        return "loop";
    }

    // Check against the /proc/partitions map
    for (const auto& entry : major_map_) {
        if (entry.first == major_num) {
            if (entry.second.find("nvme") != std::string::npos) {
                return "nvme";
            }
            if (entry.second.find("dm-") != std::string::npos ||
                entry.second.find("device-mapper") != std::string::npos) {
                return "device_mapper";
            }
        }
    }

    // Fallback: heuristic detection based on the device name string
    if (device_path.find("nvme") != std::string::npos) {
        return "nvme";
    }
    if (device_path.find("dm-") != std::string::npos) {
        return "device_mapper";
    }
    if (device_path.find("loop") != std::string::npos) {
        return "loop";
    }

    return "unknown";
}

bool DeviceIdentity::read_nvme_identity(int fd, DeviceIdentityInfo& info) {
#if defined(__linux__) && defined(HAVE_LINUX_NVME_IOCTL_H)
    // Use the system's NVME_IOCTL_ADMIN_CMD
    std::vector<uint8_t> id_ctrl_buf(4096, 0);

    struct nvme_admin_cmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = 0x02; // NVMe Admin opcode: Identify
    cmd.nsid     = 0;    // Controller level identify
    cmd.addr     = reinterpret_cast<uint64_t>(id_ctrl_buf.data());
    cmd.data_len = 4096;
    cmd.cdw10    = 0x01; // CNS = 01h (Identify Controller)

    int ret = ::ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
    if (ret != 0) {
        return false;
    }

    // Extract fields from the identify controller data buffer
    // Layout per NVMe specification:
    //   SN (Serial Number)   at offset 4,  20 bytes (ASCII)
    //   MN (Model Number)    at offset 24, 40 bytes (ASCII)
    //   FR (Firmware Rev)    at offset 64, 8 bytes  (ASCII)
    char sn[21] = {};
    char mn[41] = {};
    char fr[9]  = {};

    std::memcpy(sn, id_ctrl_buf.data() + 4,  20);
    std::memcpy(mn, id_ctrl_buf.data() + 24, 40);
    std::memcpy(fr, id_ctrl_buf.data() + 64, 8);

    // Trim trailing spaces (NVMe pads with spaces)
    auto trim_right = [](char* s, size_t len) {
        for (size_t i = len; i > 0; --i) {
            if (s[i - 1] != ' ' && s[i - 1] != '\0') {
                s[i] = '\0';
                return;
            }
            s[i - 1] = '\0';
        }
    };
    trim_right(sn, 20);
    trim_right(mn, 40);
    trim_right(fr, 8);

    info.serial_number     = sn;
    info.model_number      = mn;
    info.firmware_revision = fr;

    // Query namespace identify to get NSZE and LBA data size
    std::vector<uint8_t> id_ns_buf(4096, 0);
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = 0x02;
    cmd.nsid     = 1; // Typically the first namespace
    cmd.addr     = reinterpret_cast<uint64_t>(id_ns_buf.data());
    cmd.data_len = 4096;
    cmd.cdw10    = 0x00; // CNS = 00h (Identify Namespace)

    ret = ::ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
    if (ret == 0) {
        // NSZE at offset 0 (8 bytes, little-endian)
        uint64_t nsze = 0;
        std::memcpy(&nsze, id_ns_buf.data(), sizeof(nsze));
        info.namespace_size = nsze;

        // FLBAS at offset 26 (1 byte)
        uint8_t flbas = id_ns_buf[26];
        uint8_t lbaf_index = flbas & 0x0F;

        // LBAF data size at offset 128 + lbaf_index * 8 (4 bytes, little-endian)
        uint32_t lbaf_offset = 128 + lbaf_index * 8;
        if (lbaf_offset + 4 <= id_ns_buf.size()) {
            uint32_t lbaf_entry = 0;
            std::memcpy(&lbaf_entry, id_ns_buf.data() + lbaf_offset, 4);
            // Lower 16 bits = LBA data size in bytes
            uint16_t lba_size_raw = static_cast<uint16_t>(lbaf_entry & 0xFFFF);
            if (lba_size_raw > 0) {
                info.lba_data_size = lba_size_raw;
            } else {
                info.lba_data_size = 512;
            }
        } else {
            info.lba_data_size = 512;
        }
    }

    return true;
#else
    // NVME_IOCTL_ADMIN_CMD not available on this platform
    (void)fd;
    (void)info;
    return false;
#endif
}

bool DeviceIdentity::read_sysfs_identity(const std::string& device_path,
                                          DeviceIdentityInfo& info) {
    // Derive the short device name from the path
    // /dev/nvme0n1 -> nvme0n1, /dev/sda -> sda
    std::string dev_name;
    auto pos = device_path.rfind('/');
    if (pos != std::string::npos && pos + 1 < device_path.size()) {
        dev_name = device_path.substr(pos + 1);
    } else {
        dev_name = device_path;
    }

    // NVMe sysfs path: /sys/block/<dev>/device/{model,serial,firmware_rev}
    std::string sysfs_base = "/sys/block/" + dev_name + "/device/";

    std::string model_path   = sysfs_base + "model";
    std::string serial_path  = sysfs_base + "serial";
    std::string firmware_path = sysfs_base + "firmware_rev";

    std::ifstream ifs;

    ifs.open(model_path);
    if (ifs.is_open()) {
        std::getline(ifs, info.model_number);
        ifs.close();
    }

    ifs.open(serial_path);
    if (ifs.is_open()) {
        std::getline(ifs, info.serial_number);
        ifs.close();
    }

    ifs.open(firmware_path);
    if (ifs.is_open()) {
        std::getline(ifs, info.firmware_revision);
        ifs.close();
    }

    // Read size info from sysfs
    std::string size_path = "/sys/block/" + dev_name + "/size";
    ifs.open(size_path);
    if (ifs.is_open()) {
        std::string size_str;
        std::getline(ifs, size_str);
        if (!size_str.empty()) {
            try {
                info.namespace_size = std::stoull(size_str);
            } catch (...) {
                // Ignore parse errors
            }
        }
        ifs.close();
    }

    // Read logical block size
    std::string queue_logical_path = "/sys/block/" + dev_name + "/queue/logical_block_size";
    ifs.open(queue_logical_path);
    if (ifs.is_open()) {
        std::string size_str;
        std::getline(ifs, size_str);
        if (!size_str.empty()) {
            try {
                info.lba_data_size = static_cast<uint32_t>(std::stoul(size_str));
            } catch (...) {
            }
        }
        ifs.close();
    }

    return !info.model_number.empty() || !info.serial_number.empty();
}

uint32_t DeviceIdentity::get_major_number(const std::string& device_path) {
    struct stat st;
    if (::stat(device_path.c_str(), &st) != 0) {
        return 0;
    }
    if (!S_ISBLK(st.st_mode)) {
        return 0;
    }
    return major(st.st_rdev);
}

std::vector<std::pair<uint32_t, std::string>> DeviceIdentity::load_major_map() {
    std::vector<std::pair<uint32_t, std::string>> entries;
    std::ifstream proc_partitions("/proc/partitions");
    if (!proc_partitions.is_open()) {
        return entries;
    }

    std::string line;
    // Skip header lines (first two lines)
    std::getline(proc_partitions, line);
    std::getline(proc_partitions, line);

    while (std::getline(proc_partitions, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        uint32_t major_num = 0;
        uint32_t minor_num = 0;
        uint64_t blocks = 0;
        std::string name;

        iss >> major_num >> minor_num >> blocks >> name;
        if (iss.fail()) continue;

        // Derive the device type from the name
        std::string type;
        if (name.compare(0, 4, "nvme") == 0) {
            type = "nvme";
        } else if (name.compare(0, 3, "dm-") == 0) {
            type = "dm-";
        } else if (name.compare(0, 4, "loop") == 0) {
            type = "loop";
        } else if (name.compare(0, 3, "sd") == 0) {
            type = "sd";
        } else if (name.compare(0, 3, "hd") == 0) {
            type = "hd";
        } else if (name.compare(0, 4, "vd") == 0) {
            type = "vd";
        } else {
            type = name;
        }

        // Only add each major number once
        bool found = false;
        for (const auto& entry : entries) {
            if (entry.first == major_num) {
                found = true;
                break;
            }
        }
        if (!found) {
            entries.emplace_back(major_num, type);
        }
    }

    return entries;
}

} // namespace safety
} // namespace dbrecovery