#ifndef DBRECOVERY_SAFETY_DEVICE_IDENTITY_HPP
#define DBRECOVERY_SAFETY_DEVICE_IDENTITY_HPP

#include <string>
#include <cstdint>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>

namespace dbrecovery {
namespace safety {

/**
 * @brief Device identity information collected from NVMe identify controller data
 *        or sysfs fallback.
 */
struct DeviceIdentityInfo {
    std::string model_number;
    std::string serial_number;
    std::string firmware_revision;
    uint64_t namespace_size = 0;       // In logical blocks
    uint32_t lba_data_size = 0;        // Bytes per LBA (typically 512 or 4096)
    std::string device_path;
    std::string device_type;           // "nvme", "loop", "device_mapper", "unknown"
    double confidence = 0.0;           // 0.0 - 1.0
};

/**
 * @brief DeviceIdentity: Inspects block devices and extracts identity information.
 *
 * Supports NVMe devices via NVME_IOCTL_ADMIN_CMD, with fallback to sysfs.
 * Detects loopback, device-mapper, and general block devices via stat() + major
 * number lookups.
 */
class DeviceIdentity {
public:
    explicit DeviceIdentity(std::string sysfs_dev_block_root = "/sys/dev/block",
                            std::string sysfs_block_root = {});
    ~DeviceIdentity() = default;

    // Movable, non-copyable
    DeviceIdentity(const DeviceIdentity&) = delete;
    DeviceIdentity& operator=(const DeviceIdentity&) = delete;
    DeviceIdentity(DeviceIdentity&&) = default;
    DeviceIdentity& operator=(DeviceIdentity&&) = default;

    /**
     * @brief Probe a device path and verify its identity.
     *
     * Opens the device, reads identity data (NVMe ioctl or sysfs fallback),
     * and returns a populated DeviceIdentityInfo.
     *
     * @param device_path Path to the block device (e.g., /dev/nvme0n1, /dev/loop0).
     * @return DeviceIdentityInfo with all available fields and confidence score.
     */
    DeviceIdentityInfo verify_identity(const std::string& device_path);

    /**
     * @brief Verify identity using an already-open block-device descriptor.
     *
     * The descriptor is the object that will be authorized for later reads.
     */
    DeviceIdentityInfo verify_identity(int fd,
                                       const std::string& device_path,
                                       const struct stat& device_stat);

    /**
     * @brief Check if the device at the given path is an NVMe device.
     *
     * Uses stat() + major() and compares against the NVMe major number range (usually 240-259
     * but detected dynamically from /proc/partitions).
     *
     * @param device_path Path to the device.
     * @return true if the device is an NVMe block device.
     */
    bool is_nvme(const std::string& device_path) const;

    /**
     * @brief Check if the device is a loopback device.
     *
     * @param device_path Path to the device.
     * @return true if major() matches the loop major number (7).
     */
    bool is_loop(const std::string& device_path) const;

    /**
     * @brief Check if the device is a device-mapper device.
     *
     * @param device_path Path to the device.
     * @return true if the device is managed by device-mapper.
     */
    bool is_device_mapper(const std::string& device_path) const;

    /**
     * @brief Detect the device type string.
     *
     * @param device_path Path to the device.
     * @return "nvme", "loop", "device_mapper", or "unknown".
     */
    std::string detect_device_type(const std::string& device_path) const;

private:
    /**
     * @brief Read identity from NVMe device via ioctl.
     *
     * @param fd Open file descriptor to the NVMe device.
     * @param info Output parameter to populate.
     * @return true on success.
     */
    bool read_nvme_identity(int fd, DeviceIdentityInfo& info);

    /**
     * @brief Fallback: read identity from sysfs (/sys/block/<dev>/device/).
     *
     * @param device_path Device path (e.g., /dev/nvme0n1).
     * @param info Output parameter to populate.
     * @return true on success.
     */
    bool read_sysfs_identity(const struct stat& device_stat, DeviceIdentityInfo& info);

    /**
     * @brief Read loopback backing file path from sysfs (/sys/block/<dev>/loop/backing_file).
     *
     * @param dev_name Short device name (e.g., "loop0").
     * @param backing_file Output populated with trimmed backing file path.
     * @return true if backing file was read non-empty.
     */
    bool read_loopback_sysfs(const std::string& dev_name, std::string& backing_file) const;

    /**
     * @brief Apply verified loopback identity based on regular-file backing within trusted root.
     *
     * @param device_path Path to the block device.
     * @param info Output parameter to populate.
     */
    void apply_loopback_identity(const std::string& device_path, DeviceIdentityInfo& info) const;

    /**
     * @brief Resolve the major number for a device path using stat().
     *
     * @param device_path Path to the device.
     * @return major() number, or 0 on error.
     */
    static uint32_t get_major_number(const std::string& device_path);

    /**
     * @brief Build a map of major number -> device type name from /proc/partitions.
     *
     * @return Map of major number to device type name (e.g., {259, "nvme"}).
     */
    static std::vector<std::pair<uint32_t, std::string>> load_major_map();

    /** Cached major number map */
    mutable std::vector<std::pair<uint32_t, std::string>> major_map_;
    std::string sysfs_dev_block_root_;
    std::string sysfs_block_root_;
};

} // namespace safety
} // namespace dbrecovery

#endif // DBRECOVERY_SAFETY_DEVICE_IDENTITY_HPP