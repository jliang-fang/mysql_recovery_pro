/**
 * @file row_salvage_scanner.cpp
 * @brief RowSalvageScanner implementation
 *
 * Extracted from row_salvage_scanner.hpp for better compilation
 * boundaries and link-time efficiency.
 */

#include "dbrecovery/salvage/row_salvage_scanner.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace dbrecovery {
namespace salvage {

using innodb::FIL_HEADER_SIZE;
using innodb::PAGE_INFIMUM_REC_OFFSET_ABS;

// ==================== damage_level_to_string ====================

std::string damage_level_to_string(DamageLevel level) {
    switch (level) {
        case DamageLevel::LEVEL_0_HEALTHY:      return "HEALTHY";
        case DamageLevel::LEVEL_1_MINOR:        return "MINOR_DAMAGE";
        case DamageLevel::LEVEL_2_MODERATE:     return "MODERATE_DAMAGE";
        case DamageLevel::LEVEL_3_SEVERE:       return "SEVERE_DAMAGE";
        case DamageLevel::LEVEL_4_CATASTROPHIC: return "CATASTROPHIC_DAMAGE";
        default:                                return "UNKNOWN";
    }
}

// ==================== SchemaHint::from_table_schema ====================

SchemaHint SchemaHint::from_table_schema(const catalog::TableSchema& schema) {
    SchemaHint hint;
    hint.expected_columns = static_cast<uint32_t>(schema.columns.size());

    hint.nullable_count = 0;
    for (const auto& [ord, col] : schema.columns) {
        if (col.is_nullable) hint.nullable_count++;
        hint.column_types.push_back(col.mysql_type_id);

        if (col.mysql_type_id == innodb::mysql_types::DATETIME ||
            col.mysql_type_id == innodb::mysql_types::TIMESTAMP ||
            col.mysql_type_id == innodb::mysql_types::TIMESTAMP2 ||
            col.mysql_type_id == innodb::mysql_types::DATETIME2) {
            hint.timestamp_column_positions.insert(col.physical_pos);
        }

        if (col.is_auto_increment) {
            hint.auto_increment_position = col.physical_pos;
        }
    }

    for (const auto& idx : schema.indexes) {
        if (idx.is_primary && !idx.columns.empty()) {
            hint.has_pk_info = true;
            const auto& pk_col_name = idx.columns[0];
            for (const auto& [ord, col] : schema.columns) {
                if (col.name == pk_col_name) {
                    hint.pk_column_type = col.mysql_type_id;
                    hint.pk_column_length = col.column_length;
                    hint.pk_is_integer = (col.mysql_type_id == innodb::mysql_types::TINY ||
                                          col.mysql_type_id == innodb::mysql_types::SHORT ||
                                          col.mysql_type_id == innodb::mysql_types::LONG ||
                                          col.mysql_type_id == innodb::mysql_types::LONGLONG ||
                                          col.mysql_type_id == innodb::mysql_types::INT24 ||
                                          col.mysql_type_id == innodb::mysql_types::YEAR);
                    hint.pk_is_string = (col.mysql_type_id == innodb::mysql_types::VARCHAR ||
                                         col.mysql_type_id == innodb::mysql_types::STRING ||
                                         col.mysql_type_id == innodb::mysql_types::VAR_STRING);
                    break;
                }
            }
            break;
        }
    }
    return hint;
}

// ==================== SalvageScannerStats::reset ====================

void SalvageScannerStats::reset() {
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

// ==================== RowSalvageScanner — detect_damage_level ====================

DamageLevel RowSalvageScanner::detect_damage_level(
    const uint8_t* page_data, size_t data_size) const {

    if (!page_data || data_size < FIL_HEADER_SIZE + 56) {
        return DamageLevel::LEVEL_4_CATASTROPHIC;
    }

    uint32_t failures = 0;
    uint32_t total_checks = 0;

    // Check 1: Checksum
    total_checks++;
    if (!check_checksum(page_data, data_size)) failures++;

    // Check 2: SPACE_ID range
    total_checks++;
    uint32_t space_id = innodb::read_le<uint32_t>(page_data, 34);
    if (space_id > 0xFFFFFF) failures++;

    // Check 3: PAGE_NO range
    total_checks++;
    uint32_t page_no = innodb::read_le<uint32_t>(page_data, 30);
    if (page_no > 0xFFFFFF) failures++;

    // Check 4: Page type
    total_checks++;
    uint16_t page_type = innodb::read_be<uint16_t>(page_data, 24);
    if (!innodb::is_valid_page_type(page_type)) failures++;

    // Check 5: LSN sanity
    total_checks++;
    uint64_t lsn = innodb::read_le<uint64_t>(page_data, 16);
    if (lsn == 0 || lsn > 0xFFFFFFFFFFFFULL) failures++;

    // Check 6: PAGE_HEADER structure
    total_checks++;
    uint16_t n_dir_slots = innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 0);
    uint16_t heap_top = innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 2);
    bool page_header_ok = (n_dir_slots >= 2 && n_dir_slots <= 1000 &&
                           heap_top > FIL_HEADER_SIZE + 56 &&
                           heap_top <= data_size);
    if (!page_header_ok) failures++;

