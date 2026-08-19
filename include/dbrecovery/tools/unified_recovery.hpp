#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "dbrecovery/common/damage_level.hpp"
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/report/recovery_report.hpp"

namespace dbrecovery {
namespace tools {

struct UnifiedRecoveryResult {
    bool success = false;
    std::vector<recovery::SalvagedRow> records;
    uint32_t total_recovered = 0;
    std::string schema_source = "EVIDENCE_ONLY";
    std::string trust_level = "EVIDENCE_ONLY";
    common::DamageLevel damage_level = common::DamageLevel::NONE;
    std::vector<recovery::SalvagedRow> evidence_fragments;
    std::string recovery_method = "none";
    report::RecoveryReport report;
    std::string result_classification = "forensic_evidence";
    bool ddl_variation_detected = false;
};

UnifiedRecoveryResult run_unified_recovery(
    const std::string& ibd_path,
    const std::string& output_dir,
    const std::string& table_name,
    const std::string& format,
    uint32_t page_size,
    const std::string& schema_str,
    const std::string& frm_path,
    uint64_t table_id,
    const std::string& system_tablespace,
    bool repair,
    bool schemaless_recovery,
    bool verbose);

bool write_unified_result_json(const std::string& output_dir,
                               const std::string& table_name,
                               const UnifiedRecoveryResult& result);

} // namespace tools
} // namespace dbrecovery
