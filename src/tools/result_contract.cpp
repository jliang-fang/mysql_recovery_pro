#include "dbrecovery/tools/result_contract.hpp"

#include "dbrecovery/tools/unified_recovery.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace dbrecovery::tools {

ResultContract result_contract_from_unified(const UnifiedRecoveryResult& result,
                                             const std::string& table_name,
                                             const std::string& source_ibd) {
    ResultContract contract;
    contract.table_name = table_name.empty() ? "recovered_table" : table_name;
    contract.success = result.success;
    contract.classification = result.result_classification;
    contract.structured_rows = result.total_recovered;
    contract.evidence_fragments = result.evidence_fragments.size();
    contract.schema_source = result.schema_source;
    contract.schema_trust_level = result.trust_level;
    contract.damage_level = result.damage_level;
    contract.recovery_method = result.recovery_method;
    contract.ddl_variation_detected = result.ddl_variation_detected;
    contract.export_success = result.export_success;
    contract.export_rejection_reason = result.export_rejection_reason;
    contract.total_pages_scanned = result.page_damage.size();
    for (const auto& page : result.page_damage) {
        if (page.damage_level != common::DamageLevel::NONE) ++contract.damaged_pages;
        if (!page.checksum_valid) ++contract.checksum_mismatches;
        if (!page.fil_header_valid || !page.page_header_valid) ++contract.header_damage_pages;
    }
    contract.importable = false;
    contract.reason = contract.export_rejection_reason.empty()
        ? "independent validation is required before import"
        : contract.export_rejection_reason;
    contract.source_ibd = source_ibd;
    return contract;
}

std::string result_contract_json(const ResultContract& result, int indent) {
    nlohmann::json output = {
        {"format_version", 2},
        {"table_name", result.table_name},
        {"success", result.success},
        {"classification", result.classification},
        {"structured_rows", result.structured_rows},
        {"evidence_fragments", result.evidence_fragments},
        {"schema_source", result.schema_source},
        {"schema_trust_level", result.schema_trust_level},
        {"damage_level", common::damage_level_to_string(result.damage_level)},
        {"recovery_method", result.recovery_method},
        {"ddl_variation_detected", result.ddl_variation_detected},
        {"export_success", result.export_success},
        {"export_rejection_reason", result.export_rejection_reason},
        {"total_pages_scanned", result.total_pages_scanned},
        {"damaged_pages", result.damaged_pages},
        {"checksum_mismatches", result.checksum_mismatches},
        {"header_damage_pages", result.header_damage_pages},
        {"importable", false},
        {"reason", result.reason},
        {"source_ibd", result.source_ibd}
    };
    return output.dump(indent) + "\n";
}

bool write_result_contract_json(const std::string& path,
                                const ResultContract& result) {
    try {
        const std::filesystem::path output_path(path);
        if (!output_path.parent_path().empty()) {
            std::filesystem::create_directories(output_path.parent_path());
        }
        std::ofstream output(path, std::ios::trunc);
        if (!output) return false;
        output << result_contract_json(result);
        return static_cast<bool>(output);
    } catch (...) {
        return false;
    }
}

} // namespace dbrecovery::tools
