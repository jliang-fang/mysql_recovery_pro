#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "dbrecovery/common/damage_level.hpp"
#include "dbrecovery/recovery/salvage_engine.hpp"
#include "dbrecovery/report/recovery_report.hpp"

namespace dbrecovery {
namespace tools {

struct PageDamageEvidence {
    uint32_t page_no = 0;
    uint64_t page_offset = 0;
    uint16_t page_type = 0;
    uint32_t space_id = 0;
    bool fil_header_valid = false;
    bool page_header_valid = false;
    bool checksum_valid = false;
    bool trailer_mismatch = false;
    bool free_space_corrupt = false;
    common::DamageLevel damage_level = common::DamageLevel::NONE;
    std::string diagnosis;
};

struct UnifiedRecoveryResult {
    std::string table_name;
    std::string source_ibd;
    bool success = false;
    std::vector<recovery::SalvagedRow> records;
    uint32_t total_recovered = 0;
    std::string schema_source = "EVIDENCE_ONLY";
    std::string trust_level = "EVIDENCE_ONLY";
    common::DamageLevel damage_level = common::DamageLevel::NONE;
    std::vector<PageDamageEvidence> page_damage;
    std::vector<recovery::SalvagedRow> evidence_fragments;
    std::string recovery_method = "none";
    std::vector<std::string> column_names;
    /// Parallel to column_names: true for columns whose bytes must be hex
    /// encoded on export (BINARY/VARBINARY/BLOB and their text aliases).
    std::vector<bool> column_is_binary;
    report::RecoveryReport report;
    std::string result_classification = "forensic_evidence";
    bool ddl_variation_detected = false;
    bool export_success = false;
    std::string export_rejection_reason;
    std::string compression_status = "NOT_COMPRESSED";
    uint64_t logical_page_size = 0;
    uint64_t physical_page_size = 0;
    std::string compression_classification_basis = "canonical_fsp_flags";
    bool decoder_validated = false;
    std::string compression_rejection_reason;
    uint64_t total_pages_scanned = 0;
    uint64_t skipped_pages = 0;
    /// Diagnostics emitted while converting schema evidence into parser
    /// column definitions (fail-closed type-ID conversion boundary).
    std::vector<std::string> schema_diagnostics;
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
    bool verbose,
    const std::string& schema_file_path = "",
    const std::string& secondary_index_paths = "",
    const std::string& secondary_index_name = "");

bool write_unified_result_json(const std::string& output_dir,
                               const std::string& table_name,
                               const UnifiedRecoveryResult& result);

} // namespace tools
} // namespace dbrecovery
