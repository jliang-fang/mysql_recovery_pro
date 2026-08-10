/**
 * @file unified_validator.hpp
 * @brief UnifiedValidator — integrated validation of recovery capabilities
 *
 * Phase 7: Runs SDI extraction → page type detection → salvage (L1-L4) →
 * capability report against a target MySQL version, producing a CapabilityReport
 * indicating which features this build supports or rejects for that version.
 */

#ifndef DBRECOVERY_VALIDATE_UNIFIED_VALIDATOR_HPP
#define DBRECOVERY_VALIDATE_UNIFIED_VALIDATOR_HPP

#include <string>
#include <vector>

#include "dbrecovery/common/version_compat.hpp"
#include "dbrecovery/report/recovery_report.hpp"

namespace dbrecovery {
namespace validate {

class UnifiedValidator {
public:
    UnifiedValidator() = default;

    /**
     * @brief Static entry point — validate all capabilities for a version.
     */
    static report::CapabilityReport validate_all(MySQLVersion version);

    /**
     * @brief Instance method using the configured version.
     */
    report::CapabilityReport execute() const;

    void set_version(MySQLVersion version) { version_ = version; }
    MySQLVersion version() const { return version_; }

private:
    MySQLVersion version_ = MySQLVersion::UNKNOWN;

    report::CapabilityReport build_capability_report() const;
};

} // namespace validate
} // namespace dbrecovery

#endif // DBRECOVERY_VALIDATE_UNIFIED_VALIDATOR_HPP