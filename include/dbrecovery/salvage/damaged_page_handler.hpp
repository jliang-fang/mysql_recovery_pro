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
    NONE            = 0,
    HEADER_ONLY     = 1,
    STRUCTURE_ONLY  = 2,
    FULL_REPAIR     = 3,
    SALVAGE_ONLY    = 4,
};

std::string repair_strategy_to_string(RepairStrategy strategy);

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

class DamagedPageHandler {
public:
    DamagedPageHandler() = default;

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

        void reset();
    };

    const HandlerStats& get_stats() const { return stats_; }
    void reset_stats() { stats_.reset(); scanner_.reset_stats(); }

    RowSalvageScanner& scanner() { return scanner_; }
    const RowSalvageScanner& scanner() const { return scanner_; }

    std::pair<RepairAttemptResult, SalvagePageResult> handle_page(
        const uint8_t* page_data, uint32_t page_no, size_t data_size);

    std::vector<std::pair<RepairAttemptResult, SalvagePageResult>> handle_pages(
        const std::vector<std::pair<std::vector<uint8_t>, uint32_t>>& pages);

    static RepairStrategy select_repair_strategy(DamageLevel level);

    std::string generate_report() const;

private:
    size_t page_size_ = 16384;
    double min_confidence_ = 0.3;
    bool enable_repair_ = true;
    catalog::TableSchema schema_;
    RowSalvageScanner scanner_{page_size_};
    repair::HeaderRepairer header_repairer_;
    repair::PageContentRepairer content_repairer_;
    HandlerStats stats_;

    RepairAttemptResult attempt_repair(
        const uint8_t* page_data, uint32_t page_no, size_t data_size, RepairStrategy strategy);
};

} // namespace salvage
} // namespace dbrecovery

#endif // DBRECOVERY_SALVAGE_DAMAGED_PAGE_HANDLER_HPP