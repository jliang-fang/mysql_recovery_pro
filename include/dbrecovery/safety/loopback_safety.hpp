#ifndef DBRECOVERY_SAFETY_LOOPBACK_SAFETY_HPP
#define DBRECOVERY_SAFETY_LOOPBACK_SAFETY_HPP

#include <string>
#include <vector>
#include <system_error>
#include <sys/types.h>
#include <sys/stat.h>

namespace dbrecovery {
namespace safety {

struct BlockDeviceId {
    unsigned int major = 0;
    unsigned int minor = 0;

    bool operator==(const BlockDeviceId& other) const {
        return major == other.major && minor == other.minor;
    }
};

struct MountTopologyReport {
    bool complete = false;
    std::vector<BlockDeviceId> related_devices;
    std::vector<std::string> mount_points;
    std::string error;

    bool mounted() const { return !mount_points.empty(); }
};

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
     * @brief Inspect mounted and dependent devices from an admitted descriptor.
     *
     * The result is complete only when mountinfo and the complete sysfs device
     * relationship closure were read successfully. Callers must fail closed
     * whenever complete is false.
     */
    MountTopologyReport inspect_mount_topology(const struct stat& device_stat) const;

    /**
     * @brief Testable mount topology inspection using explicit proc/sysfs roots.
     */
    MountTopologyReport inspect_mount_topology_for_paths(
        const BlockDeviceId& device,
        const std::string& mountinfo_path,
        const std::string& sysfs_dev_block_root) const;

    /**
     * @brief Check if the device is currently mounted for diagnostics only.
     *
     * This pathname helper is not an authorization primitive.
     */
    bool check_mounted(const std::string& device_path) const;

    /**
     * @brief Get mount points for diagnostics only.
     *
     * This pathname helper is not an authorization primitive.
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