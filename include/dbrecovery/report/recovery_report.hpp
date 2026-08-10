#ifndef DBRECOVERY_REPORT_RECOVERY_REPORT_HPP
#define DBRECOVERY_REPORT_RECOVERY_REPORT_HPP

/**
 * @file recovery_report.hpp
 * @brief Professional recovery report system (v2.0)
 *
 * Generates detailed JSON/Text/HTML reports after each recovery operation.
 * Includes execution summary, data statistics, strategy documentation,
 * damage analysis, failure analysis, and reconstructed DDL.
 *
 * Note: Implementations moved to src/report/recovery_report.cpp
 */

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>

#include "dbrecovery/common/version_compat.hpp"

namespace dbrecovery {
namespace report {

// ==================== Report Structures ====================

struct InputInfo {
    std::string source_type;
    std::string source_path;
    uint64_t source_size_bytes = 0;
    bool has_schema = false;
    std::string schema_source;
    bool has_undo_logs = false;
    bool has_redo_logs = false;
    std::vector<std::string> files_processed;
};

struct StrategyInfo {
    std::vector<std::string> strategies_attempted;
    std::string primary_strategy;
    std::vector<std::string> fallback_strategies;
    std::map<std::string, std::string> parameters;
};

struct DataStats {
    uint64_t total_pages_scanned = 0;
    uint64_t index_pages_found = 0;
    uint64_t blob_pages_found = 0;
    uint64_t corrupted_pages = 0;
    uint64_t skipped_pages = 0;
    uint64_t total_records_attempted = 0;
    uint64_t records_fully_recovered = 0;
    uint64_t records_partially_recovered = 0;
    uint64_t records_failed = 0;
    double recovery_rate_percent = 0.0;
    uint64_t blob_records_recovered = 0;
    uint64_t history_versions_recovered = 0;
    uint64_t flashback_statements_generated = 0;
    std::map<std::string, uint64_t> records_by_type;
};

struct DamageDetail {
    uint32_t page_no;
    std::string damage_type;
    std::string description;
};

struct DamageAnalysis {
    uint32_t pages_with_header_damage = 0;
    uint32_t pages_with_checksum_mismatch = 0;
    uint32_t pages_with_record_corruption = 0;
    uint32_t pages_with_blob_corruption = 0;
    std::vector<DamageDetail> details;
};

struct FailureCategory {
    std::string category;
    uint64_t count = 0;
    double percentage = 0.0;
    std::string recommendation;
};

struct FailureAnalysis {
    std::vector<FailureCategory> categories;
};

struct OutputInfo {
    std::string output_directory;
    std::vector<std::string> output_files;
    uint64_t total_output_size_bytes = 0;
    std::string format;
};

struct TimingInfo {
    double scan_time_seconds = 0.0;
    double parse_time_seconds = 0.0;
    double export_time_seconds = 0.0;
    double total_time_seconds = 0.0;
};

// ==================== Capability Report ====================

/**
 * @brief CapabilityReport — what this build can/can't recover for a given version.
 *
 * Surfaces the recovery capability boundaries (SDI extraction, page type
 * recovery, salvage levels, encryption/compression) so users can see at a
 * glance which features are supported for the detected MySQL version.
 */
struct CapabilityReport {
    struct Feature {
        std::string name;
        bool supported;
        std::string limitation;

        std::string to_string() const {
            return name + ": " + (supported ? "SUPPORTED" : "NOT SUPPORTED")
                + (limitation.empty() ? "" : " (" + limitation + ")");
        }
    };

    std::string mysql_version;
    dbrecovery::MySQLVersion version_id = dbrecovery::MySQLVersion::UNKNOWN;
    std::vector<Feature> features;
    std::vector<std::string> warnings;

    bool is_feature_supported(const std::string& name) const {
        for (const auto& f : features) {
            if (f.name == name) return f.supported;
        }
        return false;
    }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "=== Capability Report ===\n"
            << "MySQL version: " << (mysql_version.empty() ? "UNKNOWN" : mysql_version) << "\n"
            << "Features:\n";
        for (const auto& f : features) {
            oss << "  " << f.to_string() << "\n";
        }
        if (!warnings.empty()) {
            oss << "Warnings:\n";
            for (const auto& w : warnings) {
                oss << "  - " << w << "\n";
            }
        }
        return oss.str();
    }
};

struct RecoveryReport {
    std::string report_id;
    std::string report_time;
    std::string recovery_type;
    std::string mysql_version;
    std::string overall_status;

    InputInfo input;
    StrategyInfo strategy;
    DataStats data;
    DamageAnalysis damage;
    FailureAnalysis failures;
    std::vector<std::string> reconstructed_ddl;
    OutputInfo output;
    TimingInfo timing;

    static std::string generate_report_id();
    static std::string format_time(const std::chrono::system_clock::time_point& tp);

    std::string to_json() const;
    std::string to_text() const;
    std::string to_html() const;
    std::vector<std::string> write_files(const std::string& base_path, const std::string& formats) const;
};

// ==================== Report Builder ====================

class ReportBuilder {
public:
    ReportBuilder();

    ReportBuilder& set_recovery_type(const std::string& type);
    ReportBuilder& set_mysql_version(const std::string& ver);
    ReportBuilder& set_status(const std::string& status);
    ReportBuilder& set_input(const InputInfo& input);
    ReportBuilder& set_strategy(const StrategyInfo& strategy);
    ReportBuilder& set_data(const DataStats& data);
    ReportBuilder& add_damage(const DamageDetail& detail);
    ReportBuilder& add_failure(const std::string& category, uint64_t count,
                                double pct, const std::string& recommendation);
    ReportBuilder& add_ddl(const std::string& ddl);
    ReportBuilder& set_output(const OutputInfo& output);
    ReportBuilder& set_timing(const TimingInfo& timing);
    RecoveryReport build();

private:
    RecoveryReport report_;
};

} // namespace report
} // namespace dbrecovery

#endif // DBRECOVERY_REPORT_RECOVERY_REPORT_HPP