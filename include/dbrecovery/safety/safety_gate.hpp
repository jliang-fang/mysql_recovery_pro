#ifndef DBRECOVERY_SAFETY_SAFETY_GATE_HPP
#define DBRECOVERY_SAFETY_SAFETY_GATE_HPP

#include "dbrecovery/safety/device_identity.hpp"
#include "dbrecovery/safety/loopback_safety.hpp"
#include "dbrecovery/safety/write_protect.hpp"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <cstdint>
#include <sys/types.h>
#include <sys/stat.h>

namespace dbrecovery {
namespace safety {

/**
 * @brief Result of a full safety gate verification.
 */
struct SafetyReport {
    bool device_identity_verified = false;
    bool read_only_confirmed = false;
    bool exclusive_access = false;
    bool not_mounted_or_ok = false;
    bool requires_loopback_verification = false;
    bool loopback_verified = false;
    std::string backing_file;
    std::string model;
    std::string serial;
    double confidence = 0.0;
    std::vector<std::string> warnings;
};

class AuthorizedRawDevice {
public:
    AuthorizedRawDevice() = default;
    ~AuthorizedRawDevice();

    AuthorizedRawDevice(const AuthorizedRawDevice&) = delete;
    AuthorizedRawDevice& operator=(const AuthorizedRawDevice&) = delete;
    AuthorizedRawDevice(AuthorizedRawDevice&& other) noexcept;
    AuthorizedRawDevice& operator=(AuthorizedRawDevice&& other) noexcept;

    bool valid() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    int duplicate_fd() const;
    uint64_t size_bytes() const { return size_bytes_; }
    const struct stat& device_stat() const { return device_stat_; }
    const SafetyReport& report() const { return report_; }

private:
    friend class SafetyGate;
    AuthorizedRawDevice(int fd, struct stat device_stat, uint64_t size_bytes,
                        SafetyReport report);

    int fd_ = -1;
    struct stat device_stat_ {};
    uint64_t size_bytes_ = 0;
    SafetyReport report_;
};

/**
 * @brief SafetyGate: Orchestrates all safety checks for bare device access.
 *
 * Runs the full verification pipeline:
 * 1. Device identity verification (NVMe ioctl or sysfs)
 * 2. Write-protect verification (BLKROGET, O_EXCL, sysfs)
 * 3. Mount status check
 * 4. Loopback safety check
 *
 * Usage:
 * @code
 *   SafetyGate gate;
 *   SafetyReport report = gate.verify_device("/dev/nvme0n1");
 *   if (SafetyGate::permits_read_only_raw_access(report)) {
 *       // Safe to proceed with read-only recovery
 *   }
 * @endcode
 */
class SafetyGate {
public:
    /**
     * @brief Log callback type. Receives log messages during verification.
     */
    using LogCallback = std::function<void(const std::string&)>;

    SafetyGate();
    ~SafetyGate() = default;

    // Movable, non-copyable
    SafetyGate(const SafetyGate&) = delete;
    SafetyGate& operator=(const SafetyGate&) = delete;
    SafetyGate(SafetyGate&&) = default;
    SafetyGate& operator=(SafetyGate&&) = default;

    /**
     * @brief Return whether all mandatory read-only raw-access facts are proven.
     */
    static bool permits_read_only_raw_access(const SafetyReport& report);

    /**
     * @brief Set a callback for log messages.
     *
     * @param cb Callback function that receives log strings.
     */
    void set_log_callback(LogCallback cb);

    /**
     * @brief Run the full safety verification pipeline on a device.
     *
     * Executes all checks in sequence:
     * - Detects device type and identity
     * - Verifies write-protection
     * - Checks mount status
     * - Verifies loopback backing file
     *
     * @param device_path Path to the block device.
     * @return SafetyReport with all verification results.
     */
    SafetyReport verify_device(const std::string& device_path);

    /**
     * @brief Authorize a block device and retain the exact descriptor to scan.
     *
     * The caller must use this capability (or a duplicate from it) for all
     * subsequent reads. Failure returns no descriptor and provides a reason.
     */
    std::optional<AuthorizedRawDevice> authorize_read_only_raw_device(
        const std::string& device_path, std::string* reason = nullptr);

private:
    /**
     * @brief Internal log method. Calls the callback if set, otherwise
     *        writes to stderr.
     */
    void log(const std::string& message);

    DeviceIdentity identity_;
    LoopbackSafety loopback_;
    LogCallback log_callback_;
};

} // namespace safety
} // namespace dbrecovery

#endif // DBRECOVERY_SAFETY_SAFETY_GATE_HPP