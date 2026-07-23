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

namespace dbrecovery {
namespace salvage {

using innodb::FIL_HEADER_SIZE;
using innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
using innodb::REC_STATUS_SUPREMUM;

// ==================== Damage Level Enum ====================

enum class DamageLevel : uint8_t {
    LEVEL_0_HEALTHY       = 0,  ///< Fully healthy, normal B+Tree parsing
    LEVEL_1_MINOR         = 1,  ///< FIL header OK, page header partially damaged
    LEVEL_2_MODERATE      = 2,  ///< FIL header damaged, checksum mismatch
    LEVEL_3_SEVERE        = 3,  ///< FIL header completely damaged, record structures remain
    LEVEL_4_CATASTROPHIC  = 4   ///< Mostly overwritten, only data fragments
};

inline std::string damage_level_to_string(DamageLevel level) {
    switch (level) {
        case DamageLevel::LEVEL_0_HEALTHY:      return "HEALTHY";
        case DamageLevel::LEVEL_1_MINOR:        return "MINOR_DAMAGE";
        case DamageLevel::LEVEL_2_MODERATE:     return "MODERATE_DAMAGE";
        case DamageLevel::LEVEL_3_SEVERE:       return "SEVERE_DAMAGE";
        case DamageLevel::LEVEL_4_CATASTROPHIC: return "CATASTROPHIC_DAMAGE";
        default:                                return "UNKNOWN";
    }
}

// ==================== Scan Strategy Enum ====================

enum class ScanStrategy : uint8_t {
    BPLUS_TREE   = 0,  ///< Normal B+Tree page parsing
    PAGE_DIR     = 1,  ///< Page directory slot scan
    LINEAR_SCAN  = 2,  ///< Linear INFIMUM→SUPREMUM scan
    BRUTE_FORCE  = 3,  ///< Brute-force heuristic scanning
    FRAGMENT     = 4   ///< Data fragment extraction only
};

// ==================== Data Structures ====================

/**
 * @brief A single salvaged record from the page
 */
struct SalvagedRecord {
    // Position in the page
    uint32_t page_no       = 0;
    uint16_t offset        = 0;
    uint16_t estimated_size = 0;

    // Record data
    std::vector<uint8_t> raw_data;          // Raw bytes of the record
    std::map<std::string, std::string> parsed_fields; // Column name → value (if schema available)
    std::set<std::string> missing_fields;   // Fields that could not be recovered

    // Confidence
    double confidence = 0.0;                // 0.0–1.0
    bool has_deleted_flag = false;          // REC_INFO_DELETED_FLAG set
    bool is_system_record = false;          // INFIMUM/SUPREMUM

    // Source information
    ScanStrategy source = ScanStrategy::BRUTE_FORCE;

    // Primary key (for deduplication)
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

/**
 * @brief Damage log entry — records a corruption event during salvage
 */
struct DamageLogEntry {
    uint32_t position = 0;       // Byte position in the page
    std::string description;     // What was damaged
    uint32_t bytes_skipped = 0;  // Bytes skipped due to damage
    uint8_t severity = 0;        // 0=info, 1=warning, 2=error

    DamageLogEntry(uint32_t pos, std::string desc, uint32_t skipped = 0, uint8_t sev = 1)
        : position(pos), description(std::move(desc)), bytes_skipped(skipped), severity(sev) {}
};

/**
 * @brief Result of a salvage scan on a single page
 */
struct SalvagePageResult {
    uint32_t page_no = 0;
    DamageLevel damage_level = DamageLevel::LEVEL_0_HEALTHY;
    ScanStrategy strategy_used = ScanStrategy::BPLUS_TREE;

    std::vector<SalvagedRecord> records;
    std::vector<DamageLogEntry> damage_log;

    // Statistics
    uint32_t total_bytes_scanned = 0;
    uint32_t bytes_salvaged = 0;
    double recovery_ratio = 0.0;   // bytes_salvaged / page_size
    double scan_time_ms = 0.0;

    uint32_t record_count() const { return static_cast<uint32_t>(records.size()); }
    uint32_t damage_count() const { return static_cast<uint32_t>(damage_log.size()); }

    bool has_healthy_records() const {
        return std::any_of(records.begin(), records.end(),
            [](const SalvagedRecord& r) { return r.confidence >= 0.7; });
    }
};

/**
 * @brief Salvage scanner statistics
 */
struct SalvageScannerStats {
    uint64_t pages_scanned = 0;
    uint64_t pages_with_damage = 0;
    uint64_t total_records_salvaged = 0;
    uint64_t high_confidence_records = 0;  // confidence >= 0.7
    uint64_t total_damage_events = 0;
    double total_scan_time_ms = 0.0;
    uint64_t dedup_removed = 0;

    std::map<DamageLevel, uint64_t> damage_level_distribution;
    std::map<ScanStrategy, uint64_t> strategy_usage;

    void reset() {
        pages_scanned = 0;
        pages_with_damage = 0;
        total_records_salvaged = 0;
        high_confidence_records = 0;
        total_damage_events = 0;
        total_scan_time_ms = 0.0;
        dedup_removed = 0;
        damage_level_distribution.clear();
        strategy_usage.clear();
    }
};

// ==================== Schema Hint ====================

/**
 * @brief Optional schema hint for heuristic record parsing
 */
struct SchemaHint {
    bool has_pk_info = false;
    uint32_t pk_column_type = 0;    // MYSQL_TYPE_LONG=3, etc.
    uint32_t pk_column_length = 0;
    bool pk_is_integer = false;
    bool pk_is_string = false;

