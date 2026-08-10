/**
 * @file unified_validator.cpp
 * @brief UnifiedValidator implementation
 */

#include "dbrecovery/validate/unified_validator.hpp"

#include "dbrecovery/common/version_compat.hpp"

#include <sstream>

namespace dbrecovery {
namespace validate {

using dbrecovery::report::CapabilityReport;

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

    auto add = [&](const std::string& name, bool supported, const std::string& lim = "") {
        report.features.push_back({name, supported, lim});
    };

    // For UNKNOWN version, report conservative defaults
    if (version_ == MySQLVersion::UNKNOWN) {
        add("SDI extraction", false, "MySQL 8.0+ required");
        add("TDE/page encryption", false, "unsupported for this version");
        add("Page compression", false, "unsupported for this version");
        add("Temporary pages", false, "MySQL 8.4+ required");

        static const std::vector<PageType> types = {
            PageType::INDEX, PageType::SYSTEM, PageType::UNDO_LOG, PageType::SDI,
            PageType::BLOB, PageType::FSP_HDR, PageType::XDES,
            PageType::COMPRESSED, PageType::COMPRESSED_AND_ENCRYPTED, PageType::TEMPORARY
        };
        for (PageType t : types) {
            const PageTypeInfo& ti = factory.get_page_type_info(t);
            add("page_type:" + ti.name, false, "recovery unavailable — version is UNKNOWN");
        }

        add("salvage L1 (header repair)", true, "");
        add("salvage L2 (signature scan)", true, "");
        add("salvage L3 (heuristic scan)", true, "evidence tagged 'heuristic/partial'");
        add("salvage L4 (fragment extract)", true, "evidence tagged 'heuristic/fragment'");

        report.warnings.push_back("Version is UNKNOWN — capability assertions are conservative");
        return report;
    }

    const MySQLVersionInfo& info = factory.get_version_info(version_);

    // SDI extraction (MySQL 8.0+)
    bool sdi = info.supports_sdi;
    add("SDI extraction", sdi, sdi ? "" : "MySQL 8.0+ required");

    // Encryption
    bool enc = info.supports_encryption;
    add("TDE/page encryption", enc, enc ? "" : "unsupported for this version");

    // Compression
    bool comp = info.supports_compression;
    add("Page compression", comp, comp ? "" : "unsupported for this version");

    // Temporary pages (MySQL 8.4+)
    bool temp = info.supports_temporary_pages;
    add("Temporary pages", temp, temp ? "" : "MySQL 8.4+ required");

    // Page type recovery capability matrix
    static const std::vector<PageType> types = {
        PageType::INDEX, PageType::SYSTEM, PageType::UNDO_LOG, PageType::SDI,
        PageType::BLOB, PageType::FSP_HDR, PageType::XDES,
        PageType::COMPRESSED, PageType::COMPRESSED_AND_ENCRYPTED, PageType::TEMPORARY
    };
    for (PageType t : types) {
        const PageTypeInfo& ti = factory.get_page_type_info(t);
        bool supported = factory.can_recover_page_type(t, version_);
        std::string lim = supported ? "" : "recovery unavailable for this version";
        add("page_type:" + ti.name, supported, lim);
    }

    // Salvage levels (L1-L4) are a build capability, available for all versions
    add("salvage L1 (header repair)", true, "");
    add("salvage L2 (signature scan)", true, "");
    add("salvage L3 (heuristic scan)", true, "evidence tagged 'heuristic/partial'");
    add("salvage L4 (fragment extract)", true, "evidence tagged 'heuristic/fragment'");

    // Version range support
    if (!factory.is_version_supported(version_)) {
        report.warnings.push_back("MySQL version " + factory.version_to_string(version_)
            + " is outside the supported range");
    }
    if (version_ == MySQLVersion::UNKNOWN) {
        report.warnings.push_back("Version is UNKNOWN — capability assertions are conservative");
    }

    return report;
}

report::CapabilityReport UnifiedValidator::execute() const {
    return build_capability_report();
}

} // namespace validate
} // namespace dbrecovery