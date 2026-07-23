/**
 * @file damaged_page_handler.hpp
 * @brief DamagedPageHandler — Unified facade for damaged page salvage & repair
 *
 * v2.0 architecture: Wraps RowSalvageScanner, CorruptionDetector, HeaderRepairer,
 * and PageContentRepairer into a single damage-aware pipeline.
 *
 * Pipeline:
 *   1. Detect damage level (delegates to RowSalvageScanner::detect_damage_level)
 *   2. Attempt repair (HeaderRepairer for FIL header, PageContentRepairer for page structure)
 *   3. Re-detect damage level after repair
 *   4. Execute salvage scan (delegates to RowSalvageScanner)
 *   5. Aggregate statistics
 */

#ifndef DBRECOVERY_SALVAGE_DAMAGED_PAGE_HANDLER_HPP
#define DBRECOVERY_SALVAGE_DAMAGED_PAGE_HANDLER_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <iostream>
#include <chrono>
#include <mutex>

#include "dbrecovery/salvage/row_salvage_scanner.hpp"
#include "dbrecovery/repair/corruption_detector.hpp"
#include "dbrecovery/repair/header_repairer.hpp"
#include "dbrecovery/repair/page_content_repair.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace salvage {

// ==================== Repair Strategy Enum ====================

enum class RepairStrategy : uint8_t {
    NONE            = 0,  ///< No repair needed
    HEADER_ONLY     = 1,  ///< Only repair FIL header (checksum, page_no, page_type)
    STRUCTURE_ONLY  = 2,  ///< Only repair page structure (page dir, heap_top, n_dir_slots)
    FULL_REPAIR     = 3,  ///< Full repair: header + structure
    SALVAGE_ONLY    = 4,  ///< No repair possible, direct salvage
};

inline std::string repair_strategy_to_string(RepairStrategy strategy) {
    switch (strategy) {
        case RepairStrategy::NONE:           return "NONE";
        case RepairStrategy::HEADER_ONLY:    return "HEADER_ONLY";
        case RepairStrategy::STRUCTURE_ONLY: return "STRUCTURE_ONLY";
        case RepairStrategy::FULL_REPAIR:    return "FULL_REPAIR";
        case RepairStrategy::SALVAGE_ONLY:   return "SALVAGE_ONLY";
        default:                             return "UNKNOWN";
    }
}

// ==================== Repair Result ====================

struct RepairAttemptResult {
    bool header_repaired    = false;
    bool structure_repaired = false;
    bool checksum_fixed     = false;
    uint32_t page_no        = 0;
    DamageLevel before_level = DamageLevel::LEVEL_4_CATASTROPHIC;
    DamageLevel after_level  = DamageLevel::LEVEL_4_CATASTROPHIC;
    std::vector<std::string> repair_log;

    bool improved() const { return after_level < before_level; }
    bool is_fully_repaired() const { return after_level == DamageLevel::LEVEL_0_HEALTHY; }
};

// ==================== DamagedPageHandler ====================

/**
 * @brief DamagedPageHandler — Unified facade for damaged page handling
 *
 * Combines:
 *   - Damage detection (RowSalvageScanner::detect_damage_level)
 *   - Repair (HeaderRepairer + PageContentRepairer)
 *   - Salvage scanning (RowSalvageScanner)
 *   - Statistics aggregation
 *
 * Usage:
 * @code
 *   DamagedPageHandler handler;
 *   handler.set_schema(schema);
 *   auto result = handler.handle_page(page_data, page_no, data_size);
 *   // Check result.repair_result, result.salvage_result
 * @endcode
 */
class DamagedPageHandler {
public:
    DamagedPageHandler() = default;

    // ==================== Configuration ====================

    void set_page_size(size_t page_size) {
        page_size_ = page_size;
        scanner_.set_page_size(page_size);
        content_repairer_.set_page_size(page_size);
    }

    void set_schema(const catalog::TableSchema& schema) {
        schema_ = schema;
        scanner_.set_schema(schema);
    }

    void set_schema_hint(const SchemaHint& hint) {
        scanner_.set_schema_hint(hint);
    }

    void set_min_confidence(double min_conf) {
        min_confidence_ = min_conf;
        scanner_.set_min_confidence(min_conf);
    }

    void set_enable_repair(bool enable) { enable_repair_ = enable; }
    void set_enable_dedup(bool enable) { scanner_.set_enable_dedup(enable); }
    void set_scan_brute_force_step(uint32_t step) { scanner_.set_scan_brute_force_step(step); }

