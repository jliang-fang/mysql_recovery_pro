/**
 * @file unified_validator.cpp
 * @brief UnifiedValidator implementation
 */

#include "dbrecovery/validate/unified_validator.hpp"

#include "dbrecovery/common/version_compat.hpp"

#include <sstream>
#include <utility>

namespace dbrecovery {
namespace validate {

using dbrecovery::report::CapabilityReport;
using dbrecovery::report::CapabilityStatus;
using dbrecovery::report::RecoveryOutputCategory;

report::CapabilityReport UnifiedValidator::validate_all(MySQLVersion version) {
    UnifiedValidator validator;
    validator.set_version(version);
    return validator.execute();
}

report::CapabilityReport UnifiedValidator::build_capability_report() const {
    const auto& factory = VersionCompatibilityFactory::instance();

    CapabilityReport report;
    report.mysql_version = factory.version_to_string(version_);
    report.version_id = version_;

    auto add = [&](const std::string& id, const std::string& name,
                   CapabilityStatus status, RecoveryOutputCategory output_category,
                   const std::string& limitation = "",
                   std::vector<std::string> evidence = {}) {
        report.features.push_back({
            name,
            status == CapabilityStatus::SUPPORTED,
            limitation,
            id,
            status,
            output_category,
            std::move(evidence),
        });
    };

    auto add_page_type = [&](PageType type, CapabilityStatus status,
                             RecoveryOutputCategory output_category,
                             const std::string& limitation) {
        const PageTypeInfo& info = factory.get_page_type_info(type);
        add("page_type." + info.name, "page_type:" + info.name, status,
            output_category, limitation, {"version compatibility matrix"});
    };

    // A version alone is insufficient proof of a recoverable physical format.
    if (version_ == MySQLVersion::UNKNOWN) {
        add("sdi.extraction", "SDI extraction", CapabilityStatus::REJECTED,
            RecoveryOutputCategory::FORENSIC_EVIDENCE,
            "version evidence is unresolved");
        add("tde.page_encryption", "TDE/page encryption",
            CapabilityStatus::RECOGNIZED_UNSUPPORTED,
            RecoveryOutputCategory::FORENSIC_EVIDENCE,
            "native keyring and tablespace decryption are not format-verified");
        add("compression.page", "Page compression",
            CapabilityStatus::RECOGNIZED_UNSUPPORTED,
            RecoveryOutputCategory::FORENSIC_EVIDENCE,
            "compressed-page decoding is not format-verified");
        add("distributed.recovery", "Distributed recovery",
            CapabilityStatus::RECOGNIZED_UNSUPPORTED,
            RecoveryOutputCategory::FORENSIC_EVIDENCE,
            "distributed/remote tablespace recovery is not implemented; "
            "this tool operates on a single .ibd file at a time");
        add("temporary.pages", "Temporary pages", CapabilityStatus::REJECTED,
            RecoveryOutputCategory::FORENSIC_EVIDENCE,
            "version evidence is unresolved");

        static const std::vector<PageType> types = {
            PageType::INDEX, PageType::SYSTEM, PageType::UNDO_LOG, PageType::SDI,
            PageType::BLOB, PageType::FSP_HDR, PageType::XDES,
            PageType::COMPRESSED, PageType::COMPRESSED_AND_ENCRYPTED, PageType::TEMPORARY
        };
        for (PageType type : types) {
            add_page_type(type, CapabilityStatus::REJECTED,
                          RecoveryOutputCategory::FORENSIC_EVIDENCE,
                          "recovery unavailable — version is UNKNOWN");
        }

        add("salvage.l1", "salvage L1 (header repair)", CapabilityStatus::EVIDENCE_ONLY,
            RecoveryOutputCategory::PAGE_BUNDLE,
            "page repairs require independent validation before structured export");
        add("salvage.l2", "salvage L2 (signature scan)", CapabilityStatus::EVIDENCE_ONLY,
            RecoveryOutputCategory::FORENSIC_EVIDENCE,
            "signature candidates are not structured rows");
        add("salvage.l3", "salvage L3 (heuristic scan)", CapabilityStatus::EVIDENCE_ONLY,
            RecoveryOutputCategory::FORENSIC_EVIDENCE,
            "heuristic candidates are not structured rows");
        add("salvage.l4", "salvage L4 (fragment extract)", CapabilityStatus::EVIDENCE_ONLY,
            RecoveryOutputCategory::FORENSIC_EVIDENCE,
            "fragments are not structured rows");

        report.warnings.push_back("Version is UNKNOWN — physical recovery is rejected unless separately evidenced");
        return report;
    }

    const MySQLVersionInfo& info = factory.get_version_info(version_);
    const std::string version_evidence = "declared MySQL version " + report.mysql_version;

    add("sdi.extraction", "SDI extraction",
        info.supports_sdi ? CapabilityStatus::EVIDENCE_ONLY : CapabilityStatus::REJECTED,
        RecoveryOutputCategory::FORENSIC_EVIDENCE,
        info.supports_sdi
            ? "schema authority requires validated SDI origin and JSON integrity"
            : "MySQL 8.0+ required",
        {version_evidence});

    add("tde.page_encryption", "TDE/page encryption",
        CapabilityStatus::RECOGNIZED_UNSUPPORTED,
        RecoveryOutputCategory::FORENSIC_EVIDENCE,
        info.supports_encryption
            ? "native keyring and tablespace decryption are not format-verified"
            : "unsupported by the declared MySQL version",
        {version_evidence});

    add("compression.page", "Page compression",
        CapabilityStatus::RECOGNIZED_UNSUPPORTED,
        RecoveryOutputCategory::FORENSIC_EVIDENCE,
        info.supports_compression
            ? "compressed-page decoding is not format-verified"
            : "unsupported by the declared MySQL version",
        {version_evidence});

    add("distributed.recovery", "Distributed recovery",
        CapabilityStatus::RECOGNIZED_UNSUPPORTED,
        RecoveryOutputCategory::FORENSIC_EVIDENCE,
        "distributed/remote tablespace recovery is not implemented; "
        "this tool operates on a single .ibd file at a time",
        {version_evidence});

    add("temporary.pages", "Temporary pages",
        info.supports_temporary_pages ? CapabilityStatus::RECOGNIZED_UNSUPPORTED
                                      : CapabilityStatus::REJECTED,
        RecoveryOutputCategory::FORENSIC_EVIDENCE,
        info.supports_temporary_pages
            ? "temporary-page recovery is not format-verified"
            : "MySQL 8.4+ required",
        {version_evidence});

    static const std::vector<PageType> types = {
        PageType::INDEX, PageType::SYSTEM, PageType::UNDO_LOG, PageType::SDI,
        PageType::BLOB, PageType::FSP_HDR, PageType::XDES,
        PageType::COMPRESSED, PageType::COMPRESSED_AND_ENCRYPTED, PageType::TEMPORARY
    };
    for (PageType type : types) {
        const bool compatible = factory.can_recover_page_type(type, version_);
        const bool specialized = type == PageType::SDI || type == PageType::COMPRESSED
            || type == PageType::COMPRESSED_AND_ENCRYPTED || type == PageType::TEMPORARY;
        const CapabilityStatus status = !compatible ? CapabilityStatus::REJECTED
            : specialized ? CapabilityStatus::EVIDENCE_ONLY : CapabilityStatus::SUPPORTED;
        add_page_type(type, status,
            status == CapabilityStatus::SUPPORTED ? RecoveryOutputCategory::PAGE_BUNDLE
                                                  : RecoveryOutputCategory::FORENSIC_EVIDENCE,
            !compatible ? "recovery unavailable for the declared version"
            : specialized ? "format-specific validation is required before structured export" : "");
    }

    add("salvage.l1", "salvage L1 (header repair)", CapabilityStatus::EVIDENCE_ONLY,
        RecoveryOutputCategory::PAGE_BUNDLE,
        "page repairs require independent validation before structured export");
    add("salvage.l2", "salvage L2 (signature scan)", CapabilityStatus::EVIDENCE_ONLY,
        RecoveryOutputCategory::FORENSIC_EVIDENCE,
        "signature candidates are not structured rows");
    add("salvage.l3", "salvage L3 (heuristic scan)", CapabilityStatus::EVIDENCE_ONLY,
        RecoveryOutputCategory::FORENSIC_EVIDENCE,
        "heuristic candidates are not structured rows");
    add("salvage.l4", "salvage L4 (fragment extract)", CapabilityStatus::EVIDENCE_ONLY,
        RecoveryOutputCategory::FORENSIC_EVIDENCE,
        "fragments are not structured rows");

    if (!factory.is_version_supported(version_)) {
        report.warnings.push_back("MySQL version " + factory.version_to_string(version_)
            + " is outside the supported range");
    }

    return report;
}

report::CapabilityReport UnifiedValidator::execute() const {
    return build_capability_report();
}

} // namespace validate
} // namespace dbrecovery