#include "dbrecovery/tools/result_contract.hpp"

#include "dbrecovery/tools/unified_recovery.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <limits>
#include <type_traits>

namespace dbrecovery::tools {

namespace {

template <typename T>
T legacy_value(const nlohmann::json& object, const char* key, T fallback) {
    const auto it = object.find(key);
    if (it == object.end()) return fallback;
    try {
        if constexpr (std::is_same_v<T, std::string>) {
            return it->is_string() ? it->get<std::string>() : fallback;
        } else if constexpr (std::is_same_v<T, bool>) {
            return it->is_boolean() ? it->get<bool>() : fallback;
        } else {
            if constexpr (std::is_unsigned_v<T>) {
                if (it->is_number_integer() && it->get<int64_t>() < 0) {
                    return fallback;
                }
            }
            return it->is_number_unsigned() || it->is_number_integer()
                ? it->get<T>() : fallback;
        }
    } catch (...) {
        return fallback;
    }
}

common::DamageLevel legacy_damage_level(const nlohmann::json& object,
                                        common::DamageLevel fallback) {
    const auto it = object.find("damage_level");
    if (it == object.end()) return fallback;
    if (it->is_string()) {
        const auto value = it->get<std::string>();
        if (value == "NONE") return common::DamageLevel::NONE;
        if (value == "L1_MINOR") return common::DamageLevel::L1_MINOR;
        if (value == "L2_MODERATE") return common::DamageLevel::L2_MODERATE;
        if (value == "L3_SEVERE") return common::DamageLevel::L3_SEVERE;
        if (value == "L4_CRITICAL") return common::DamageLevel::L4_CRITICAL;
        return fallback;
    }
    if (it->is_number_unsigned() || it->is_number_integer()) {
        try {
            const auto value = it->get<int>();
            if (value >= 0 && value <= 4) {
                return static_cast<common::DamageLevel>(value);
            }
        } catch (...) {
        }
    }
    return fallback;
}

} // namespace

ResultContract result_contract_from_legacy(const nlohmann::json& legacy,
                                           const std::string& fallback_table_name,
                                           const std::string& fallback_source_ibd) {
    ResultContract contract;
    contract.table_name = legacy_value(legacy, "table_name", fallback_table_name);
    if (contract.table_name.empty()) contract.table_name = "recovered_table";
    contract.success = legacy_value(legacy, "success", contract.success);
    contract.classification = legacy_value(legacy, "classification", contract.classification);
    contract.structured_rows = legacy_value(legacy, "structured_rows", contract.structured_rows);
    contract.evidence_fragments = legacy_value(legacy, "evidence_fragments", contract.evidence_fragments);
    contract.schema_source = legacy_value(legacy, "schema_source", contract.schema_source);
    contract.schema_trust_level = legacy_value(legacy, "schema_trust_level", contract.schema_trust_level);
    contract.damage_level = legacy_damage_level(legacy, contract.damage_level);
    contract.recovery_method = legacy_value(legacy, "recovery_method", contract.recovery_method);
    contract.ddl_variation_detected = legacy_value(legacy, "ddl_variation_detected", contract.ddl_variation_detected);
    contract.export_success = legacy_value(legacy, "export_success", contract.export_success);
    contract.export_rejection_reason = legacy_value(legacy, "export_rejection_reason", contract.export_rejection_reason);
    contract.total_pages_scanned = legacy_value(legacy, "total_pages_scanned", contract.total_pages_scanned);
    contract.damaged_pages = legacy_value(legacy, "damaged_pages", contract.damaged_pages);
    contract.checksum_mismatches = legacy_value(legacy, "checksum_mismatches", contract.checksum_mismatches);
    contract.header_damage_pages = legacy_value(legacy, "header_damage_pages", contract.header_damage_pages);
    contract.reason = legacy_value(legacy, "reason", contract.reason);
    contract.source_ibd = legacy_value(legacy, "source_ibd", fallback_source_ibd);
    contract.importable = false;
    return contract;
}

ResultContract result_contract_from_unified(const UnifiedRecoveryResult& result,
                                             const std::string& table_name,
                                             const std::string& source_ibd) {
    ResultContract contract;
    contract.table_name = result.table_name.empty()
        ? (table_name.empty() ? "recovered_table" : table_name)
        : result.table_name;
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
    contract.source_ibd = !result.source_ibd.empty() ? result.source_ibd : source_ibd;
    return contract;
}

nlohmann::json result_contract_object(const ResultContract& result) {
    return {
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
}

std::string result_contract_json(const ResultContract& result, int indent) {
    return result_contract_object(result).dump(indent) + "\n";
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