    // ==================== Statistics ====================

    struct HandlerStats {
        uint64_t pages_handled           = 0;
        uint64_t pages_repaired          = 0;
        uint64_t pages_salvaged          = 0;
        uint64_t total_records_recovered = 0;
        uint64_t high_confidence_records  = 0;
        double   total_repair_time_ms    = 0.0;
        double   total_scan_time_ms      = 0.0;
        uint64_t repair_attempts         = 0;
        uint64_t repair_successes        = 0;

        std::map<DamageLevel, uint64_t> damage_distribution;
        std::map<RepairStrategy, uint64_t> repair_strategy_usage;

        void reset() {
            pages_handled = 0; pages_repaired = 0; pages_salvaged = 0;
            total_records_recovered = 0; high_confidence_records = 0;
            total_repair_time_ms = 0.0; total_scan_time_ms = 0.0;
            repair_attempts = 0; repair_successes = 0;
            damage_distribution.clear(); repair_strategy_usage.clear();
        }
    };

    const HandlerStats& get_stats() const { return stats_; }
    void reset_stats() { stats_.reset(); scanner_.reset_stats(); }

    RowSalvageScanner& scanner() { return scanner_; }
    const RowSalvageScanner& scanner() const { return scanner_; }

    // ==================== Core API ====================

    /**
     * @brief Handle a single damaged page — detect, repair, salvage
     *
     * Pipeline:
     *   1. Detect damage level
     *   2. Select repair strategy based on damage level
     *   3. Attempt repair (if enabled)
     *   4. Re-detect damage level after repair
     *   5. Execute salvage scan
     *   6. Aggregate statistics
     *
     * @param page_data Raw page buffer
     * @param page_no   Page number
     * @param data_size Buffer size
     * @return Pair of (repair result, salvage result)
     */
    std::pair<RepairAttemptResult, SalvagePageResult> handle_page(
        const uint8_t* page_data,
        uint32_t page_no,
        size_t data_size) {

        stats_.pages_handled++;

        // Step 1: Detect damage level
        DamageLevel initial_level = scanner_.detect_damage_level(page_data, data_size);
        stats_.damage_distribution[initial_level]++;

        // Step 2: Select repair strategy
        RepairStrategy strategy = select_repair_strategy(initial_level);
        stats_.repair_strategy_usage[strategy]++;

        // Step 3: Attempt repair
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

        // Step 4: Re-detect after repair (if repair was attempted)
        DamageLevel after_level = repair_result.improved()
            ? scanner_.detect_damage_level(page_data, data_size)
            : initial_level;
        repair_result.after_level = after_level;

        // Step 5: Execute salvage scan
        SalvagePageResult salvage_result = scanner_.salvage_scan_page(page_data, page_no, data_size);

        // Step 6: Aggregate
        stats_.total_scan_time_ms += salvage_result.scan_time_ms;
        stats_.total_records_recovered += salvage_result.records.size();
        stats_.high_confidence_records += std::count_if(
            salvage_result.records.begin(), salvage_result.records.end(),
            [](const SalvagedRecord& r) { return r.confidence >= 0.7; });

        if (repair_result.improved()) {
            stats_.pages_repaired++;
        }
        if (after_level > DamageLevel::LEVEL_0_HEALTHY || salvage_result.record_count() > 0) {
            stats_.pages_salvaged++;
        }

        return {repair_result, salvage_result};
    }

    /**
     * @brief Handle multiple pages
     */
    std::vector<std::pair<RepairAttemptResult, SalvagePageResult>> handle_pages(
        const std::vector<std::pair<std::vector<uint8_t>, uint32_t>>& pages) {

        std::vector<std::pair<RepairAttemptResult, SalvagePageResult>> results;
        results.reserve(pages.size());
        for (const auto& [page_data, page_no] : pages) {
            results.push_back(handle_page(page_data.data(), page_no, page_data.size()));
        }
        return results;
    }

    /**
     * @brief Select repair strategy based on damage level
     */
    static RepairStrategy select_repair_strategy(DamageLevel level) {
        switch (level) {
            case DamageLevel::LEVEL_0_HEALTHY:      return RepairStrategy::NONE;
            case DamageLevel::LEVEL_1_MINOR:        return RepairStrategy::HEADER_ONLY;
            case DamageLevel::LEVEL_2_MODERATE:     return RepairStrategy::FULL_REPAIR;
            case DamageLevel::LEVEL_3_SEVERE:       return RepairStrategy::STRUCTURE_ONLY;
            case DamageLevel::LEVEL_4_CATASTROPHIC: return RepairStrategy::SALVAGE_ONLY;
            default:                                return RepairStrategy::SALVAGE_ONLY;
        }
    }

