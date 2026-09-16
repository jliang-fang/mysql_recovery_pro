#include "dbrecovery/safety/safety_gate.hpp"

#include <iostream>
#include <sstream>

namespace dbrecovery {
namespace safety {

SafetyGate::SafetyGate() = default;

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
    auto mount_points = loopback_.get_mount_points(device_path);
    if (mount_points.empty()) {
        report.not_mounted_or_ok = true;
        log("  Device is NOT mounted - OK");
    } else {
        std::ostringstream oss;
        oss << "Device is mounted at:";
        for (const auto& mp : mount_points) {
            oss << " " << mp;
        }
        std::string warning = oss.str();
        log("  WARNING: " + warning);
        report.warnings.push_back(warning);
        // not_mounted_or_ok stays false since the device is mounted;
        // recovery from a mounted device may still be possible but risky
        report.not_mounted_or_ok = false;
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

void SafetyGate::log(const std::string& message) {
    if (log_callback_) {
        log_callback_(message);
    } else {
        std::clog << message << std::endl;
    }
}

} // namespace safety
} // namespace dbrecovery