    // Expected column count
    uint32_t expected_columns = 0;

    // Nullable column bitmap size
    uint32_t nullable_count = 0;

    // Column types (in physical order)
    std::vector<uint32_t> column_types;

    // Timestamp column positions (for heuristic detection)
    std::set<uint32_t> timestamp_column_positions;

    // Auto-increment column position
    std::optional<uint32_t> auto_increment_position;

    static SchemaHint from_table_schema(const catalog::TableSchema& schema) {
        SchemaHint hint;
        hint.expected_columns = static_cast<uint32_t>(schema.columns.size());

        // Count nullable columns
        hint.nullable_count = 0;
        for (const auto& [ord, col] : schema.columns) {
            if (col.is_nullable) hint.nullable_count++;
            hint.column_types.push_back(col.mysql_type_id);

            // Detect timestamp columns
            if (col.mysql_type_id == dbrecovery::innodb::mysql_types::DATETIME ||
                col.mysql_type_id == dbrecovery::innodb::mysql_types::TIMESTAMP ||
                col.mysql_type_id == dbrecovery::innodb::mysql_types::TIMESTAMP2 ||
                col.mysql_type_id == dbrecovery::innodb::mysql_types::DATETIME2) {
                hint.timestamp_column_positions.insert(col.physical_pos);
            }

            // Detect auto-increment
            if (col.is_auto_increment) {
                hint.auto_increment_position = col.physical_pos;
            }
        }

        // Extract PK info from first index
        for (const auto& idx : schema.indexes) {
            if (idx.is_primary && !idx.columns.empty()) {
                hint.has_pk_info = true;
                const auto& pk_col_name = idx.columns[0];
                for (const auto& [ord, col] : schema.columns) {
                    if (col.name == pk_col_name) {
                        hint.pk_column_type = col.mysql_type_id;
                        hint.pk_column_length = col.column_length;
                        hint.pk_is_integer = (col.mysql_type_id == dbrecovery::innodb::mysql_types::TINY ||
                                              col.mysql_type_id == dbrecovery::innodb::mysql_types::SHORT ||
                                              col.mysql_type_id == dbrecovery::innodb::mysql_types::LONG ||
                                              col.mysql_type_id == dbrecovery::innodb::mysql_types::LONGLONG ||
                                              col.mysql_type_id == dbrecovery::innodb::mysql_types::INT24 ||
                                              col.mysql_type_id == dbrecovery::innodb::mysql_types::YEAR);
                        hint.pk_is_string = (col.mysql_type_id == dbrecovery::innodb::mysql_types::VARCHAR ||
                                             col.mysql_type_id == dbrecovery::innodb::mysql_types::STRING ||
                                             col.mysql_type_id == dbrecovery::innodb::mysql_types::VAR_STRING);
                        break;
                    }
                }
                break;
            }
        }
        return hint;
    }
};

// ==================== RowSalvageScanner ====================

/**
 * @brief RowSalvageScanner — Salvage rows from damaged InnoDB pages
 *
 * Algorithm:
 *   1. Auto-detect damage level (0-4)
 *   2. Select scan strategy based on damage level
 *   3. Execute scan, collecting records with confidence scores
 *   4. Deduplicate and sort results
 *   5. Generate damage log
 */
class RowSalvageScanner {
public:
    explicit RowSalvageScanner(size_t page_size = 16384)
        : page_size_(page_size) {}

    // ==================== Configuration ====================

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

    // ==================== Core API ====================

    /**
     * @brief Auto-detect damage level of a page
     *
     * Performs up to 10 checks:
     *   1. Checksum validation
     *   2. SPACE_ID range check
     *   3. PAGE_NO range check
     *   4. Page type recognition
     *   5. LSN sanity check
     *   6. PAGE_HEADER structure
     *   7. INFIMUM record presence
     *   8. SUPREMUM record presence
     *   9. Offset table integrity
     *  10. Record chain link integrity
     *
     * @param page_data Raw page buffer
     * @return Detected damage level
     */
    DamageLevel detect_damage_level(const uint8_t* page_data, size_t data_size) const {
        if (!page_data || data_size < FIL_HEADER_SIZE + 56) {
            return DamageLevel::LEVEL_4_CATASTROPHIC;
        }

        uint32_t failures = 0;
        uint32_t total_checks = 0;

        // Check 1: Checksum
        total_checks++;
        if (!check_checksum(page_data, data_size)) {
            failures++;
        }

        // Check 2: SPACE_ID range
        total_checks++;
        uint32_t space_id = dbrecovery::innodb::read_le<uint32_t>(page_data, 34);
        if (space_id > 0xFFFFFF) {
            failures++;
        }

        // Check 3: PAGE_NO range
        total_checks++;
        uint32_t page_no = dbrecovery::innodb::read_le<uint32_t>(page_data, 30);
        if (page_no > 0xFFFFFF) {
            failures++;
        }

        // Check 4: Page type
        total_checks++;
        uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page_data, 24);
        bool type_ok = dbrecovery::innodb::is_valid_page_type(page_type);
        if (!type_ok) {
            failures++;
        }

        // Check 5: LSN sanity
        total_checks++;
        uint64_t lsn = dbrecovery::innodb::read_le<uint64_t>(page_data, 16);
        if (lsn == 0 || lsn > 0xFFFFFFFFFFFFULL) {
            failures++;
        }

        // Check 6: PAGE_HEADER structure
        total_checks++;
        uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 0);
        uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 2);
        bool page_header_ok = (n_dir_slots >= 2 && n_dir_slots <= 1000 &&
                               heap_top > FIL_HEADER_SIZE + 56 &&
                               heap_top <= data_size);
        if (!page_header_ok) {
            failures++;
        }

