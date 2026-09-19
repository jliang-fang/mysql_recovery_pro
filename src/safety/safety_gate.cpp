#include "dbrecovery/safety/safety_gate.hpp"

#include <iostream>
#include <sstream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/fs.h>
#endif

namespace dbrecovery {
namespace safety {

SafetyGate::SafetyGate() = default;

AuthorizedRawDevice::AuthorizedRawDevice(int fd, struct stat device_stat,
                                         uint64_t size_bytes, SafetyReport report)
    : fd_(fd), device_stat_(device_stat), size_bytes_(size_bytes),
      report_(std::move(report)) {}

AuthorizedRawDevice::~AuthorizedRawDevice() {
    if (fd_ >= 0) ::close(fd_);
}

AuthorizedRawDevice::AuthorizedRawDevice(AuthorizedRawDevice&& other) noexcept
    : fd_(other.fd_), device_stat_(other.device_stat_), size_bytes_(other.size_bytes_),
      report_(std::move(other.report_)) {
    other.fd_ = -1;
    other.size_bytes_ = 0;
}

AuthorizedRawDevice& AuthorizedRawDevice::operator=(AuthorizedRawDevice&& other) noexcept {
    if (this == &other) return *this;
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    device_stat_ = other.device_stat_;
    size_bytes_ = other.size_bytes_;
    report_ = std::move(other.report_);
    other.fd_ = -1;
    other.size_bytes_ = 0;
    return *this;
}

int AuthorizedRawDevice::duplicate_fd() const {
    if (fd_ < 0) return -1;
#ifdef F_DUPFD_CLOEXEC
    return ::fcntl(fd_, F_DUPFD_CLOEXEC, 0);
#else
    const int duplicate = ::dup(fd_);
    if (duplicate >= 0) ::fcntl(duplicate, F_SETFD, FD_CLOEXEC);
    return duplicate;
#endif
}

bool SafetyGate::permits_read_only_raw_access(const SafetyReport& report) {
    return report.device_identity_verified &&
           report.read_only_confirmed &&
           report.exclusive_access &&
           report.not_mounted_or_ok &&
           (!report.requires_loopback_verification || report.loopback_verified);
}

void SafetyGate::set_log_callback(LogCallback cb) {
    log_callback_ = std::move(cb);
}

SafetyReport SafetyGate::verify_device(const std::string& device_path) {
    SafetyReport report;

    log("=== Safety Gate Verification for " + device_path + " ===");

    // ---------------------------------------------------------------
    // Step 1: Device Identity Verification
    // ---------------------------------------------------------------
    log("[Step 1/4] Verifying device identity...");
    DeviceIdentityInfo identity = identity_.verify_identity(device_path);

    report.device_identity_verified = (identity.confidence > 0.5);
    report.model = identity.model_number;
    report.serial = identity.serial_number;
    report.confidence = identity.confidence;

    if (report.device_identity_verified) {
        log("  Device type: " + identity.device_type);
        log("  Model: " + (identity.model_number.empty() ? "(unknown)" : identity.model_number));
        log("  Serial: " + (identity.serial_number.empty() ? "(unknown)" : identity.serial_number));
        log("  Firmware: " + (identity.firmware_revision.empty() ? "(unknown)" : identity.firmware_revision));
        log("  Confidence: " + std::to_string(identity.confidence));
    } else {
        std::string warning = "Device identity verification failed (confidence=" +
                              std::to_string(identity.confidence) + ")";
        log("  WARNING: " + warning);
        report.warnings.push_back(warning);
    }

    // ---------------------------------------------------------------
    // Step 2: Write-Protection Verification
    // ---------------------------------------------------------------
    log("[Step 2/4] Verifying write-protection...");
    try {
        WriteProtectGuard guard(device_path);
        report.read_only_confirmed = guard.is_write_protected();
        report.exclusive_access = guard.is_exclusively_open();

        if (report.read_only_confirmed) {
            log("  Write-protect: CONFIRMED (BLKROGET=1)");
        } else {
            std::string warning = "Device is NOT write-protected at kernel level";
            log("  WARNING: " + warning);
            report.warnings.push_back(warning);
        }

        if (report.exclusive_access) {
            log("  Exclusive access: YES");
        } else {
            log("  Exclusive access: NO (device may be in use by another process)");
        }
    } catch (const std::exception& e) {
        std::string warning = std::string("Write-protect check failed: ") + e.what();
        log("  ERROR: " + warning);
        report.warnings.push_back(warning);
        report.read_only_confirmed = false;
        report.exclusive_access = false;
    }

    // ---------------------------------------------------------------
    // Step 3: Mount Status Check
    // ---------------------------------------------------------------
    log("[Step 3/4] Checking mount status...");
    struct stat device_stat {};
    const bool inspected_device = ::stat(device_path.c_str(), &device_stat) == 0 &&
                                  S_ISBLK(device_stat.st_mode);
    const auto topology = inspected_device
        ? loopback_.inspect_mount_topology(device_stat)
        : MountTopologyReport{};
    report.not_mounted_or_ok = topology.complete && !topology.mounted();
    if (!topology.complete) {
        const std::string detail = topology.error.empty()
            ? "could not inspect a block-device identity"
            : topology.error;
        const std::string warning = "could not prove mounted-device topology is safe: " + detail;
        log("  WARNING: " + warning);
        report.warnings.push_back(warning);
    } else if (topology.mounted()) {
        std::ostringstream mounts;
        mounts << "Device or related block device is mounted at:";
        for (const auto& mount_point : topology.mount_points) {
            mounts << " " << mount_point;
        }
        const std::string warning = mounts.str();
        log("  WARNING: " + warning);
        report.warnings.push_back(warning);
    } else {
        log("  Device topology is not mounted - OK");
    }

    // ---------------------------------------------------------------
    // Step 4: Loopback Safety Check
    // ---------------------------------------------------------------
    log("[Step 4/4] Checking loopback status...");
    if (identity.device_type == "loop") {
        report.requires_loopback_verification = true;
        report.backing_file = loopback_.find_backing_file(device_path);
        // loopback_verified requires a discovered backing file that already
        // passed the identity policy inside DeviceIdentity (model/serial set
        // only for verified file-backed loops under a trusted root).
        if (!report.backing_file.empty() && report.device_identity_verified) {
            report.loopback_verified = true;
            log("  Loopback device backed by: " + report.backing_file);
        } else if (!report.backing_file.empty()) {
            std::string warning =
                "Loopback backing file failed identity policy: " + report.backing_file;
            log("  WARNING: " + warning);
            report.warnings.push_back(warning);
        } else {
            std::string warning = "Loopback device has no discoverable backing file";
            log("  WARNING: " + warning);
            report.warnings.push_back(warning);
        }
    } else {
        log("  Not a loopback device - OK");
    }

    // ---------------------------------------------------------------
    // Summary
    // ---------------------------------------------------------------
    log("=== Safety Gate Summary ===");
    log("  Device identity verified: " + std::string(report.device_identity_verified ? "YES" : "NO"));
    log("  Read-only confirmed:      " + std::string(report.read_only_confirmed ? "YES" : "NO"));
    log("  Exclusive access:         " + std::string(report.exclusive_access ? "YES" : "NO"));
    log("  Not mounted:              " + std::string(report.not_mounted_or_ok ? "YES" : "NO"));
    if (report.requires_loopback_verification) {
        log("  Loopback verified:        " + std::string(report.loopback_verified ? "YES" : "NO"));
    } else {
        log("  Loopback verified:        N/A (not a loop device)");
    }
    log("  Confidence:               " + std::to_string(report.confidence));

    if (report.warnings.empty()) {
        log("  Result: ALL CHECKS PASSED");
    } else {
        log("  Result: " + std::to_string(report.warnings.size()) + " warning(s)");
        for (size_t i = 0; i < report.warnings.size(); ++i) {
            log("    Warning " + std::to_string(i + 1) + ": " + report.warnings[i]);
        }
    }

    log("=== End of Safety Gate Verification ===");

    return report;
}

std::optional<AuthorizedRawDevice> SafetyGate::authorize_read_only_raw_device(
    const std::string& device_path, std::string* reason) {
    auto reject = [&](const std::string& message) -> std::optional<AuthorizedRawDevice> {
        if (reason) *reason = message;
        log("Authorization refused: " + message);
        return std::nullopt;
    };

    const int fd = ::open(device_path.c_str(), O_RDONLY | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return reject("could not exclusively open block device read-only: " +
                      std::string(std::strerror(errno)));
    }

    struct stat device_stat{};
    if (::fstat(fd, &device_stat) != 0) {
        const std::string message = "could not inspect authorized descriptor: " +
                                    std::string(std::strerror(errno));
        ::close(fd);
        return reject(message);
    }
    if (!S_ISBLK(device_stat.st_mode)) {
        ::close(fd);
        return reject("authorized source is not a block device");
    }

    SafetyReport report;
    const DeviceIdentityInfo identity =
        identity_.verify_identity(fd, device_path, device_stat);
    report.device_identity_verified = identity.confidence > 0.5;
    report.model = identity.model_number;
    report.serial = identity.serial_number;
    report.confidence = identity.confidence;
    if (!report.device_identity_verified) {
        report.warnings.push_back("device identity verification failed");
    }

#ifdef __linux__
    int read_only = 0;
    report.read_only_confirmed = ::ioctl(fd, BLKROGET, &read_only) == 0 && read_only != 0;
    uint64_t size_bytes = 0;
    if (::ioctl(fd, BLKGETSIZE64, &size_bytes) != 0 || size_bytes == 0) {
        const std::string message = "could not determine block-device capacity: " +
                                    std::string(std::strerror(errno));
        ::close(fd);
        return reject(message);
    }
#else
    const uint64_t size_bytes = 0;
    report.read_only_confirmed = false;
#endif
    report.exclusive_access = true;

    const auto topology = loopback_.inspect_mount_topology(device_stat);
    report.not_mounted_or_ok = topology.complete && !topology.mounted();
    if (!topology.complete) {
        report.warnings.push_back("could not prove mounted-device topology is safe: " +
                                  topology.error);
    } else if (topology.mounted()) {
        std::ostringstream mounts;
        mounts << "device or related block device is mounted at:";
        for (const auto& mount_point : topology.mount_points) {
            mounts << " " << mount_point;
        }
        report.warnings.push_back(mounts.str());
    }

    if (identity.device_type == "loop") {
        report.requires_loopback_verification = true;
        report.backing_file = loopback_.find_backing_file(device_path);
        report.loopback_verified = !report.backing_file.empty() &&
                                   report.device_identity_verified;
        if (!report.loopback_verified) {
            report.warnings.push_back("loopback backing file failed identity policy");
        }
    }

    if (!permits_read_only_raw_access(report)) {
        ::close(fd);
        return reject("mandatory raw-device safety facts were not proven");
    }

    if (reason) reason->clear();
    return AuthorizedRawDevice(fd, device_stat, size_bytes, std::move(report));
}

void SafetyGate::log(const std::string& message) {
    if (log_callback_) {
        log_callback_(message);
    } else {
        std::clog << message << std::endl;
    }
}

} // namespace safety
} // namespace dbrecovery