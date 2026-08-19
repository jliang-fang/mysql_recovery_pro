/**
 * @file damaged_page_handler.cpp
 * @brief DamagedPageHandler implementation
 *
 * Extracted from damaged_page_handler.hpp for better compilation boundaries.
 */

#include "dbrecovery/salvage/damaged_page_handler.hpp"
#include "dbrecovery/salvage/row_salvage_scanner.hpp"
#include "dbrecovery/repair/corruption_detector.hpp"
#include "dbrecovery/repair/header_repairer.hpp"
#include "dbrecovery/repair/page_content_repair.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <chrono>

namespace dbrecovery {
namespace salvage {

std::string repair_strategy_to_string(RepairStrategy strategy) {
    switch (strategy) {
        case RepairStrategy::NONE:           return "NONE";
        case RepairStrategy::HEADER_ONLY:    return "HEADER_ONLY";
        case RepairStrategy::STRUCTURE_ONLY: return "STRUCTURE_ONLY";
        case RepairStrategy::FULL_REPAIR:    return "FULL_REPAIR";
        case RepairStrategy::SALVAGE_ONLY:   return "SALVAGE_ONLY";
        default:                             return "UNKNOWN";
    }
}

// ==================== DamagedPageHandler ====================

std::pair<RepairAttemptResult, SalvagePageResult> DamagedPageHandler::handle_page(
    const uint8_t* page_data, uint32_t page_no, size_t data_size) {

    stats_.pages_handled++;

    DamageLevel initial_level = scanner_.detect_damage_level(page_data, data_size);
    stats_.damage_distribution[initial_level]++;

    RepairStrategy strategy = select_repair_strategy(initial_level);
    stats_.repair_strategy_usage[strategy]++;

    RepairAttemptResult repair_result;
    repair_result.page_no = page_no;
    repair_result.before_level = initial_level;

    if (enable_repair_ && strategy != RepairStrategy::NONE && strategy != RepairStrategy::SALVAGE_ONLY) {
        repair_result = attempt_repair(page_data, page_no, data_size, strategy);
        stats_.repair_attempts++;
        if (repair_result.improved()) {
            stats_.repair_successes++;
        }
    }

    DamageLevel after_level = repair_result.improved()
        ? scanner_.detect_damage_level(page_data, data_size)
        : initial_level;
    repair_result.after_level = after_level;

    SalvagePageResult salvage_result = scanner_.salvage_scan_page(page_data, page_no, data_size);

    stats_.total_scan_time_ms += salvage_result.scan_time_ms;

    // Provenance-gated counting: only records from structured-exportable
    // strategies count toward recovery. BRUTE_FORCE and FRAGMENT are
    // forensic evidence and never counted as recovered or high-confidence.
    for (const auto& rec : salvage_result.records) {
        if (is_structured_exportable(rec.source)) {
            stats_.structured_exportable_records++;
            stats_.total_records_recovered++;
            if (rec.confidence >= 0.7) {
                stats_.high_confidence_records++;
            }
        } else {
            stats_.evidence_only_records++;
            if (rec.confidence < 0.7) {
                stats_.low_confidence_records++;
            }
        }
    }

    if (repair_result.improved()) {
        stats_.pages_repaired++;
    }
    if (after_level > LEVEL_0_HEALTHY || salvage_result.record_count() > 0) {
        stats_.pages_salvaged++;
    }

    return {repair_result, salvage_result};
}

std::vector<std::pair<RepairAttemptResult, SalvagePageResult>> DamagedPageHandler::handle_pages(
    const std::vector<std::pair<std::vector<uint8_t>, uint32_t>>& pages) {

    std::vector<std::pair<RepairAttemptResult, SalvagePageResult>> results;
    results.reserve(pages.size());
    for (const auto& [page_data, page_no] : pages) {
        results.push_back(handle_page(page_data.data(), page_no, page_data.size()));
    }
    return results;
}

RepairStrategy DamagedPageHandler::select_repair_strategy(DamageLevel level) {
    switch (level) {
        case LEVEL_0_HEALTHY:      return RepairStrategy::NONE;
        case LEVEL_1_MINOR:        return RepairStrategy::HEADER_ONLY;
        case LEVEL_2_MODERATE:     return RepairStrategy::FULL_REPAIR;
        case LEVEL_3_SEVERE:       return RepairStrategy::STRUCTURE_ONLY;
        case LEVEL_4_CATASTROPHIC: return RepairStrategy::SALVAGE_ONLY;
        default:                                return RepairStrategy::SALVAGE_ONLY;
    }
}

std::string DamagedPageHandler::generate_report() const {
    std::ostringstream oss;
    oss << "=== DamagedPageHandler Report ===\n"
        << "Pages handled:      " << stats_.pages_handled << "\n"
        << "Pages repaired:     " << stats_.pages_repaired << "\n"
        << "Pages salvaged:     " << stats_.pages_salvaged << "\n"
        << "Records recovered:  " << stats_.total_records_recovered << "\n"
        << "  Structured:      " << stats_.structured_exportable_records << "\n"
        << "  Evidence-only:   " << stats_.evidence_only_records << "\n"
        << "High confidence:    " << stats_.high_confidence_records << "\n"
        << "Low confidence:     " << stats_.low_confidence_records << "\n"
        << "Repair attempts:    " << stats_.repair_attempts << "\n"
        << "Repair successes:   " << stats_.repair_successes << "\n"
        << "Repair time:        " << std::fixed << std::setprecision(1)
        << stats_.total_repair_time_ms << " ms\n"
        << "Scan time:          " << stats_.total_scan_time_ms << " ms\n";

    oss << "Damage distribution:\n";
    for (const auto& [level, count] : stats_.damage_distribution) {
        oss << "  " << damage_level_to_string(level) << ": " << count << "\n";
    }

    oss << "Repair strategy usage:\n";
    for (const auto& [strategy, count] : stats_.repair_strategy_usage) {
        oss << "  " << repair_strategy_to_string(strategy) << ": " << count << "\n";
    }

    const auto& scanner_stats = scanner_.get_stats();
    oss << "Scanner stats:\n"
        << "  Pages scanned:     " << scanner_stats.pages_scanned << "\n"
        << "  Pages with damage: " << scanner_stats.pages_with_damage << "\n"
        << "  Dedup removed:     " << scanner_stats.dedup_removed << "\n";

    return oss.str();
}

RepairAttemptResult DamagedPageHandler::attempt_repair(
    const uint8_t* page_data, uint32_t page_no, size_t data_size, RepairStrategy strategy) {

    auto start_time = std::chrono::high_resolution_clock::now();
    RepairAttemptResult result;
    result.page_no = page_no;
    result.before_level = scanner_.detect_damage_level(page_data, data_size);

    std::vector<uint8_t> page_copy(page_data, page_data + data_size);

    if (strategy == RepairStrategy::HEADER_ONLY || strategy == RepairStrategy::FULL_REPAIR) {
        bool header_ok = header_repairer_.repair_page_header(page_copy.data(), page_copy.size());
        if (header_ok) {
            DamageLevel after_header = scanner_.detect_damage_level(
                page_copy.data(), page_copy.size());
            if (after_header < result.before_level) {
                result.header_repaired = true;
                result.repair_log.push_back(
                    "FIL header repaired: " + damage_level_to_string(result.before_level)
                    + " -> " + damage_level_to_string(after_header));
            }
        }

        uint32_t checksum = header_repairer_.calculate_and_fix_checksum(
            page_copy.data(), page_copy.size());
        if (checksum != 0) {
            result.checksum_fixed = true;
            result.repair_log.push_back("Checksum recalculated: 0x" +
                std::to_string(checksum));
        }
    }

    if (strategy == RepairStrategy::STRUCTURE_ONLY || strategy == RepairStrategy::FULL_REPAIR) {
        content_repairer_.set_page_size(page_size_);
        auto corruptions = content_repairer_.detect_corruptions(
            page_copy.data(), page_copy.size());

        if (!corruptions.empty()) {
            size_t repaired = content_repairer_.repair(
                page_copy.data(), page_copy.size(), corruptions);
            if (repaired > 0) {
                result.structure_repaired = true;
                result.repair_log.push_back(
                    "Page structure repaired: " + std::to_string(repaired) + " corruptions fixed");
            }
        }

        if (content_repairer_.rebuild_page_directory(page_copy.data(), page_copy.size())) {
            result.structure_repaired = true;
            result.repair_log.push_back("Page directory rebuilt");
        }
    }

    DamageLevel after_level = scanner_.detect_damage_level(
        page_copy.data(), page_copy.size());
    result.after_level = after_level;

    if (after_level < result.before_level) {
        std::memcpy(const_cast<uint8_t*>(page_data), page_copy.data(), page_copy.size());
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.total_repair_time_ms += std::chrono::duration<double, std::milli>(
        end_time - start_time).count();

    return result;
}

void DamagedPageHandler::HandlerStats::reset() {
    pages_handled = 0; pages_repaired = 0; pages_salvaged = 0;
    total_records_recovered = 0; high_confidence_records = 0;
    structured_exportable_records = 0; evidence_only_records = 0;
    low_confidence_records = 0;
    total_repair_time_ms = 0.0; total_scan_time_ms = 0.0;
    repair_attempts = 0; repair_successes = 0;
    damage_distribution.clear(); repair_strategy_usage.clear();
}

} // namespace salvage
} // namespace dbrecovery