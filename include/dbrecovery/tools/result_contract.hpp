#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
    uint64_t damaged_pages = 0;
    uint64_t checksum_mismatches = 0;
    uint64_t header_damage_pages = 0;
    bool importable = false;
    std::string reason = "independent validation is required before import";
    std::string source_ibd;
};

ResultContract result_contract_from_unified(const UnifiedRecoveryResult& result,
                                             const std::string& table_name,
                                             const std::string& source_ibd = "");

std::string result_contract_json(const ResultContract& result, int indent = 2);
bool write_result_contract_json(const std::string& path,
                                const ResultContract& result);

} // namespace dbrecovery::tools
