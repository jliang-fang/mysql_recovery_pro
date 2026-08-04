#ifndef DBRECOVERY_SAFETY_LOOPBACK_SAFETY_HPP
#define DBRECOVERY_SAFETY_LOOPBACK_SAFETY_HPP

#include <string>
#include <vector>
#include <system_error>

namespace dbrecovery {
namespace safety {

/**
 * @brief LoopbackSafety: Detects loopback devices and manages safe access.
 *
 * Provides utilities for identifying loopback devices, finding their backing
 * files, checking mount status, and ensuring read-only access.
 */
class LoopbackSafety {
public:
    LoopbackSafety() = default;
    ~LoopbackSafety() = default;

    // Movable, non-copyable
    LoopbackSafety(const LoopbackSafety&) = delete;
    LoopbackSafety& operator=(const LoopbackSafety&) = delete;
    LoopbackSafety(LoopbackSafety&&) = default;
    LoopbackSafety& operator=(LoopbackSafety&&) = default;

    /**
     * @brief Check if the given path is a loopback device.
     *
     * Parses /sys/block/loopN/loop/backing_file or runs losetup -l internally.
     *
     * @param device_path Path to check (e.g., /dev/loop0).
     * @return true if the path is a loopback device.
     */
    bool is_loopback_device(const std::string& device_path) const;

    /**
     * @brief Find the backing file for a loopback device.
     *
     * Reads /sys/block/<dev>/loop/backing_file directly.
     *
     * @param device_path Path to the loop device (e.g., /dev/loop0).
     * @return Backing file path, or empty string if not found.
     */
    std::string find_backing_file(const std::string& device_path) const;

    /**
     * @brief Check if the device is currently mounted.
     *
     * Parses /proc/mounts to see if the device path appears as a mounted device.
     *
     * @param device_path Path to the device.
     * @return true if the device is listed in /proc/mounts.
     */
    bool check_mounted(const std::string& device_path) const;

    /**
     * @brief Get all mount points for the given device.
     *
     * @param device_path Path to the device.
     * @return Vector of mount point paths (empty if not mounted).
     */
    std::vector<std::string> get_mount_points(const std::string& device_path) const;

    /**
     * @brief Verify exclusive read-only access to the device.
     *
     * Attempts to open the device with O_RDONLY | O_EXCL.
     *
     * @param device_path Path to the device.
     * @return true if the open succeeded (exclusive read-only access obtained).
     */
    bool verify_read_only(const std::string& device_path) const;

    /**
     * @brief Safely open a device for reading with safety checks.
     *
     * Runs the following checks before opening:
     * 1. If it is a loopback device, logs the backing file.
     * 2. Checks if the device is mounted (warns if so).
     * 3. Opens with O_RDONLY only (not O_EXCL, to avoid failing on
     *    already-open devices).
     *
     * @param device_path Path to the device.
     * @return File descriptor on success, or -1 with errno set on error.
     */
    int safe_open(const std::string& device_path) const;

private:
    /**
     * @brief Extract the short device name from a path like /dev/loop0.
     *
     * @param device_path Full path.
     * @return Short name (e.g., "loop0").
     */
    static std::string device_name_from_path(const std::string& device_path);

    /**
     * @brief Read the contents of a sysfs file as a string.
     *
     * @param path Full path to the sysfs file.
     * @return Contents with trailing whitespace trimmed, or empty on error.
     */
    static std::string read_sysfs_file(const std::string& path);
};

} // namespace safety
} // namespace dbrecovery

#endif // DBRECOVERY_SAFETY_LOOPBACK_SAFETY_HPP