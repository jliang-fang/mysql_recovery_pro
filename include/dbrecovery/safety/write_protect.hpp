#ifndef DBRECOVERY_SAFETY_WRITE_PROTECT_HPP
#define DBRECOVERY_SAFETY_WRITE_PROTECT_HPP

#include <string>
#include <cstdint>

namespace dbrecovery {
namespace safety {

/**
 * @brief WriteProtectGuard: RAII guard that verifies and maintains
 *        read-only access to a block device during recovery operations.
 *
 * On construction, the guard opens the device and checks write-protect status.
 * On destruction, it closes the file descriptor and logs the release.
 */
class WriteProtectGuard {
public:
    /**
     * @brief Construct the guard and open the device for read-only access.
     *
     * @param device_path Path to the block device.
     * @throws std::system_error if the device cannot be opened or if
     *         write-protect checks fail.
     */
    explicit WriteProtectGuard(const std::string& device_path);

    /**
     * @brief Destroy the guard, close the fd, and log release.
     */
    ~WriteProtectGuard();

    // Movable, non-copyable
    WriteProtectGuard(const WriteProtectGuard&) = delete;
    WriteProtectGuard& operator=(const WriteProtectGuard&) = delete;
    WriteProtectGuard(WriteProtectGuard&& other) noexcept;
    WriteProtectGuard& operator=(WriteProtectGuard&& other) noexcept;

    /**
     * @brief Check if the device is write-protected at the kernel level.
     *
     * Uses BLKROGET ioctl to query the read-only flag.
     *
     * @return true if the device is read-only.
     */
    bool is_write_protected() const;

    /**
     * @brief Check if the device is opened exclusively.
     *
     * Returns true if the initial O_RDONLY | O_EXCL open succeeded.
     *
     * @return true if exclusive access was obtained.
     */
    bool is_exclusively_open() const { return exclusive_; }

    /**
     * @brief Re-open the device without O_EXCL if exclusive access failed.
     *
     * This is a fallback for devices that are already in use. The device
     * is still opened O_RDONLY.
     *
     * @return true if re-open succeeded.
     */
    bool force_read_only();

    /**
     * @brief Get the device path.
     *
     * @return Const reference to the device path string.
     */
    const std::string& device_path() const { return device_path_; }

    /**
     * @brief Get the file descriptor.
     *
     * @return The fd, or -1 if not open.
     */
    int fd() const { return fd_; }

    /**
     * @brief Check if the guard holds a valid open fd.
     */
    bool is_open() const { return fd_ >= 0; }

private:
    /**
     * @brief Query the kernel block device read-only flag from sysfs.
     *
     * Reads /sys/block/<dev>/ro. Returns 0 if writable, 1 if read-only, -1 on error.
     */
    int query_sysfs_read_only() const;

    /**
     * @brief Log a message (writes to stderr).
     */
    static void log(const std::string& message);

    std::string device_path_;
    std::string device_name_;
    int fd_ = -1;
    bool exclusive_ = false;
};

} // namespace safety
} // namespace dbrecovery

#endif // DBRECOVERY_SAFETY_WRITE_PROTECT_HPP