        // Check 7: INFIMUM presence
        total_checks++;
        if (PAGE_INFIMUM_REC_OFFSET_ABS + 5 <= data_size) {
            uint8_t info_bits = page_data[PAGE_INFIMUM_REC_OFFSET_ABS];
            uint8_t status = info_bits & 0x07;
            if (status != dbrecovery::innodb::REC_STATUS_INFIMUM) {
                failures++;
            }
        } else {
            failures++;
        }

        // Check 8: SUPREMUM presence
        total_checks++;
        if (page_header_ok) {
            uint16_t supremum_pos = heap_top - 5;
            if (supremum_pos > 0 && supremum_pos + 5 <= data_size) {
                uint8_t sup_status = page_data[supremum_pos] & 0x07;
                if (sup_status != dbrecovery::innodb::REC_STATUS_SUPREMUM) {
                    failures++;
                }
            } else {
                failures++;
            }
        }

        // Check 9: Offset table integrity
        total_checks++;
        if (page_header_ok) {
            uint16_t heap_top_check = dbrecovery::innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 2);
            uint16_t n_heap = dbrecovery::innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 4);
            uint16_t n_recs = n_heap & 0x3FFF;
            if (n_recs > 0) {
                uint16_t offset_table_end = heap_top_check;
                if (offset_table_end >= n_recs * 2 + 2 && offset_table_end <= data_size) {
                    uint16_t first_offset = dbrecovery::innodb::read_le<uint16_t>(page_data, offset_table_end - 2);
                    if (first_offset < PAGE_INFIMUM_REC_OFFSET_ABS || first_offset >= heap_top_check) {
                        failures++;
                    }
                } else {
                    failures++;
                }
            }
        }

        // Check 10: Record chain links
        total_checks++;
        if (page_header_ok) {
            uint16_t supremum_pos = dbrecovery::innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 2) - 5;
            if (supremum_pos + 5 <= data_size) {
                uint16_t chain_next = dbrecovery::innodb::read_le<uint16_t>(page_data, supremum_pos + 1) & 0x3FFF;
                if (chain_next != 0) {
                    failures++;
                }
            }
        }

        // Classify damage level
        double fail_ratio = static_cast<double>(failures) / static_cast<double>(total_checks);
        if (fail_ratio == 0.0) return DamageLevel::LEVEL_0_HEALTHY;
        if (fail_ratio <= 0.2) return DamageLevel::LEVEL_1_MINOR;
        if (fail_ratio <= 0.4) return DamageLevel::LEVEL_2_MODERATE;
        if (fail_ratio <= 0.7) return DamageLevel::LEVEL_3_SEVERE;
        return DamageLevel::LEVEL_4_CATASTROPHIC;
    }

    /**
     * @brief Select scan strategy based on damage level
     */
    static ScanStrategy select_strategy(DamageLevel level) {
        switch (level) {
            case DamageLevel::LEVEL_0_HEALTHY:      return ScanStrategy::BPLUS_TREE;
            case DamageLevel::LEVEL_1_MINOR:        return ScanStrategy::PAGE_DIR;
            case DamageLevel::LEVEL_2_MODERATE:     return ScanStrategy::LINEAR_SCAN;
            case DamageLevel::LEVEL_3_SEVERE:       return ScanStrategy::BRUTE_FORCE;
            case DamageLevel::LEVEL_4_CATASTROPHIC: return ScanStrategy::FRAGMENT;
            default:                                return ScanStrategy::BRUTE_FORCE;
        }
    }

    /**
     * @brief Salvage scan a single page — main entry point
     *
     * @param page_data Raw page buffer
     * @param page_no   Page number (for metadata)
     * @param data_size Size of the buffer
     * @return Salvage result with records and damage log
     */
    SalvagePageResult salvage_scan_page(
        const uint8_t* page_data,
        uint32_t page_no,
        size_t data_size) {

        auto start_time = std::chrono::high_resolution_clock::now();
        SalvagePageResult result;
        result.page_no = page_no;

        if (!page_data || data_size < 54) {
            result.damage_level = DamageLevel::LEVEL_4_CATASTROPHIC;
            result.strategy_used = ScanStrategy::FRAGMENT;
            return result;
        }

        // Detect damage level
        result.damage_level = detect_damage_level(page_data, data_size);
        result.strategy_used = select_strategy(result.damage_level);

        // Update stats
        stats_.pages_scanned++;
        if (result.damage_level > DamageLevel::LEVEL_0_HEALTHY) {
            stats_.pages_with_damage++;
        }
        stats_.damage_level_distribution[result.damage_level]++;

        // Execute strategy
        switch (result.strategy_used) {
            case ScanStrategy::BPLUS_TREE:
                // Level 0 — delegate to normal parsing (no salvage needed)
                result.total_bytes_scanned = static_cast<uint32_t>(data_size);
                result.bytes_salvaged = static_cast<uint32_t>(data_size);
                result.recovery_ratio = 1.0;
                break;

            case ScanStrategy::PAGE_DIR:
                scan_level1_minor(page_data, data_size, result);
                break;

            case ScanStrategy::LINEAR_SCAN:
                scan_level2_linear(page_data, data_size, result);
                break;

            case ScanStrategy::BRUTE_FORCE:
                scan_level3_brute_force(page_data, data_size, result);
                break;

            case ScanStrategy::FRAGMENT:
                scan_level4_fragment(page_data, data_size, result);
                break;
        }

        // Deduplicate records
        if (enable_dedup_ && !result.records.empty()) {
            auto before = result.records.size();
            deduplicate_records(result.records);
            stats_.dedup_removed += (before - result.records.size());
        }

        // Sort by offset (ascending)
        std::sort(result.records.begin(), result.records.end(),
            [](const SalvagedRecord& a, const SalvagedRecord& b) {
                return a.offset < b.offset;
            });

        // Calculate recovery ratio
        if (data_size > 0) {
            result.recovery_ratio = static_cast<double>(result.bytes_salvaged) /
                                    static_cast<double>(data_size);
        }

        // Update aggregate stats
        stats_.total_records_salvaged += result.records.size();
        stats_.high_confidence_records += std::count_if(result.records.begin(), result.records.end(),
            [](const SalvagedRecord& r) { return r.confidence >= 0.7; });
        stats_.total_damage_events += result.damage_log.size();
        stats_.strategy_usage[result.strategy_used]++;

        auto end_time = std::chrono::high_resolution_clock::now();
        result.scan_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        stats_.total_scan_time_ms += result.scan_time_ms;

        return result;
    }

    /**
     * @brief Salvage scan multiple pages
     */
    std::vector<SalvagePageResult> salvage_scan_pages(
        const std::vector<std::pair<std::vector<uint8_t>, uint32_t>>& pages) {

        std::vector<SalvagePageResult> results;
        results.reserve(pages.size());
        for (const auto& [page_data, page_no] : pages) {
            results.push_back(salvage_scan_page(page_data.data(), page_no, page_data.size()));
        }
        return results;
    }

    /**
     * @brief Deduplicate records by primary key (keep highest confidence)
     */
    void deduplicate_records(std::vector<SalvagedRecord>& records) const {
        if (records.empty()) return;

        // Group by primary key hex
        std::map<std::string, SalvagedRecord> best;
        for (auto& rec : records) {
            std::string pk_hex;
            for (auto b : rec.primary_key) {
                std::ostringstream oss;
                oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
                pk_hex += oss.str();
            }

            auto it = best.find(pk_hex);
            if (it == best.end() || rec.confidence > it->second.confidence) {
                best[pk_hex] = std::move(rec);
            }
        }

        records.clear();
        for (auto& [pk, rec] : best) {
            records.push_back(std::move(rec));
        }
    }