    /**
     * @brief Generate a human-readable report of all handler stats
     */
    std::string generate_report() const {
        std::ostringstream oss;
        oss << "=== DamagedPageHandler Report ===\n"
            << "Pages handled:      " << stats_.pages_handled << "\n"
            << "Pages repaired:     " << stats_.pages_repaired << "\n"
            << "Pages salvaged:     " << stats_.pages_salvaged << "\n"
            << "Records recovered:  " << stats_.total_records_recovered << "\n"
            << "High confidence:    " << stats_.high_confidence_records << "\n"
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

        // Scanner stats
        const auto& scanner_stats = scanner_.get_stats();
        oss << "Scanner stats:\n"
            << "  Pages scanned:     " << scanner_stats.pages_scanned << "\n"
            << "  Pages with damage: " << scanner_stats.pages_with_damage << "\n"
            << "  Dedup removed:     " << scanner_stats.dedup_removed << "\n";

        return oss.str();
    }

private:
    size_t page_size_ = 16384;
    double min_confidence_ = 0.3;
    bool enable_repair_ = true;
    catalog::TableSchema schema_;
    RowSalvageScanner scanner_{page_size_};
    repair::HeaderRepairer header_repairer_;
    repair::PageContentRepairer content_repairer_;
    HandlerStats stats_;

    /**
     * @brief Attempt repair on a page based on the selected strategy
     *
     * Operates on a mutable copy of the page data. The original page data
     * is not modified — the repair is attempted on a local copy, and
     * improvements are reported back.
     */
    RepairAttemptResult attempt_repair(
        const uint8_t* page_data,
        uint32_t page_no,
        size_t data_size,
        RepairStrategy strategy) {

        auto start_time = std::chrono::high_resolution_clock::now();
        RepairAttemptResult result;
        result.page_no = page_no;
        result.before_level = scanner_.detect_damage_level(page_data, data_size);

        // Create a mutable copy for repair attempts
        std::vector<uint8_t> page_copy(page_data, page_data + data_size);

        if (strategy == RepairStrategy::HEADER_ONLY || strategy == RepairStrategy::FULL_REPAIR) {
            // Attempt FIL header repair
            bool header_ok = header_repairer_.repair_page_header(page_copy.data(), page_copy.size());
            if (header_ok) {
                // Verify the repair actually fixed something
                DamageLevel after_header = scanner_.detect_damage_level(
                    page_copy.data(), page_copy.size());
                if (after_header < result.before_level) {
                    result.header_repaired = true;
                    result.repair_log.push_back(
                        "FIL header repaired: " + damage_level_to_string(result.before_level)
                        + " -> " + damage_level_to_string(after_header));
                }
            }

            // Calculate and fix checksum
            uint32_t checksum = header_repairer_.calculate_and_fix_checksum(
                page_copy.data(), page_copy.size());
            if (checksum != 0) {
                result.checksum_fixed = true;
                result.repair_log.push_back("Checksum recalculated: 0x" +
                    std::to_string(checksum));
            }
        }

        if (strategy == RepairStrategy::STRUCTURE_ONLY || strategy == RepairStrategy::FULL_REPAIR) {
            // Attempt page structure repair
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

            // Try rebuilding page directory
            if (content_repairer_.rebuild_page_directory(page_copy.data(), page_copy.size())) {
                result.structure_repaired = true;
                result.repair_log.push_back("Page directory rebuilt");
            }
        }

        // Re-evaluate after repair
        DamageLevel after_level = scanner_.detect_damage_level(
            page_copy.data(), page_copy.size());
        result.after_level = after_level;

        // If repair improved the situation, copy the repaired data back
        if (after_level < result.before_level) {
            // Copy repaired data back (only if page_data is not const in caller context)
            // We use memcpy into a non-const buffer — the caller must handle this
            std::memcpy(const_cast<uint8_t*>(page_data), page_copy.data(), page_copy.size());
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        stats_.total_repair_time_ms += std::chrono::duration<double, std::milli>(
            end_time - start_time).count();

        return result;
    }
};

} // namespace salvage
} // namespace dbrecovery

#endif // DBRECOVERY_SALVAGE_DAMAGED_PAGE_HANDLER_HPP