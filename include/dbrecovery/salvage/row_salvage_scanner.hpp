/**
 * @file row_salvage_scanner.hpp
 * @brief RowSalvageScanner — Row-by-row salvage scanning for damaged pages
 *
 * v2.0 architecture: When page headers are too damaged for normal B+Tree parsing,
 * this engine performs heuristic record-level scanning to recover data.
 *
 * Algorithm (salvage_scan_page):
 *   Level 0 — Healthy page, use normal B+Tree parsing
 *   Level 1 — Minor damage, skip page header stats, use page directory
 *   Level 2 — Moderate damage, linear scan from INFIMUM to SUPREMUM
 *   Level 3 — Severe damage, brute-force scan for record patterns
 *   Level 4 — Catastrophic damage, extract data fragments only
 */

#ifndef DBRECOVERY_INNODB_ROW_SALVAGE_SCANNER_HPP
#define DBRECOVERY_INNODB_ROW_SALVAGE_SCANNER_HPP

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
#include <functional>
#include <iostream>
#include <chrono>
#include <mutex>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/common/damage_level.hpp"

namespace dbrecovery {
namespace salvage {

using innodb::FIL_HEADER_SIZE;
using innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
using innodb::REC_STATUS_SUPREMUM;

// ==================== Damage Level Enum ====================

// Canonical damage classification (shared with the recovery engine).
using DamageLevel = common::DamageLevel;

inline constexpr DamageLevel LEVEL_0_HEALTHY      = DamageLevel::NONE;
inline constexpr DamageLevel LEVEL_1_MINOR        = DamageLevel::L1_MINOR;
inline constexpr DamageLevel LEVEL_2_MODERATE     = DamageLevel::L2_MODERATE;
inline constexpr DamageLevel LEVEL_3_SEVERE       = DamageLevel::L3_SEVERE;
inline constexpr DamageLevel LEVEL_4_CATASTROPHIC = DamageLevel::L4_CRITICAL;

// damage_level_to_string is inherited from common::damage_level_to_string
using common::damage_level_to_string;

// ==================== Scan Strategy Enum ====================

enum class ScanStrategy : uint8_t {
    BPLUS_TREE   = 0,
    PAGE_DIR     = 1,
    LINEAR_SCAN  = 2,
    BRUTE_FORCE  = 3,
    FRAGMENT     = 4
};

// ==================== Data Structures ====================

struct SalvagedRecord {
    uint32_t page_no       = 0;
    uint16_t offset        = 0;
    uint16_t estimated_size = 0;

    std::vector<uint8_t> raw_data;
    std::map<std::string, std::string> parsed_fields;
    std::set<std::string> missing_fields;

    double confidence = 0.0;
    bool has_deleted_flag = false;
    bool is_system_record = false;

    ScanStrategy source = ScanStrategy::BRUTE_FORCE;
    std::vector<uint8_t> primary_key;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "Record[off=" << offset << ", size=" << estimated_size
            << ", conf=" << std::fixed << std::setprecision(2) << confidence
            << ", deleted=" << has_deleted_flag
            << ", source=" << static_cast<int>(source) << "]";
        return oss.str();
    }
};

struct DamageLogEntry {
    uint32_t position = 0;
    std::string description;
    uint32_t bytes_skipped = 0;
    uint8_t severity = 0;

    DamageLogEntry(uint32_t pos, std::string desc, uint32_t skipped = 0, uint8_t sev = 1)
        : position(pos), description(std::move(desc)), bytes_skipped(skipped), severity(sev) {}
};

struct SalvagePageResult {
    uint32_t page_no = 0;
    DamageLevel damage_level = LEVEL_0_HEALTHY;
    ScanStrategy strategy_used = ScanStrategy::BPLUS_TREE;

    std::vector<SalvagedRecord> records;
    std::vector<DamageLogEntry> damage_log;

    uint32_t total_bytes_scanned = 0;
    uint32_t bytes_salvaged = 0;
    double recovery_ratio = 0.0;
    double scan_time_ms = 0.0;

    uint32_t record_count() const { return static_cast<uint32_t>(records.size()); }
    uint32_t damage_count() const { return static_cast<uint32_t>(damage_log.size()); }