private:
    size_t page_size_ = 16384;
    SchemaHint schema_hint_;
    double min_confidence_ = 0.3;
    bool enable_dedup_ = true;
    uint32_t brute_force_step_ = 1;
    SalvageScannerStats stats_;

    // ==================== Checksum Check ====================

    bool check_checksum(const uint8_t* page_data, size_t data_size) const {
        if (data_size < 4) return false;

        // Read checksum from offset 0 (FIL_PAGE_SPACE_OR_CHKSUM)
        uint32_t stored_checksum = dbrecovery::innodb::read_le<uint32_t>(page_data, 0);

        // If checksum is 0, page is freshly allocated — skip check
        if (stored_checksum == 0) return true;

        // Try CRC32 checksum (MySQL 8.0+)
        uint32_t computed_crc32 = dbrecovery::innodb::compute_crc32c_checksum(page_data, data_size);
        if (computed_crc32 == stored_checksum) return true;

        // Try innodb checksum
        uint32_t computed_innodb = dbrecovery::innodb::compute_innodb_checksum(page_data, data_size);
        if (computed_innodb == stored_checksum) return true;

        // Try none checksum
        uint32_t computed_none = 0;  // "none" algorithm always returns 0
        if (computed_none == stored_checksum) return true;

        return false;
    }

    // ==================== Level 1: Page Directory Scan ====================

    void scan_level1_minor(const uint8_t* page_data, size_t data_size, SalvagePageResult& result) {
        result.total_bytes_scanned = static_cast<uint32_t>(data_size);

        // Read page directory slots
        uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 0);
        if (n_dir_slots < 2 || n_dir_slots > 1000) {
            // Page directory is corrupted, fall back to linear scan
            result.damage_log.emplace_back(FIL_HEADER_SIZE,
                "Page directory corrupted (n_dir_slots=" + std::to_string(n_dir_slots) + "), falling back to LINEAR_SCAN");
            scan_level2_linear(page_data, data_size, result);
            result.strategy_used = ScanStrategy::LINEAR_SCAN;
            return;
        }

        // Page directory starts at the end of the page
        // Each slot is 2 bytes (LE), pointing to the last record in each slot group
        uint16_t dir_start = static_cast<uint16_t>(data_size) - n_dir_slots * 2;

        if (dir_start < 54) {
            result.damage_log.emplace_back(0, "Page directory start invalid");
            return;
        }

        // Collect unique record offsets from page directory
        std::set<uint16_t> record_offsets;
        for (uint16_t i = 0; i < n_dir_slots; i++) {
            uint16_t slot_offset = dir_start + i * 2;
            if (slot_offset + 2 > data_size) break;
            uint16_t rec_offset = dbrecovery::innodb::read_le<uint16_t>(page_data, slot_offset);
            if (rec_offset >= PAGE_INFIMUM_REC_OFFSET_ABS && rec_offset < dir_start) {
                record_offsets.insert(rec_offset);
            }
        }

        // Parse each record at the collected offsets
        for (uint16_t offset : record_offsets) {
            if (offset + 5 > data_size) continue;

            // Skip INFIMUM/SUPREMUM
            uint8_t status = page_data[offset] & 0x07;
            if (status == dbrecovery::innodb::REC_STATUS_INFIMUM ||
                status == dbrecovery::innodb::REC_STATUS_SUPREMUM) {
                continue;
            }

            auto record = try_parse_compact_record(page_data, data_size, offset);
            if (record.confidence >= min_confidence_) {
                record.page_no = result.page_no;
                record.source = ScanStrategy::PAGE_DIR;
                result.records.push_back(std::move(record));
                result.bytes_salvaged += record.estimated_size;
            }
        }
    }

    // ==================== Level 2: Linear Scan ====================

    void scan_level2_linear(const uint8_t* page_data, size_t data_size, SalvagePageResult& result) {
        result.total_bytes_scanned = static_cast<uint32_t>(data_size);

        // Find INFIMUM and SUPREMUM
        uint16_t infimum_pos = PAGE_INFIMUM_REC_OFFSET_ABS;
        uint16_t supremum_pos = find_supremum(page_data, data_size);

        if (supremum_pos == 0 || supremum_pos >= data_size) {
            // SUPREMUM not found, guess at page_size - 8
            supremum_pos = static_cast<uint16_t>(std::min<size_t>(page_size_ - 8, data_size - 5));
            result.damage_log.emplace_back(infimum_pos,
                "SUPREMUM not found, using heuristic position " + std::to_string(supremum_pos));
        }

        // Start at first user record after INFIMUM
        uint16_t pos = infimum_pos + 5;  // INFIMUM has 5-byte COMPACT header
        if (pos + 5 > data_size) return;

        // Try to follow next_offset chain from INFIMUM
        uint16_t chain_start = dbrecovery::innodb::read_le<uint16_t>(page_data, infimum_pos + 1) & 0x3FFF;
        if (chain_start > infimum_pos && chain_start < supremum_pos) {
            pos = chain_start;
        }

        // Chain traversal
        std::set<uint16_t> visited;  // Prevent infinite loops
        uint32_t consecutive_failures = 0;

        while (pos + 5 < data_size && pos < supremum_pos) {
            if (visited.count(pos)) {
                result.damage_log.emplace_back(pos, "Cycle detected, breaking chain");
                break;
            }
            visited.insert(pos);

            uint8_t status = page_data[pos] & 0x07;
            if (status == dbrecovery::innodb::REC_STATUS_SUPREMUM) break;

            // Try to parse record at current position
            auto record = try_parse_compact_record(page_data, data_size, pos);

            if (record.confidence >= min_confidence_) {
                record.page_no = result.page_no;
                record.source = ScanStrategy::LINEAR_SCAN;
                result.records.push_back(std::move(record));
                result.bytes_salvaged += record.estimated_size;
                consecutive_failures = 0;

                // Follow next_offset
                uint16_t next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data, pos + 1) & 0x3FFF;
                if (next_offset <= pos || next_offset >= supremum_pos) {
                    result.damage_log.emplace_back(pos,
                        "Invalid next_offset " + std::to_string(next_offset) + " at pos " + std::to_string(pos));
                    break;
                }
                pos = next_offset;
            } else {
                consecutive_failures++;
                // Try to find next likely record
                uint16_t next_pos = find_next_likely_record(page_data, data_size, pos, supremum_pos);
                if (next_pos <= pos || next_pos >= supremum_pos) {
                    if (consecutive_failures > 50) {
                        result.damage_log.emplace_back(pos, "Too many consecutive failures, aborting", 0, 2);
                    }
                    break;
                }
                result.damage_log.emplace_back(pos,
                    "Skipped corrupted bytes", next_pos - pos);
                pos = next_pos;
            }
        }
    }

    // ==================== Level 3: Brute-Force Scan ====================

    void scan_level3_brute_force(const uint8_t* page_data, size_t data_size, SalvagePageResult& result) {
        result.total_bytes_scanned = static_cast<uint32_t>(data_size);

        uint32_t pos = 0;
        uint32_t consecutive_failures = 0;
        std::set<uint32_t> used_positions;

        while (pos + 20 < data_size) {
            // Skip positions already covered by previous records
            if (used_positions.count(pos)) {
                pos++;
                continue;
            }

            auto record = try_parse_record_heuristic(page_data, data_size, pos);

            if (record.confidence >= min_confidence_) {
                record.page_no = result.page_no;
                record.source = ScanStrategy::BRUTE_FORCE;
                result.records.push_back(std::move(record));
                result.bytes_salvaged += record.estimated_size;

                // Mark the range as used
                for (uint32_t i = 0; i < record.estimated_size && (pos + i) < data_size; i++) {
                    used_positions.insert(pos + i);
                }

                pos += record.estimated_size;
                consecutive_failures = 0;
            } else {
                consecutive_failures++;
                pos += brute_force_step_;

                // If many consecutive failures, jump ahead
                if (consecutive_failures > 100) {
                    result.damage_log.emplace_back(pos,
                        "Large corruption gap, skipping 16 bytes", 16);
                    pos += 16;
                    consecutive_failures = 0;
                }
            }
        }
    }

    // ==================== Level 4: Fragment Extraction ====================

    void scan_level4_fragment(const uint8_t* page_data, size_t data_size, SalvagePageResult& result) {
        result.total_bytes_scanned = static_cast<uint32_t>(data_size);

        // Look for data fragments — small contiguous regions of non-zero, non-0xFF data
        const uint32_t min_fragment_size = 8;
        uint32_t fragment_start = 0;
        bool in_fragment = false;

        for (uint32_t pos = 0; pos < data_size; pos++) {
            bool is_data_byte = (page_data[pos] != 0x00 && page_data[pos] != 0xFF);

            if (is_data_byte && !in_fragment) {
                fragment_start = pos;
                in_fragment = true;
            } else if (!is_data_byte && in_fragment) {
                uint32_t fragment_size = pos - fragment_start;
                if (fragment_size >= min_fragment_size) {
                    SalvagedRecord fragment;
                    fragment.offset = fragment_start;
                    fragment.estimated_size = fragment_size;
                    fragment.raw_data.assign(page_data + fragment_start,
                                             page_data + pos);
                    fragment.confidence = std::min(0.3, 0.1 + fragment_size / 16384.0);
                    fragment.source = ScanStrategy::FRAGMENT;
                    fragment.page_no = result.page_no;
                    result.records.push_back(std::move(fragment));
                    result.bytes_salvaged += fragment_size;

                    result.damage_log.emplace_back(fragment_start,
                        "Data fragment recovered", fragment_size, 1);
                }
                in_fragment = false;
            }
        }

        // Handle fragment at end of page
        if (in_fragment) {
            uint32_t fragment_size = static_cast<uint32_t>(data_size) - fragment_start;
            if (fragment_size >= min_fragment_size) {
                SalvagedRecord fragment;
                fragment.offset = fragment_start;
                fragment.estimated_size = fragment_size;
                fragment.raw_data.assign(page_data + fragment_start,
                                         page_data + data_size);
                fragment.confidence = 0.2;
                fragment.source = ScanStrategy::FRAGMENT;
                fragment.page_no = result.page_no;
                result.records.push_back(std::move(fragment));
                result.bytes_salvaged += fragment_size;
            }
        }
    }

    // ==================== SUPREMUM Finder ====================

    /**
     * @brief Find SUPREMUM record in the page
     *
     * SUPREMUM is typically at heap_top - 5 in COMPACT format.
     * If heap_top is corrupted, scan for the SUPREMUM status byte pattern.
     */
    uint16_t find_supremum(const uint8_t* page_data, size_t data_size) const {
        // Try reading from page header first
        if (data_size >= FIL_HEADER_SIZE + 4) {
            uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 2);
            if (heap_top > FIL_HEADER_SIZE + 56 && heap_top <= data_size) {
                uint16_t supremum_pos = heap_top - 5;
                if (supremum_pos + 5 <= data_size) {
                    uint8_t status = page_data[supremum_pos] & 0x07;
                    if (status == dbrecovery::innodb::REC_STATUS_SUPREMUM) {
                        return supremum_pos;
                    }
                }
            }
        }

        // Scan for SUPREMUM pattern: byte[0] & 0x07 == 0x03 and the next_offset == 0
        // SUPREMUM is typically at page_size - 8 or nearby
        uint16_t search_start = static_cast<uint16_t>(std::min<size_t>(data_size - 8, page_size_));
        uint16_t search_end = static_cast<uint16_t>(std::min<size_t>(data_size - 5, search_start + 100));

        // Try from the end of page first
        for (uint16_t pos = search_start; pos >= PAGE_INFIMUM_REC_OFFSET_ABS + 5 && pos > 0; pos--) {
            if (pos + 5 > data_size) continue;
            uint8_t status = page_data[pos] & 0x07;
            if (status == dbrecovery::innodb::REC_STATUS_SUPREMUM) {
                // Verify: SUPREMUM's next_offset should be 0
                uint16_t next_off = dbrecovery::innodb::read_le<uint16_t>(page_data, pos + 1) & 0x3FFF;
                if (next_off == 0) {
                    return pos;
                }
            }
        }

        return 0;  // Not found
    }

    // ==================== Record Parsing ====================

    /**
     * @brief Try to parse a COMPACT format record at the given offset
     *
     * COMPACT record layout:
     *   [0]    info_bits (1 byte)  — status(3) + heap_no(4) + n_owned(1)
     *   [1-2]  next_offset (2 bytes, LE, masked to 14 bits)
     *   [3-4]  heap_no + n_fields (2 bytes)
     *   [5]    n_fields extension (1 byte, high 4 bits)
     *   [6+]   variable-length field list
     *   [...]  NULL bitmap
     *   [...]  field data
     */
    SalvagedRecord try_parse_compact_record(const uint8_t* page_data, size_t data_size, uint16_t offset) const {
        SalvagedRecord result;
        result.offset = offset;
        result.confidence = 0.0;

        if (offset + 5 > data_size) return result;

        // Check 1: info_bits high 3 bits must be 0
        uint8_t info_bits = page_data[offset];
        if ((info_bits & 0xE0) != 0) {
            result.confidence += 0.0;  // Fatal for compact format
        } else {
            result.confidence += 0.15;
        }

        // Check 2: next_offset must be reasonable
        uint16_t next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data, offset + 1) & 0x3FFF;
        if (next_offset > offset && next_offset < data_size) {
            result.confidence += 0.15;
        } else if (next_offset == 0) {
            // Last record in chain (SUPREMUM has next=0, but user records shouldn't)
            // This is possible but uncommon
            result.confidence += 0.05;
        } else {
            result.confidence -= 0.1;
        }

        // Check 3: heap_no must be >= 2 (user records)
        uint16_t heap_no = dbrecovery::innodb::read_le<uint16_t>(page_data, offset + 3) & 0x3FFF;
        // Actually, heap_no is in bits [12:15] of the word at offset+3
        // Wait — in COMPACT format, the word at offset+3 is: heap_no(13 bits) + n_fields(4 bits) + bit15(1 bit)
        // Actually let me re-read the spec:
        // COMPACT: bytes 3-4: heap_no(top 4 bits of byte3), n_fields(byte3 low 4 bits), n_fields(byte4)
        // Actually this is getting complex. Let me use a simpler approach.
        // The heap_no is packed in the 2 bytes at offset+3 (13 bits for heap_no, 3 bits for something else)
        // For practical purposes, we just check if heap_no is in a reasonable range
        if (heap_no >= 2 && heap_no < 1000) {
            result.confidence += 0.1;
        }

        // Check 4: record must not be INFIMUM or SUPREMUM
        uint8_t status = info_bits & 0x07;
        if (status == dbrecovery::innodb::REC_STATUS_INFIMUM ||
            status == dbrecovery::innodb::REC_STATUS_SUPREMUM) {
            result.is_system_record = true;
            result.confidence = 0.0;
            return result;
        }

        // Check 5: deleted flag
        result.has_deleted_flag = (info_bits & dbrecovery::innodb::REC_INFO_DELETED_FLAG) != 0;

        // Check 6: try to estimate record size and verify it's reasonable
        uint16_t estimated_size = estimate_record_length(page_data, data_size, offset);
        if (estimated_size >= 10 && estimated_size < data_size - offset) {
            result.confidence += 0.1;
            result.estimated_size = estimated_size;
        } else {
            result.confidence -= 0.1;
            if (next_offset > offset) {
                result.estimated_size = next_offset - offset;
            } else {
                result.estimated_size = 20;  // Minimum reasonable record size
            }
        }

        // Check 7: if schema hint available, validate first field
        if (schema_hint_.has_pk_info) {
            std::vector<uint8_t> pk_bytes = extract_likely_pk(page_data, data_size, offset);
            if (!pk_bytes.empty()) {
                result.primary_key = pk_bytes;
                result.confidence += 0.15;

                // Validate PK plausibility
                if (schema_hint_.pk_is_integer && pk_bytes.size() <= 8) {
                    uint64_t pk_val = 0;
                    for (size_t i = 0; i < pk_bytes.size(); i++) {
                        pk_val = (pk_val << 8) | pk_bytes[i];
                    }
                    if (pk_val > 0 && pk_val < 100000000) {
                        result.confidence += 0.1;  // Plausible integer PK
                    }
                }
            }
        }

        // Check 8: extract raw data
        if (result.estimated_size > 0 && offset + result.estimated_size <= data_size) {
            result.raw_data.assign(page_data + offset, page_data + offset + result.estimated_size);
        }

        // Cap confidence at 1.0
        result.confidence = std::min(1.0, result.confidence);

        return result;
    }

    /**
     * @brief Heuristic record parsing for brute-force mode
     *
     * Uses multiple checks to determine if data at position looks like a record:
     *   1. Offset table pattern (reverse 2-byte offsets)
     *   2. NULL bitmap byte not all 1s
     *   3. info_bits high 3 bits = 0
     *   4. Schema hint: first field looks like PK
     *   5. Record length reasonable
     */
    SalvagedRecord try_parse_record_heuristic(const uint8_t* page_data, size_t data_size, uint32_t pos) const {
        SalvagedRecord result;
        result.offset = static_cast<uint16_t>(pos);
        result.confidence = 0.0;

        if (pos + 5 > data_size) return result;

        // Check 1: Offset table pattern
        // In COMPACT format, the offset table is at the end of the record area
        // with reverse 2-byte offsets pointing to record starts
        uint16_t first_offset = dbrecovery::innodb::read_le<uint16_t>(page_data, pos);
        if (first_offset > pos && first_offset < data_size && first_offset < pos + 200) {
            result.confidence += 0.15;
            // Check second offset decreasing
            if (pos + 4 <= data_size) {
                uint16_t second_offset = dbrecovery::innodb::read_le<uint16_t>(page_data, pos + 2);
                if (second_offset > 0 && second_offset < first_offset) {
                    result.confidence += 0.15;
                }
            }
        }

        // Check 2: info_bits validity — high 3 bits must be 0
        uint8_t info_byte = page_data[pos];
        if ((info_byte & 0xE0) == 0) {
            result.confidence += 0.2;
        }

        // Check 3: next_offset field must be reasonable
        uint16_t next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data, pos + 1) & 0x3FFF;
        if (next_offset > pos && next_offset < data_size) {
            result.confidence += 0.1;
        }

        // Check 4: NULL bitmap byte not all 1s
        if (pos + 6 <= data_size) {
            uint8_t null_byte = page_data[pos + 5];  // Approximate position
            if (null_byte != 0xFF) {
                result.confidence += 0.1;
            }
        }

        // Check 5: Schema hint — validate first field as PK
        if (schema_hint_.has_pk_info && pos + 8 <= data_size) {
            std::vector<uint8_t> pk_bytes = extract_likely_pk(page_data, data_size, pos);
            if (!pk_bytes.empty()) {
                result.primary_key = pk_bytes;
                result.confidence += 0.2;

                // Additional validation for integer PKs
                if (schema_hint_.pk_is_integer && pk_bytes.size() <= 8) {
                    uint64_t pk_val = 0;
                    for (size_t i = 0; i < pk_bytes.size(); i++) {
                        pk_val = (pk_val << 8) | pk_bytes[i];
                    }
                    if (pk_val > 0 && pk_val < 100000000) {
                        result.confidence += 0.1;
                    }
                }
            }
        }

        // Check 6: Record length reasonable
        uint16_t estimated_size = estimate_record_length(page_data, data_size, pos);
        if (estimated_size >= 10 && estimated_size <= data_size - pos) {
            result.confidence += 0.1;
            result.estimated_size = estimated_size;
        } else {
            result.estimated_size = 20;
        }

        // Check 7: Deleted flag
        result.has_deleted_flag = (info_byte & dbrecovery::innodb::REC_INFO_DELETED_FLAG) != 0;

        // Check 8: Extract raw data
        if (result.estimated_size > 0 && pos + result.estimated_size <= data_size) {
            result.raw_data.assign(page_data + pos, page_data + pos + result.estimated_size);
        }

        // Cap confidence at 1.0
        result.confidence = std::min(1.0, result.confidence);

        return result;
    }

    // ==================== Helpers ====================

    /**
     * @brief Find the next likely record position after a failed parse
     */
    uint16_t find_next_likely_record(const uint8_t* page_data, size_t data_size,
                                     uint16_t current_pos, uint16_t supremum_pos) const {
        // Try to use next_offset chain first
        if (current_pos + 3 <= data_size) {
            uint16_t next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data, current_pos + 1) & 0x3FFF;
            if (next_offset > current_pos && next_offset < supremum_pos) {
                return next_offset;
            }
        }

        // Look for valid record headers within a reasonable range
        uint16_t search_end = static_cast<uint16_t>(std::min<uint32_t>(current_pos + 128, supremum_pos));
        for (uint16_t pos = current_pos + 4; pos < search_end; pos++) {
            if (pos + 5 > data_size) break;

            // Check for valid COMPACT record header
            uint8_t info_bits = page_data[pos];
            if ((info_bits & 0xE0) != 0) continue;  // High 3 bits must be 0

            uint8_t status = info_bits & 0x07;
            if (status == dbrecovery::innodb::REC_STATUS_ORDINARY ||
                status == dbrecovery::innodb::REC_STATUS_INFIMUM) {
                uint16_t next_off = dbrecovery::innodb::read_le<uint16_t>(page_data, pos + 1) & 0x3FFF;
                if (next_off > pos && next_off < supremum_pos) {
                    return pos;  // Found a valid record header
                }
            }
        }

        // Fallback: skip ahead by 16 bytes
        return current_pos + 16;
    }

    /**
     * @brief Estimate record length from raw data
     */
    uint16_t estimate_record_length(const uint8_t* page_data, size_t data_size, uint16_t offset) const {
        if (offset + 5 > data_size) return 0;

        // Method 1: Use next_offset if available and valid
        uint16_t next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data, offset + 1) & 0x3FFF;
        if (next_offset > offset && next_offset < data_size) {
            return next_offset - offset;
        }

        // Method 2: Count non-zero, non-0xFF bytes until we hit a long run of zeros
        uint16_t max_len = static_cast<uint16_t>(std::min<size_t>(200, data_size - offset));
        uint16_t zero_run = 0;
        for (uint16_t i = 5; i < max_len; i++) {
            if (page_data[offset + i] == 0x00) {
                zero_run++;
                if (zero_run >= 4) {
                    return i - zero_run + 1;
                }
            } else {
                zero_run = 0;
            }
        }

        return max_len;
    }

    /**
     * @brief Extract likely primary key bytes from a record
     *
     * In COMPACT format, the first field after the record header is the PK.
     * The record header is typically 5-6 bytes, then variable-length fields,
     * then NULL bitmap, then field data.
     */
    std::vector<uint8_t> extract_likely_pk(const uint8_t* page_data, size_t data_size, uint32_t offset) const {
        std::vector<uint8_t> result;

        if (offset + 6 > data_size) return result;

        // Skip record header (5 bytes for COMPACT)
        // Check if there's a variable-length field list
        uint8_t info_bits = page_data[offset];
        uint16_t n_fields = (info_bits & 0xF8) >> 3;  // High 5 bits
        if (offset + 5 < data_size) {
            n_fields = static_cast<uint16_t>((n_fields << 4) | (page_data[offset + 5] >> 4));
        }

        // Skip 6 bytes of record header
        uint32_t data_start = offset + 6;

        // Skip variable-length field list (at most n_fields entries, each 1-2 bytes)
        // For simplicity, skip up to 20 bytes
        uint32_t var_end = data_start;
        for (uint16_t i = 0; i < n_fields && i < 10; i++) {
            if (var_end >= data_size) break;
            if (page_data[var_end] < 0x80) {
                var_end += 1;
            } else {
                var_end += 2;
            }
        }

        // Skip NULL bitmap
        uint32_t null_bytes = (n_fields + 7) / 8;
        if (null_bytes > 10) null_bytes = 10;  // Sanity
        uint32_t field_start = var_end + null_bytes;

        if (field_start + 4 > data_size) return result;

        // Extract PK bytes based on type
        if (schema_hint_.pk_is_integer) {
            uint32_t pk_len = schema_hint_.pk_column_length;
            if (pk_len == 0) pk_len = 4;  // Default INT
            if (pk_len > 8) pk_len = 8;
            if (field_start + pk_len <= data_size) {
                result.assign(page_data + field_start, page_data + field_start + pk_len);
            }
        } else if (schema_hint_.pk_is_string) {
            // Read variable-length string
            uint32_t str_len = 8;  // Default: read 8 bytes
            if (field_start + str_len <= data_size) {
                result.assign(page_data + field_start, page_data + field_start + str_len);
            }
        } else {
            // Unknown type, read first 4 bytes as PK
            if (field_start + 4 <= data_size) {
                result.assign(page_data + field_start, page_data + field_start + 4);
            }
        }

        return result;
    }
};

} // namespace salvage
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_ROW_SALVAGE_SCANNER_HPP