    // Check 7: INFIMUM presence
    total_checks++;
    if (PAGE_INFIMUM_REC_OFFSET_ABS + 5 <= data_size) {
        uint8_t info_bits = page_data[PAGE_INFIMUM_REC_OFFSET_ABS];
        uint8_t status = info_bits & 0x07;
        if (status != innodb::REC_STATUS_INFIMUM) failures++;
    } else {
        failures++;
    }

    // Check 8: SUPREMUM presence
    total_checks++;
    if (page_header_ok) {
        uint16_t supremum_pos = heap_top - 5;
        if (supremum_pos > 0 && supremum_pos + 5 <= data_size) {
            uint8_t sup_status = page_data[supremum_pos] & 0x07;
            if (sup_status != innodb::REC_STATUS_SUPREMUM) failures++;
        } else {
            failures++;
        }
    }

    // Check 9: Offset table integrity
    total_checks++;
    if (page_header_ok) {
        uint16_t heap_top_check = innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 2);
        uint16_t n_heap = innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 4);
        uint16_t n_recs = n_heap & 0x3FFF;
        if (n_recs > 0) {
            uint16_t offset_table_end = heap_top_check;
            if (offset_table_end >= n_recs * 2 + 2 && offset_table_end <= data_size) {
                uint16_t first_offset = innodb::read_le<uint16_t>(page_data, offset_table_end - 2);
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
        uint16_t supremum_pos = innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 2) - 5;
        if (supremum_pos + 5 <= data_size) {
            uint16_t chain_next = innodb::read_le<uint16_t>(page_data, supremum_pos + 1) & 0x3FFF;
            if (chain_next != 0) failures++;
        }
    }

    double fail_ratio = static_cast<double>(failures) / static_cast<double>(total_checks);
    if (fail_ratio == 0.0) return DamageLevel::LEVEL_0_HEALTHY;
    if (fail_ratio <= 0.2) return DamageLevel::LEVEL_1_MINOR;
    if (fail_ratio <= 0.4) return DamageLevel::LEVEL_2_MODERATE;
    if (fail_ratio <= 0.7) return DamageLevel::LEVEL_3_SEVERE;
    return DamageLevel::LEVEL_4_CATASTROPHIC;
}

// ==================== RowSalvageScanner — select_strategy ====================