    bool has_healthy_records() const {
        return std::any_of(records.begin(), records.end(),
            [](const SalvagedRecord& r) { return r.confidence >= 0.7; });
    }
};

struct SalvageScannerStats {
    uint64_t pages_scanned = 0;
    uint64_t pages_with_damage = 0;
    uint64_t total_records_salvaged = 0;
    uint64_t high_confidence_records = 0;
    uint64_t total_damage_events = 0;
    double total_scan_time_ms = 0.0;
    uint64_t dedup_removed = 0;

    std::map<DamageLevel, uint64_t> damage_level_distribution;
    std::map<ScanStrategy, uint64_t> strategy_usage;

    void reset();
};

// ==================== Schema Hint ====================

struct SchemaHint {
    bool has_pk_info = false;
    uint32_t pk_column_type = 0;
    uint32_t pk_column_length = 0;
    bool pk_is_integer = false;
    bool pk_is_string = false;

    uint32_t expected_columns = 0;
    uint32_t nullable_count = 0;
    std::vector<uint32_t> column_types;
    std::set<uint32_t> timestamp_column_positions;
    std::optional<uint32_t> auto_increment_position;

    static SchemaHint from_table_schema(const catalog::TableSchema& schema);
};

// ==================== RowSalvageScanner ====================

class RowSalvageScanner {
public:
    explicit RowSalvageScanner(size_t page_size = 16384)
        : page_size_(page_size) {}

    void set_page_size(size_t page_size) { page_size_ = page_size; }
    void set_schema_hint(const SchemaHint& hint) { schema_hint_ = hint; }
    void set_schema(const catalog::TableSchema& schema) {
        schema_hint_ = SchemaHint::from_table_schema(schema);
    }
    void set_min_confidence(double min_conf) { min_confidence_ = min_conf; }
    void set_enable_dedup(bool enable) { enable_dedup_ = enable; }
    void set_scan_brute_force_step(uint32_t step) { brute_force_step_ = step; }

    const SalvageScannerStats& get_stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

    DamageLevel detect_damage_level(const uint8_t* page_data, size_t data_size) const;
    static ScanStrategy select_strategy(DamageLevel level);
    SalvagePageResult salvage_scan_page(const uint8_t* page_data, uint32_t page_no, size_t data_size);
    std::vector<SalvagePageResult> salvage_scan_pages(
        const std::vector<std::pair<std::vector<uint8_t>, uint32_t>>& pages);
    void deduplicate_records(std::vector<SalvagedRecord>& records) const;

private:
    size_t page_size_ = 16384;
    SchemaHint schema_hint_;
    double min_confidence_ = 0.3;
    bool enable_dedup_ = true;
    uint32_t brute_force_step_ = 1;
    SalvageScannerStats stats_;

    bool check_checksum(const uint8_t* page_data, size_t data_size) const;
    void scan_level1_minor(const uint8_t* page_data, size_t data_size, SalvagePageResult& result);
    void scan_level2_linear(const uint8_t* page_data, size_t data_size, SalvagePageResult& result);
    void scan_level3_brute_force(const uint8_t* page_data, size_t data_size, SalvagePageResult& result);
    void scan_level4_fragment(const uint8_t* page_data, size_t data_size, SalvagePageResult& result);
    uint16_t find_supremum(const uint8_t* page_data, size_t data_size) const;
    SalvagedRecord try_parse_compact_record(const uint8_t* page_data, size_t data_size, uint16_t offset) const;
    SalvagedRecord try_parse_record_heuristic(const uint8_t* page_data, size_t data_size, uint32_t pos) const;
    uint16_t find_next_likely_record(const uint8_t* page_data, size_t data_size,
                                     uint16_t current_pos, uint16_t supremum_pos) const;
    uint16_t estimate_record_length(const uint8_t* page_data, size_t data_size, uint16_t offset) const;
    std::vector<uint8_t> extract_likely_pk(const uint8_t* page_data, size_t data_size, uint32_t offset) const;
};

} // namespace salvage
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_ROW_SALVAGE_SCANNER_HPP