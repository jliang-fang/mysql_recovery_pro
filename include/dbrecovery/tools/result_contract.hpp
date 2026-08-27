#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dbrecovery/common/damage_level.hpp"

namespace dbrecovery::tools {

struct UnifiedRecoveryResult;

struct ResultContract {
    std::string table_name = "recovered_table";
    bool success = false;
    std::string classification = "forensic_evidence";
    uint64_t structured_rows = 0;
    uint64_t evidence_fragments = 0;
    std::string schema_source = "EVIDENCE_ONLY";
    std::string schema_trust_level = "EVIDENCE_ONLY";
    common::DamageLevel damage_level = common::DamageLevel::NONE;
    std::string recovery_method = "legacy";
    bool ddl_variation_detected = false;
    bool export_success = false;
    std::string export_rejection_reason;
    uint64_t total_pages_scanned = 0;
    uint64_t skipped_pages = 0;
    uint64_t damaged_pages = 0;
    uint64_t checksum_mismatches = 0;
    uint64_t header_damage_pages = 0;
    bool importable = false;
    std::string reason = "independent validation is required before import";
    std::string source_ibd;
    std::string compression_status = "NOT_COMPRESSED";
    uint64_t logical_page_size = 0;
    uint64_t physical_page_size = 0;
    std::string compression_classification_basis = "canonical_fsp_flags";
    bool decoder_validated = false;
    std::string compression_rejection_reason;
};

ResultContract result_contract_from_unified(const UnifiedRecoveryResult& result,
                                             const std::string& table_name,
                                             const std::string& source_ibd = "");

ResultContract result_contract_from_legacy(const nlohmann::json& legacy,
                                           const std::string& fallback_table_name,
                                           const std::string& fallback_source_ibd = "");

nlohmann::json result_contract_object(const ResultContract& result);
std::string result_contract_json(const ResultContract& result, int indent = 2);
bool write_result_contract_json(const std::string& path,
                                const ResultContract& result);

} // namespace dbrecovery::tools