ScanStrategy RowSalvageScanner::select_strategy(DamageLevel level) {
    switch (level) {
        case DamageLevel::LEVEL_0_HEALTHY:      return ScanStrategy::BPLUS_TREE;
        case DamageLevel::LEVEL_1_MINOR:        return ScanStrategy::PAGE_DIR;
        case DamageLevel::LEVEL_2_MODERATE:     return ScanStrategy::LINEAR_SCAN;
        case DamageLevel::LEVEL_3_SEVERE:       return ScanStrategy::BRUTE_FORCE;
        case DamageLevel::LEVEL_4_CATASTROPHIC: return ScanStrategy::FRAGMENT;
        default:                                return ScanStrategy::BRUTE_FORCE;
    }
}

// ==================== RowSalvageScanner — salvage_scan_page ====================

SalvagePageResult RowSalvageScanner::salvage_scan_page(
    const uint8_t* page_data, uint32_t page_no, size_t data_size) {

    auto start_time = std::chrono::high_resolution_clock::now();
    SalvagePageResult result;
    result.page_no = page_no;

    if (!page_data || data_size < 54) {
        result.damage_level = DamageLevel::LEVEL_4_CATASTROPHIC;
        result.strategy_used = ScanStrategy::FRAGMENT;
        return result;
    }

    result.damage_level = detect_damage_level(page_data, data_size);
    result.strategy_used = select_strategy(result.damage_level);

    stats_.pages_scanned++;
    if (result.damage_level > DamageLevel::LEVEL_0_HEALTHY) {
        stats_.pages_with_damage++;
    }
    stats_.damage_level_distribution[result.damage_level]++;

    switch (result.strategy_used) {
        case ScanStrategy::BPLUS_TREE:
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

    if (enable_dedup_ && !result.records.empty()) {
        auto before = result.records.size();
        deduplicate_records(result.records);
        stats_.dedup_removed += (before - result.records.size());
    }

    std::sort(result.records.begin(), result.records.end(),
        [](const SalvagedRecord& a, const SalvagedRecord& b) {
            return a.offset < b.offset;
        });

    if (data_size > 0) {
        result.recovery_ratio = static_cast<double>(result.bytes_salvaged) /
                                static_cast<double>(data_size);
    }

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

// ==================== RowSalvageScanner — salvage_scan_pages ====================

std::vector<SalvagePageResult> RowSalvageScanner::salvage_scan_pages(
    const std::vector<std::pair<std::vector<uint8_t>, uint32_t>>& pages) {

    std::vector<SalvagePageResult> results;
    results.reserve(pages.size());
    for (const auto& [page_data, page_no] : pages) {
        results.push_back(salvage_scan_page(page_data.data(), page_no, page_data.size()));
    }
    return results;
}

// ==================== RowSalvageScanner — deduplicate_records ====================

void RowSalvageScanner::deduplicate_records(std::vector<SalvagedRecord>& records) const {
    if (records.empty()) return;

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

// ==================== RowSalvageScanner — check_checksum ====================

bool RowSalvageScanner::check_checksum(const uint8_t* page_data, size_t data_size) const {
    if (data_size < 4) return false;

    uint32_t stored_checksum = innodb::read_le<uint32_t>(page_data, 0);
    if (stored_checksum == 0) return true;

    uint32_t computed_crc32 = innodb::compute_crc32c_checksum(page_data, data_size);
    if (computed_crc32 == stored_checksum) return true;

    uint32_t computed_innodb = innodb::compute_innodb_checksum(page_data, data_size);
    if (computed_innodb == stored_checksum) return true;

    if (0 == stored_checksum) return true;

    return false;
}

// ==================== RowSalvageScanner — scan_level1_minor ====================

void RowSalvageScanner::scan_level1_minor(
    const uint8_t* page_data, size_t data_size, SalvagePageResult& result) {

    result.total_bytes_scanned = static_cast<uint32_t>(data_size);

    uint16_t n_dir_slots = innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 0);
    if (n_dir_slots < 2 || n_dir_slots > 1000) {
        result.damage_log.emplace_back(FIL_HEADER_SIZE,
            "Page directory corrupted (n_dir_slots=" + std::to_string(n_dir_slots) +
            "), falling back to LINEAR_SCAN");
        scan_level2_linear(page_data, data_size, result);
        result.strategy_used = ScanStrategy::LINEAR_SCAN;
        return;
    }

    uint16_t dir_start = static_cast<uint16_t>(data_size) - n_dir_slots * 2;
    if (dir_start < 54) {
        result.damage_log.emplace_back(0, "Page directory start invalid");
        return;
    }

    std::set<uint16_t> record_offsets;
    for (uint16_t i = 0; i < n_dir_slots; i++) {
        uint16_t slot_offset = dir_start + i * 2;
        if (slot_offset + 2 > data_size) break;
        uint16_t rec_offset = innodb::read_le<uint16_t>(page_data, slot_offset);
        if (rec_offset >= PAGE_INFIMUM_REC_OFFSET_ABS && rec_offset < dir_start) {
            record_offsets.insert(rec_offset);
        }
    }

    for (uint16_t offset : record_offsets) {
        if (offset + 5 > data_size) continue;
        uint8_t status = page_data[offset] & 0x07;
        if (status == innodb::REC_STATUS_INFIMUM ||
            status == innodb::REC_STATUS_SUPREMUM) {
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

// ==================== RowSalvageScanner — scan_level2_linear ====================

void RowSalvageScanner::scan_level2_linear(
    const uint8_t* page_data, size_t data_size, SalvagePageResult& result) {

    result.total_bytes_scanned = static_cast<uint32_t>(data_size);

    uint16_t infimum_pos = PAGE_INFIMUM_REC_OFFSET_ABS;
    uint16_t supremum_pos = find_supremum(page_data, data_size);

    if (supremum_pos == 0 || supremum_pos >= data_size) {
        supremum_pos = static_cast<uint16_t>(std::min<size_t>(page_size_ - 8, data_size - 5));
        result.damage_log.emplace_back(infimum_pos,
            "SUPREMUM not found, using heuristic position " + std::to_string(supremum_pos));
    }

    uint16_t pos = infimum_pos + 5;
    if (pos + 5 > data_size) return;

    uint16_t chain_start = innodb::read_le<uint16_t>(page_data, infimum_pos + 1) & 0x3FFF;
    if (chain_start > infimum_pos && chain_start < supremum_pos) {
        pos = chain_start;
    }

    std::set<uint16_t> visited;
    uint32_t consecutive_failures = 0;

    while (pos + 5 < data_size && pos < supremum_pos) {
        if (visited.count(pos)) {
            result.damage_log.emplace_back(pos, "Cycle detected, breaking chain");
            break;
        }
        visited.insert(pos);

        uint8_t status = page_data[pos] & 0x07;
        if (status == innodb::REC_STATUS_SUPREMUM) break;

        auto record = try_parse_compact_record(page_data, data_size, pos);

        if (record.confidence >= min_confidence_) {
            record.page_no = result.page_no;
            record.source = ScanStrategy::LINEAR_SCAN;
            result.records.push_back(std::move(record));
            result.bytes_salvaged += record.estimated_size;
            consecutive_failures = 0;

            uint16_t next_offset = innodb::read_le<uint16_t>(page_data, pos + 1) & 0x3FFF;
            if (next_offset <= pos || next_offset >= supremum_pos) {
                result.damage_log.emplace_back(pos,
                    "Invalid next_offset " + std::to_string(next_offset) +
                    " at pos " + std::to_string(pos));
                break;
            }
            pos = next_offset;
        } else {
            consecutive_failures++;
            uint16_t next_pos = find_next_likely_record(page_data, data_size, pos, supremum_pos);
            if (next_pos <= pos || next_pos >= supremum_pos) {
                if (consecutive_failures > 50) {
                    result.damage_log.emplace_back(pos,
                        "Too many consecutive failures, aborting", 0, 2);
                }
                break;
            }
            result.damage_log.emplace_back(pos,
                "Skipped corrupted bytes", next_pos - pos);
            pos = next_pos;
        }
    }
}

// ==================== RowSalvageScanner — scan_level3_brute_force ====================

void RowSalvageScanner::scan_level3_brute_force(
    const uint8_t* page_data, size_t data_size, SalvagePageResult& result) {

    result.total_bytes_scanned = static_cast<uint32_t>(data_size);

    uint32_t pos = 0;
    uint32_t consecutive_failures = 0;
    std::set<uint32_t> used_positions;

    while (pos + 20 < data_size) {
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

            for (uint32_t i = 0; i < record.estimated_size && (pos + i) < data_size; i++) {
                used_positions.insert(pos + i);
            }

            pos += record.estimated_size;
            consecutive_failures = 0;
        } else {
            consecutive_failures++;
            pos += brute_force_step_;

            if (consecutive_failures > 100) {
                result.damage_log.emplace_back(pos,
                    "Large corruption gap, skipping 16 bytes", 16);
                pos += 16;
                consecutive_failures = 0;
            }
        }
    }
}

// ==================== RowSalvageScanner — scan_level4_fragment ====================

void RowSalvageScanner::scan_level4_fragment(
    const uint8_t* page_data, size_t data_size, SalvagePageResult& result) {

    result.total_bytes_scanned = static_cast<uint32_t>(data_size);

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
                fragment.raw_data.assign(page_data + fragment_start, page_data + pos);
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

    if (in_fragment) {
        uint32_t fragment_size = static_cast<uint32_t>(data_size) - fragment_start;
        if (fragment_size >= min_fragment_size) {
            SalvagedRecord fragment;
            fragment.offset = fragment_start;
            fragment.estimated_size = fragment_size;
            fragment.raw_data.assign(page_data + fragment_start, page_data + data_size);
            fragment.confidence = 0.2;
            fragment.source = ScanStrategy::FRAGMENT;
            fragment.page_no = result.page_no;
            result.records.push_back(std::move(fragment));
            result.bytes_salvaged += fragment_size;
        }
    }
}

// ==================== RowSalvageScanner — find_supremum ====================

uint16_t RowSalvageScanner::find_supremum(
    const uint8_t* page_data, size_t data_size) const {

    if (data_size >= FIL_HEADER_SIZE + 4) {
        uint16_t heap_top = innodb::read_le<uint16_t>(page_data, FIL_HEADER_SIZE + 2);
        if (heap_top > FIL_HEADER_SIZE + 56 && heap_top <= data_size) {
            uint16_t supremum_pos = heap_top - 5;
            if (supremum_pos + 5 <= data_size) {
                uint8_t status = page_data[supremum_pos] & 0x07;
                if (status == innodb::REC_STATUS_SUPREMUM) {
                    return supremum_pos;
                }
            }
        }
    }

    uint16_t search_start = static_cast<uint16_t>(std::min<size_t>(data_size - 8, page_size_));
    uint16_t search_end = static_cast<uint16_t>(std::min<size_t>(data_size - 5, search_start + 100));

    for (uint16_t pos = search_start; pos >= PAGE_INFIMUM_REC_OFFSET_ABS + 5 && pos > 0; pos--) {
        if (pos + 5 > data_size) continue;
        uint8_t status = page_data[pos] & 0x07;
        if (status == innodb::REC_STATUS_SUPREMUM) {
            uint16_t next_off = innodb::read_le<uint16_t>(page_data, pos + 1) & 0x3FFF;
            if (next_off == 0) {
                return pos;
            }
        }
    }

    return 0;
}

// ==================== RowSalvageScanner — try_parse_compact_record ====================

SalvagedRecord RowSalvageScanner::try_parse_compact_record(
    const uint8_t* page_data, size_t data_size, uint16_t offset) const {

    SalvagedRecord result;
    result.offset = offset;
    result.confidence = 0.0;

    if (offset + 5 > data_size) return result;

    uint8_t info_bits = page_data[offset];
    if ((info_bits & 0xE0) != 0) {
        result.confidence += 0.0;
    } else {
        result.confidence += 0.15;
    }

    uint16_t next_offset = innodb::read_le<uint16_t>(page_data, offset + 1) & 0x3FFF;
    if (next_offset > offset && next_offset < data_size) {
        result.confidence += 0.15;
    } else if (next_offset == 0) {
        result.confidence += 0.05;
    } else {
        result.confidence -= 0.1;
    }

    uint16_t heap_no = innodb::read_le<uint16_t>(page_data, offset + 3) & 0x3FFF;
    if (heap_no >= 2 && heap_no < 1000) {
        result.confidence += 0.1;
    }

    uint8_t status = info_bits & 0x07;
    if (status == innodb::REC_STATUS_INFIMUM ||
        status == innodb::REC_STATUS_SUPREMUM) {
        result.is_system_record = true;
        result.confidence = 0.0;
        return result;
    }

    result.has_deleted_flag = (info_bits & innodb::REC_INFO_DELETED_FLAG) != 0;

    uint16_t estimated_size = estimate_record_length(page_data, data_size, offset);
    if (estimated_size >= 10 && estimated_size < data_size - offset) {
        result.confidence += 0.1;
        result.estimated_size = estimated_size;
    } else {
        result.confidence -= 0.1;
        if (next_offset > offset) {
            result.estimated_size = next_offset - offset;
        } else {
            result.estimated_size = 20;
        }
    }

    if (schema_hint_.has_pk_info) {
        std::vector<uint8_t> pk_bytes = extract_likely_pk(page_data, data_size, offset);
        if (!pk_bytes.empty()) {
            result.primary_key = pk_bytes;
            result.confidence += 0.15;

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

    if (result.estimated_size > 0 && offset + result.estimated_size <= data_size) {
        result.raw_data.assign(page_data + offset, page_data + offset + result.estimated_size);
    }

    result.confidence = std::min(1.0, result.confidence);
    return result;
}

// ==================== RowSalvageScanner — try_parse_record_heuristic ====================

SalvagedRecord RowSalvageScanner::try_parse_record_heuristic(
    const uint8_t* page_data, size_t data_size, uint32_t pos) const {

    SalvagedRecord result;
    result.offset = static_cast<uint16_t>(pos);
    result.confidence = 0.0;

    if (pos + 5 > data_size) return result;

    uint16_t first_offset = innodb::read_le<uint16_t>(page_data, pos);
    if (first_offset > pos && first_offset < data_size && first_offset < pos + 200) {
        result.confidence += 0.15;
        if (pos + 4 <= data_size) {
            uint16_t second_offset = innodb::read_le<uint16_t>(page_data, pos + 2);
            if (second_offset > 0 && second_offset < first_offset) {
                result.confidence += 0.15;
            }
        }
    }

    uint8_t info_byte = page_data[pos];
    if ((info_byte & 0xE0) == 0) {
        result.confidence += 0.2;
    }

    uint16_t next_offset = innodb::read_le<uint16_t>(page_data, pos + 1) & 0x3FFF;
    if (next_offset > pos && next_offset < data_size) {
        result.confidence += 0.1;
    }

    if (pos + 6 <= data_size) {
        uint8_t null_byte = page_data[pos + 5];
        if (null_byte != 0xFF) {
            result.confidence += 0.1;
        }
    }

    if (schema_hint_.has_pk_info && pos + 8 <= data_size) {
        std::vector<uint8_t> pk_bytes = extract_likely_pk(page_data, data_size, pos);
        if (!pk_bytes.empty()) {
            result.primary_key = pk_bytes;
            result.confidence += 0.2;

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

    uint16_t estimated_size = estimate_record_length(page_data, data_size, pos);
    if (estimated_size >= 10 && estimated_size <= data_size - pos) {
        result.confidence += 0.1;
        result.estimated_size = estimated_size;
    } else {
        result.estimated_size = 20;
    }

    result.has_deleted_flag = (info_byte & innodb::REC_INFO_DELETED_FLAG) != 0;

    if (result.estimated_size > 0 && pos + result.estimated_size <= data_size) {
        result.raw_data.assign(page_data + pos, page_data + pos + result.estimated_size);
    }

    result.confidence = std::min(1.0, result.confidence);
    return result;
}

// ==================== RowSalvageScanner — find_next_likely_record ====================

uint16_t RowSalvageScanner::find_next_likely_record(
    const uint8_t* page_data, size_t data_size,
    uint16_t current_pos, uint16_t supremum_pos) const {

    if (current_pos + 3 <= data_size) {
        uint16_t next_offset = innodb::read_le<uint16_t>(page_data, current_pos + 1) & 0x3FFF;
        if (next_offset > current_pos && next_offset < supremum_pos) {
            return next_offset;
        }
    }

    uint16_t search_end = static_cast<uint16_t>(
        std::min<uint32_t>(current_pos + 128, supremum_pos));
    for (uint16_t pos = current_pos + 4; pos < search_end; pos++) {
        if (pos + 5 > data_size) break;

        uint8_t info_bits = page_data[pos];
        if ((info_bits & 0xE0) != 0) continue;

        uint8_t status = info_bits & 0x07;
        if (status == innodb::REC_STATUS_ORDINARY ||
            status == innodb::REC_STATUS_INFIMUM) {
            uint16_t next_off = innodb::read_le<uint16_t>(page_data, pos + 1) & 0x3FFF;
            if (next_off > pos && next_off < supremum_pos) {
                return pos;
            }
        }
    }

    return current_pos + 16;
}

// ==================== RowSalvageScanner — estimate_record_length ====================

uint16_t RowSalvageScanner::estimate_record_length(
    const uint8_t* page_data, size_t data_size, uint16_t offset) const {

    if (offset + 5 > data_size) return 0;

    uint16_t next_offset = innodb::read_le<uint16_t>(page_data, offset + 1) & 0x3FFF;
    if (next_offset > offset && next_offset < data_size) {
        return next_offset - offset;
    }

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

// ==================== RowSalvageScanner — extract_likely_pk ====================

std::vector<uint8_t> RowSalvageScanner::extract_likely_pk(
    const uint8_t* page_data, size_t data_size, uint32_t offset) const {

    std::vector<uint8_t> result;

    if (offset + 6 > data_size) return result;

    uint8_t info_bits = page_data[offset];
    uint16_t n_fields = (info_bits & 0xF8) >> 3;
    if (offset + 5 < data_size) {
        n_fields = static_cast<uint16_t>((n_fields << 4) | (page_data[offset + 5] >> 4));
    }

    uint32_t data_start = offset + 6;

    uint32_t var_end = data_start;
    for (uint16_t i = 0; i < n_fields && i < 10; i++) {
        if (var_end >= data_size) break;
        if (page_data[var_end] < 0x80) {
            var_end += 1;
        } else {
            var_end += 2;
        }
    }

    uint32_t null_bytes = (n_fields + 7) / 8;
    if (null_bytes > 10) null_bytes = 10;
    uint32_t field_start = var_end + null_bytes;

    if (field_start + 4 > data_size) return result;

    if (schema_hint_.pk_is_integer) {
        uint32_t pk_len = schema_hint_.pk_column_length;
        if (pk_len == 0) pk_len = 4;
        if (pk_len > 8) pk_len = 8;
        if (field_start + pk_len <= data_size) {
            result.assign(page_data + field_start, page_data + field_start + pk_len);
        }
    } else if (schema_hint_.pk_is_string) {
        uint32_t str_len = 8;
        if (field_start + str_len <= data_size) {
            result.assign(page_data + field_start, page_data + field_start + str_len);
        }
    } else {
        if (field_start + 4 <= data_size) {
            result.assign(page_data + field_start, page_data + field_start + 4);
        }
    }

    return result;
}

} // namespace salvage
} // namespace dbrecovery