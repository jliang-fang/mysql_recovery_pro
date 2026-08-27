/**
 * @file flashback_generator.cpp
 * @brief FlashbackGenerator implementation
 *
 * Key design decisions:
 * - field_no mapping: field_no=1 is old DB_ROLL_PTR (skip), field_no=2+ maps
 *   to active_columns[field_no-2] in physical_pos order
 * - Schema-aware decoding uses standalone decode_field_value() (no page context needed)
 * - DeepUndoAnalyzer handles all undo log scanning internally
 */

#include "dbrecovery/innodb/flashback_generator.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace dbrecovery {
namespace innodb {

// ==================== Construction ====================

FlashbackGenerator::FlashbackGenerator()
    : analyzer_(std::make_unique<DeepUndoAnalyzer>()) {}

void FlashbackGenerator::set_ibdata_path(const std::string& path) {
    ibdata_path_ = path;
    analyzer_->set_ibdata_path(path);
}

void FlashbackGenerator::set_page_size(size_t page_size) {
    page_size_ = page_size;
    analyzer_->set_page_size(page_size);
}

void FlashbackGenerator::set_config(const FlashbackConfig& config) {
    config_ = config;
    analyzer_->set_max_chain_depth(config_.max_chain_depth);
}

void FlashbackGenerator::set_undo_format_profile(UndoFormatProfile profile) {
    analyzer_->set_undo_format_profile(profile);
}

void FlashbackGenerator::set_mysql_version(const std::string& version) {
    UndoFormatProfile profile = UndoFormatProfile::UNRESOLVED;
    if (version == "5.6") {
        profile = undo_format_profile_for_version(MySQLVersion::MYSQL_56);
    } else if (version == "5.7") {
        profile = undo_format_profile_for_version(MySQLVersion::MYSQL_57);
    } else if (version == "8.0") {
        profile = undo_format_profile_for_version(MySQLVersion::MYSQL_80);
    } else if (version == "8.1") {
        profile = undo_format_profile_for_version(MySQLVersion::MYSQL_81);
    } else if (version == "8.2") {
        profile = undo_format_profile_for_version(MySQLVersion::MYSQL_82);
    } else if (version == "8.3") {
        profile = undo_format_profile_for_version(MySQLVersion::MYSQL_83);
    } else if (version == "8.4") {
        profile = undo_format_profile_for_version(MySQLVersion::MYSQL_84);
    } else if (version == "9.0") {
        profile = undo_format_profile_for_version(MySQLVersion::MYSQL_90);
    }
    analyzer_->set_undo_format_profile(profile);
}

// ==================== Column ordering helper ====================

std::vector<catalog::ColumnInfo> FlashbackGenerator::get_ordered_columns(
    const catalog::TableSchema& schema) const {
    // Use get_active_columns() which filters system columns and sorts by physical_pos
    auto active = schema.get_active_columns(schema.max_row_version);
    return active;
}

// ==================== Field value decoding ====================

std::string FlashbackGenerator::decode_field_value(
    const std::vector<uint8_t>& raw_bytes,
    const catalog::ColumnInfo& col_info,
    bool human_readable) {

    if (raw_bytes.empty()) return "";

    const uint8_t* data = raw_bytes.data();
    size_t len = raw_bytes.size();

    if (!human_readable) {
        return bytes_to_hex(raw_bytes);
    }

    int type_id = static_cast<int>(col_info.mysql_type_id);

    switch (type_id) {
        // === Integer types (all signed by default, little-endian) ===
        case mysql_types::TINY: { // 1
            if (len < 1) return "?";
            int8_t v = static_cast<int8_t>(data[0]);
            return std::to_string(static_cast<int>(v));
        }
        case mysql_types::SHORT: { // 2
            if (len < 2) return "?";
            int16_t v = static_cast<int16_t>(read_le<uint16_t>(data, 0));
            return std::to_string(v);
        }
        case mysql_types::LONG: { // 3
            if (len < 4) return "?";
            int32_t v = static_cast<int32_t>(read_le<uint32_t>(data, 0));
            return std::to_string(v);
        }
        case mysql_types::LONGLONG: { // 8
            if (len < 8) return "?";
            int64_t v = static_cast<int64_t>(read_le<uint64_t>(data, 0));
            return std::to_string(v);
        }
        case mysql_types::INT24: { // 9
            if (len < 3) return "?";
            // 3-byte signed integer, little-endian — read 3 bytes, not 4
            int32_t v = static_cast<int32_t>(data[0])
                      | (static_cast<int32_t>(data[1]) << 8)
                      | (static_cast<int32_t>(data[2]) << 16);
            if (v & 0x800000) v |= ~0xFFFFFF; // sign extend
            return std::to_string(v);
        }

        // === Float types ===
        case mysql_types::FLOAT: { // 4
            if (len < 4) return "?";
            float v;
            memcpy(&v, data, 4);
            char buf[64];
            snprintf(buf, sizeof(buf), "%.15g", static_cast<double>(v));
            return buf;
        }
        case mysql_types::DOUBLE: { // 5
            if (len < 8) return "?";
            double v;
            memcpy(&v, data, 8);
            char buf[64];
            snprintf(buf, sizeof(buf), "%.15g", v);
            return buf;
        }

        // === Temporal types ===
        case mysql_types::DATE: { // 10
            // InnoDB stores DATE as 3 bytes: (year << 9) | (month << 5) | day
            if (len < 3) return "?";
            uint32_t packed = (static_cast<uint32_t>(data[0]) << 16) |
                              (static_cast<uint32_t>(data[1]) << 8) |
                              static_cast<uint32_t>(data[2]);
            uint32_t day   = packed & 0x1F;
            uint32_t month = (packed >> 5) & 0x0F;
            uint32_t year  = (packed >> 9) & 0x7FFF;
            char buf[16];
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u", year, month, day);
            return buf;
        }
        case mysql_types::TIME: { // 11
            // Old TIME format: 3 bytes, HHMMSS packed
            if (len < 3) return "?";
            uint32_t packed = (static_cast<uint32_t>(data[0]) << 16) |
                              (static_cast<uint32_t>(data[1]) << 8) |
                              static_cast<uint32_t>(data[2]);
            uint32_t sec = packed % 100; packed /= 100;
            uint32_t min = packed % 100; packed /= 100;
            uint32_t hour = packed;
            char buf[16];
            snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hour, min, sec);
            return buf;
        }
        case mysql_types::TIME2: { // 19, TIME2=20
            // MySQL 5.6.4+ TIME2 format: 3 bytes base + fractional
            // Base: 3 bytes little-endian, sign bit in bit 23
            if (len < 3) return "?";
            uint32_t base = static_cast<uint32_t>(data[0])
                          | (static_cast<uint32_t>(data[1]) << 8)
                          | (static_cast<uint32_t>(data[2]) << 16);
            bool negative = (base & 0x800000) != 0;
            if (negative) base = (~base & 0x7FFFFF) + 1;
            uint32_t sec = base % 100; base /= 100;
            uint32_t min = base % 100; base /= 100;
            uint32_t hour = base;
            char buf[32];
            if (negative) {
                snprintf(buf, sizeof(buf), "-%02u:%02u:%02u", hour, min, sec);
            } else {
                snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hour, min, sec);
            }
            return buf;
        }
        case mysql_types::DATETIME: { // 12
            // Old DATETIME: 8 bytes, YYYYMMDDHHMMSS as big-endian int64
            if (len < 8) return "?";
            uint64_t packed = read_be<uint64_t>(data, 0);
            uint32_t sec   = packed % 100; packed /= 100;
            uint32_t min   = packed % 100; packed /= 100;
            uint32_t hour  = packed % 100; packed /= 100;
            uint32_t day   = packed % 100; packed /= 100;
            uint32_t month = packed % 100; packed /= 100;
            uint32_t year  = static_cast<uint32_t>(packed);
            char buf[24];
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
                     year, month, day, hour, min, sec);
            return buf;
        }
        case mysql_types::DATETIME2: { // 18
            // MySQL 5.6.4+ DATETIME2: 5 bytes base + 0-3 bytes fractional
            if (len < 5) return "?";
            // Base: 5 bytes big-endian, stored as int64_t
            uint64_t packed = 0;
            for (size_t i = 0; i < 5 && i < len; i++) {
                packed = (packed << 8) | data[i];
            }
            // Decode: year/month/day/hour/minute/second from packed
            // Format: YYYYMMDDHHMMSS as a big-endian integer
            uint32_t sec   = packed % 100; packed /= 100;
            uint32_t min   = packed % 100; packed /= 100;
            uint32_t hour  = packed % 100; packed /= 100;
            uint32_t day   = packed % 100; packed /= 100;
            uint32_t month = packed % 100; packed /= 100;
            uint32_t year  = static_cast<uint32_t>(packed);

            char buf[32];
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
                     year, month, day, hour, min, sec);
            return buf;
        }
        case mysql_types::TIMESTAMP2: { // 17
            // TIMESTAMP2: 4 bytes (Unix seconds) + 0-3 bytes fractional
            if (len < 4) return "?";
            uint32_t unix_sec = read_be<uint32_t>(data, 0);
            char buf[32];
            snprintf(buf, sizeof(buf), "%u", unix_sec);
            return buf;
        }
        case mysql_types::YEAR: { // 13
            if (len < 1) return "?";
            uint32_t year = 1900 + data[0];
            return std::to_string(year);
        }

        // === Decimal ===
        case mysql_types::NEWDECIMAL: { // 20
            // Packed decimal format — output hex since full decoding is complex
            return bytes_to_hex(raw_bytes);
        }

        // === ENUM / SET ===
        case mysql_types::ENUM: { // 22
            if (raw_bytes.empty()) return "?";
            uint32_t idx = 0;
            if (len == 1) {
                idx = data[0];
            } else if (len >= 2) {
                idx = read_le<uint16_t>(data, 0);
            }
            if (idx == 0) return "";
            if (idx > 0 && idx <= col_info.enum_values.size()) {
                return col_info.enum_values[idx - 1];
            }
            // If values list is empty, show the index
            return std::to_string(idx);
        }
        case mysql_types::SET: { // 23
            if (raw_bytes.empty()) return "?";
            uint64_t bits = 0;
            for (size_t i = 0; i < len && i < 8; i++) {
                bits |= static_cast<uint64_t>(data[i]) << (i * 8);
            }
            // Build SET value string from bitmask
            std::string result;
            for (size_t i = 0; i < col_info.set_values.size(); i++) {
                if (bits & (1ULL << i)) {
                    if (!result.empty()) result += ",";
                    result += col_info.set_values[i];
                }
            }
            return result.empty() ? "" : result;
        }

        // === String types ===
        case mysql_types::VARCHAR:   // 15
        case mysql_types::VAR_STRING: // 28
        case mysql_types::STRING: {  // 29 (CHAR)
            // For CHAR with utf8mb4, length is variable, data is raw
            // Check for printable ASCII
            bool printable = true;
            for (size_t i = 0; i < len && printable; i++) {
                if (data[i] < 0x20 && data[i] != '\t' && data[i] != '\n') {
                    printable = false;
                }
            }
            if (printable && !raw_bytes.empty()) {
                return std::string(reinterpret_cast<const char*>(data), len);
            }
            // Non-printable: output hex
            return bytes_to_hex(raw_bytes);
        }

        // === BLOB / TEXT ===
        case mysql_types::BLOB: // 27
        case mysql_types::TINY_BLOB:   // 249
        case mysql_types::MEDIUM_BLOB: // 250
        case mysql_types::LONG_BLOB: { // 251
            // BLOB data in undo logs is typically inline (small) or a reference
            // For inline data, try to show as hex
            return bytes_to_hex(raw_bytes);
        }

        // === JSON ===
        case mysql_types::JSON: { // 31
            // JSON in undo logs is stored as raw bytes
            // Try to show as string if printable, otherwise hex
            bool printable = true;
            for (size_t i = 0; i < len && printable; i++) {
                if (data[i] < 0x20 && data[i] != '\t' && data[i] != '\n' && data[i] != '\r') {
                    printable = false;
                }
            }
            if (printable && !raw_bytes.empty()) {
                return std::string(reinterpret_cast<const char*>(data), len);
            }
            return bytes_to_hex(raw_bytes);
        }

        // === BIT ===
        case mysql_types::BIT: { // 16
            // BIT(N) stored as (N+7)/8 bytes
            if (raw_bytes.empty()) return "0";
            uint64_t val = 0;
            for (size_t i = 0; i < len && i < 8; i++) {
                val = (val << 8) | data[i];
            }
            return std::to_string(val);
        }

        default:
            return bytes_to_hex(raw_bytes);
    }
}

std::string FlashbackGenerator::bytes_to_hex(const std::vector<uint8_t>& data) {
    if (data.empty()) return "";
    std::ostringstream oss;
    for (auto b : data) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

// ==================== Field number mapping ====================

std::optional<uint32_t> FlashbackGenerator::field_no_to_ordinal(
    uint16_t field_no, bool is_delete) {

    if (is_delete) {
        // For DELETE_MARK records:
        // field_no=1 = old DB_ROLL_PTR (skip)
        // field_no=2 = first user column, maps to ordinal offset 0
        if (field_no <= 1) return std::nullopt;
        return static_cast<uint32_t>(field_no - 2);
    }
    // For UPDATE records: field_no=1 = old DB_ROLL_PTR
    if (field_no <= 1) return std::nullopt;
    return static_cast<uint32_t>(field_no - 2);
}

// ==================== Single record decoding ====================

FlashbackRecord FlashbackGenerator::decode_single_record(
    const UndoRecordV2& rec,
    const catalog::TableSchema& schema) {

    FlashbackRecord fr;

    // Determine operation type
    if (rec.type == UndoRecordV2::DELETE_MARK) {
        fr.operation = FlashbackRecord::DELETE_OP;
    } else if (rec.type == UndoRecordV2::INSERT) {
        fr.operation = FlashbackRecord::INSERT;
    } else {
        fr.operation = FlashbackRecord::UPDATE;
    }

    fr.trx_id = rec.trx_id;
    fr.undo_no = rec.undo_no;
    fr.source_type = rec.type_str();

    // Get ordered columns (physical_pos order, system columns excluded)
    auto ordered_cols = get_ordered_columns(schema);
    fr.n_columns_total = static_cast<uint32_t>(ordered_cols.size());

    // Map undo record fields to columns
    // field_no=1 is old DB_ROLL_PTR (skip)
    // field_no=2+ maps to ordered_cols[field_no-2]
    uint32_t found = 0;

    for (const auto& fv : rec.old_fields) {
        auto ord_opt = field_no_to_ordinal(fv.field_no,
            rec.type == UndoRecordV2::DELETE_MARK);
        if (!ord_opt) continue; // Skip DB_ROLL_PTR

        uint32_t idx = *ord_opt;
        if (idx >= ordered_cols.size()) continue;

        const auto& col = ordered_cols[idx];

        DecodedField df;
        df.column_name = col.name;
        df.is_null = fv.is_null;
        df.hex_value = bytes_to_hex(fv.old_value);

        if (fv.is_null) {
            df.human_value = "NULL";
            df.is_decoded = true;
        } else {
            df.human_value = decode_field_value(fv.old_value, col, config_.human_readable);
            df.is_decoded = true;
            df.confidence = fv.old_value.empty() ? 0.0 : 1.0;
        }

        fr.fields.push_back(std::move(df));
        fr.field_map[col.name] = fr.fields.back().human_value;
        found++;
    }

    fr.n_columns_found = found;
    fr.completeness = fr.n_columns_total > 0
        ? static_cast<double>(found) / static_cast<double>(fr.n_columns_total)
        : 0.0;
    fr.is_complete = (found >= fr.n_columns_total);

    return fr;
}

// ==================== Flashback DELETE ====================

std::vector<FlashbackRecord> FlashbackGenerator::flashback_deletes(
    const catalog::TableSchema& schema) {

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<FlashbackRecord> records;

    // Delegate the undo scan to DeepUndoAnalyzer's shared primitive
    innodb::FlashbackFilter filter;
    filter.trx_id = config_.trx_id;
    filter.start_lsn = config_.start_lsn;
    filter.end_lsn = config_.end_lsn;
    filter.start_time = config_.start_time;
    filter.end_time = config_.end_time;
    filter.max_records = config_.max_records;
    filter.include_incomplete = config_.include_incomplete;

    auto undo_records = analyzer_->scan_records(schema, filter,
        {UndoRecordV2::DELETE_MARK});

    for (auto& rec : undo_records) {
        auto fr = decode_single_record(rec, schema);
        if (config_.include_incomplete || fr.is_complete) {
            records.push_back(std::move(fr));
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.execution_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_records = records.size();

    for (const auto& r : records) {
        if (r.is_complete) stats_.complete_records++;
        else stats_.partial_records++;
        stats_.avg_completeness += r.completeness;
    }
    if (!records.empty()) {
        stats_.avg_completeness /= static_cast<double>(records.size());
    }

    return records;
}

// ==================== Flashback UPDATE ====================

std::vector<FlashbackRecord> FlashbackGenerator::flashback_updates(
    const catalog::TableSchema& schema) {

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<FlashbackRecord> records;

    innodb::FlashbackFilter filter;
    filter.trx_id = config_.trx_id;
    filter.start_lsn = config_.start_lsn;
    filter.end_lsn = config_.end_lsn;
    filter.start_time = config_.start_time;
    filter.end_time = config_.end_time;
    filter.max_records = config_.max_records;
    filter.include_incomplete = config_.include_incomplete;

    auto undo_records = analyzer_->scan_records(schema, filter,
        {UndoRecordV2::UPDATE});

    for (auto& rec : undo_records) {
        auto fr = decode_single_record(rec, schema);
        if (config_.include_incomplete || fr.is_complete) {
            records.push_back(std::move(fr));
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.execution_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_records = records.size();
    for (const auto& r : records) {
        if (r.is_complete) stats_.complete_records++;
        else stats_.partial_records++;
        stats_.avg_completeness += r.completeness;
    }
    if (!records.empty()) {
        stats_.avg_completeness /= static_cast<double>(records.size());
    }

    return records;
}

// ==================== Flashback by transaction ====================

std::vector<FlashbackRecord> FlashbackGenerator::flashback_transaction(
    uint64_t trx_id,
    const catalog::TableSchema& schema) {

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<FlashbackRecord> records;

    innodb::FlashbackFilter filter;
    filter.trx_id = trx_id;
    filter.max_records = config_.max_records;
    filter.include_incomplete = config_.include_incomplete;

    // Scan all record types for this transaction
    auto undo_records = analyzer_->scan_records(schema, filter);

    for (auto& rec : undo_records) {
        auto fr = decode_single_record(rec, schema);
        if (config_.include_incomplete || fr.is_complete) {
            records.push_back(std::move(fr));
        }
    }

    // Sort by undo_no for chronological order within the transaction
    std::sort(records.begin(), records.end(),
        [](const FlashbackRecord& a, const FlashbackRecord& b) {
            return a.undo_no < b.undo_no;
        });

    auto end = std::chrono::high_resolution_clock::now();
    stats_.execution_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_records = records.size();
    for (const auto& r : records) {
        if (r.is_complete) stats_.complete_records++;
        else stats_.partial_records++;
        stats_.avg_completeness += r.completeness;
    }
    if (!records.empty()) {
        stats_.avg_completeness /= static_cast<double>(records.size());
    }

    return records;
}

// ==================== Flashback point-in-time ====================

std::vector<FlashbackRecord> FlashbackGenerator::flashback_point_in_time(
    std::chrono::system_clock::time_point point,
    const catalog::TableSchema& schema) {

    auto saved_config = config_;
    config_.end_time = point;
    config_.mode = FlashbackMode::POINT_IN_TIME;

    auto deletes = flashback_deletes(schema);
    auto updates = flashback_updates(schema);
    auto results = std::move(deletes);
    results.insert(results.end(),
                   std::make_move_iterator(updates.begin()),
                   std::make_move_iterator(updates.end()));

    config_ = saved_config;
    return results;
}

// ==================== Flashback full chain (safe) ====================

FlashbackGenerator::FlashbackChainResult
FlashbackGenerator::flashback_full_chain_safe(
    const std::vector<uint8_t>& current_record,
    uint64_t roll_ptr,
    const catalog::TableSchema& schema,
    uint64_t index_id) {

    FlashbackChainResult result;

    auto history = analyzer_->trace_version_chain(
        current_record, roll_ptr, schema, index_id);

    result.integrity = history.integrity;
    result.diagnostic = history.diagnostic;
    result.exportable = history.export_safe();

    // Only decode records when the chain is safe for export
    if (result.exportable) {
        for (const auto& rec : history.undo_chain) {
            auto fr = decode_single_record(rec, schema);
            result.records.push_back(std::move(fr));
        }
    }

    return result;
}

// ==================== Flashback full chain (deprecated) ====================

std::vector<FlashbackRecord> FlashbackGenerator::flashback_full_chain(
    const std::vector<uint8_t>& current_record,
    uint64_t roll_ptr,
    const catalog::TableSchema& schema,
    uint64_t index_id) {

    auto safe_result = flashback_full_chain_safe(
        current_record, roll_ptr, schema, index_id);

    // Fail-closed: return empty records when chain is not safely exportable
    if (!safe_result.exportable) {
        return {};
    }

    return std::move(safe_result.records);
}

// ==================== Output generation ====================

std::string FlashbackGenerator::generate_output(
    const std::vector<FlashbackRecord>& records,
    const catalog::TableSchema& schema) {

    if (records.empty()) return "";

    switch (config_.output_format) {
        case FlashbackOutputFormat::SQL:
            return FlashbackRecord::to_sql_helper(records, schema);
        case FlashbackOutputFormat::CSV: {
            std::string result;
            // Header
            result += "operation,trx_id,undo_no,";
            for (const auto& [ord, col] : schema.columns) {
                result += col.name + ",";
            }
            result += "completeness,is_complete\n";

            for (const auto& rec : records) {
                result += rec.to_csv(schema);
                result += "\n";
            }
            return result;
        }
        case FlashbackOutputFormat::JSON: {
            std::string result = "[\n";
            for (size_t i = 0; i < records.size(); i++) {
                if (i > 0) result += ",\n";
                result += records[i].to_json(config_.pretty_json);
            }
            result += "\n]\n";
            return result;
        }
    }
    return "";
}

// ==================== Escape helpers (must be before to_sql/to_json) ====================

static std::string escape_sql_string(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        if (c == '\'') result += "''";
        else if (c == '\\') result += "\\\\";
        else result += c;
    }
    return result;
}

static std::string escape_json_string(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

// ==================== SQL value formatting helper ====================

/**
 * @brief Check if a MySQL type ID needs single-quote wrapping in SQL
 */
static bool sql_type_needs_quoting(int type_id) {
    switch (type_id) {
        case mysql_types::TINY:
        case mysql_types::SHORT:
        case mysql_types::LONG:
        case mysql_types::LONGLONG:
        case mysql_types::INT24:
        case mysql_types::FLOAT:
        case mysql_types::DOUBLE:
        case mysql_types::YEAR:
        case mysql_types::BIT:
            return false;
        default:
            return true;
    }
}

/**
 * @brief Look up ColumnInfo by column name from the schema
 */
static const catalog::ColumnInfo* find_column_by_name(
    const catalog::TableSchema& schema, const std::string& name) {
    for (const auto& [ord, col] : schema.columns) {
        if (col.name == name) return &col;
    }
    return nullptr;
}

// ==================== FlashbackRecord::to_sql ====================

std::string FlashbackRecord::to_sql(const catalog::TableSchema& schema) const {
    std::ostringstream sql;

    switch (operation) {
        case DELETE_OP:
            sql << "INSERT INTO " << schema.table_name << " (";
            break;
        case UPDATE:
            sql << "UPDATE " << schema.table_name << " SET ";
            break;
        case INSERT:
            sql << "DELETE FROM " << schema.table_name << " WHERE ";
            break;
    }

    if (operation == DELETE_OP) {
        // INSERT INTO table (col1, col2, ...) VALUES (val1, val2, ...);
        bool first = true;
        for (const auto& f : fields) {
            if (!first) sql << ", ";
            sql << "`" << f.column_name << "`";
            first = false;
        }
        sql << ") VALUES (";
        first = true;
        for (const auto& f : fields) {
            if (!first) sql << ", ";
            if (f.is_null) {
                sql << "NULL";
            } else {
                // Look up column type from schema for type-aware formatting
                const auto* col = find_column_by_name(schema, f.column_name);
                if (col && !sql_type_needs_quoting(static_cast<int>(col->mysql_type_id))) {
                    // Numeric types — unquoted
                    sql << f.human_value;
                } else if (col && (col->mysql_type_id == mysql_types::BLOB ||
                                   col->mysql_type_id == mysql_types::TINY_BLOB ||
                                   col->mysql_type_id == mysql_types::MEDIUM_BLOB ||
                                   col->mysql_type_id == mysql_types::LONG_BLOB ||
                                   col->mysql_type_id == mysql_types::JSON ||
                                   col->mysql_type_id == mysql_types::GEOMETRY ||
                                   col->mysql_type_id == mysql_types::NEWDECIMAL)) {
                    // BLOB/JSON/GEOMETRY — use hex literal
                    sql << "x'" << f.hex_value << "'";
                } else {
                    // String/temporal types — quoted with escaping
                    sql << "'" << escape_sql_string(f.human_value) << "'";
                }
            }
            first = false;
        }
        sql << ");";
        return sql.str();
    }

    // Helper to format a single decoded field as a SQL literal (type-aware).
    auto format_value = [&](const DecodedField& f) -> std::string {
        if (f.is_null) return "NULL";
        const auto* col = find_column_by_name(schema, f.column_name);
        if (col && !sql_type_needs_quoting(static_cast<int>(col->mysql_type_id))) {
            return f.human_value;  // numeric family — unquoted
        }
        if (col && (col->mysql_type_id == mysql_types::BLOB ||
                    col->mysql_type_id == mysql_types::TINY_BLOB ||
                    col->mysql_type_id == mysql_types::MEDIUM_BLOB ||
                    col->mysql_type_id == mysql_types::LONG_BLOB ||
                    col->mysql_type_id == mysql_types::JSON ||
                    col->mysql_type_id == mysql_types::GEOMETRY ||
                    col->mysql_type_id == mysql_types::NEWDECIMAL)) {
            return "x'" + f.hex_value + "'";  // binary — hex literal
        }
        return "'" + escape_sql_string(f.human_value) + "'";  // string/temporal
    };

    // Build a set of primary key column names from the index definitions.
    std::set<std::string> pk_columns;
    for (const auto& idx : schema.indexes) {
        if (idx.is_primary) {
            for (const auto& cname : idx.columns) {
                pk_columns.insert(cname);
            }
            break;
        }
    }

    // Build a WHERE clause from the primary-key columns present in this record.
    // PK values are unchanged by the originating operation, so they uniquely
    // identify the row regardless of operation type.
    std::string where_clause;
    bool first_term = true;
    for (const auto& [ord, col] : schema.columns) {
        if (pk_columns.find(col.name) == pk_columns.end()) continue;
        auto it = std::find_if(fields.begin(), fields.end(),
            [&](const DecodedField& f) { return f.column_name == col.name; });
        if (it != fields.end()) {
            if (!first_term) where_clause += " AND ";
            where_clause += "`" + col.name + "` = " + format_value(*it);
            first_term = false;
        }
    }
    if (first_term) {
        where_clause = "/* no PK columns decoded — manual review required */";
    }

    if (operation == UPDATE) {
        // Undo UPDATE stores the BEFORE image. Reverse by restoring every
        // decoded column to its pre-update value. The switch already emitted
        // "UPDATE table SET ", so append the SET assignments.
        bool first_set = true;
        for (const auto& f : fields) {
            if (!first_set) sql << ", ";
            sql << "`" << f.column_name << "` = " << format_value(f);
            first_set = false;
        }
        sql << " WHERE " << where_clause << ";";
    } else if (operation == INSERT) {
        // Undo INSERT carries only the PK (the row that was inserted). Reverse
        // by deleting that single row. The switch already emitted the WHERE.
        sql << where_clause << ";";
    }

    return sql.str();
}

// ==================== FlashbackRecord::to_csv ====================

std::string FlashbackRecord::to_csv(const catalog::TableSchema& schema, char delimiter) const {
    std::ostringstream csv;

    switch (operation) {
        case DELETE_OP: csv << "DELETE"; break;
        case UPDATE:    csv << "UPDATE"; break;
        case INSERT:    csv << "INSERT"; break;
    }
    csv << delimiter << trx_id << delimiter << undo_no;

    // Column values in schema ordinal order
    for (const auto& [ord, col] : schema.columns) {
        csv << delimiter;
        auto it = field_map.find(col.name);
        if (it != field_map.end()) {
            std::string val = it->second;
            // CSV-escape: wrap in quotes if contains delimiter or quotes
            if (val.find(delimiter) != std::string::npos || val.find('"') != std::string::npos) {
                for (size_t i = 0; i < val.size(); i++) {
                    if (val[i] == '"') val.insert(i++, 1, '"');
                }
                csv << '"' << val << '"';
            } else {
                csv << val;
            }
        }
    }

    csv << delimiter << std::fixed << std::setprecision(4) << completeness;
    csv << delimiter << (is_complete ? "1" : "0");

    return csv.str();
}

// ==================== FlashbackRecord::to_json ====================

std::string FlashbackRecord::to_json(bool pretty) const {
    std::ostringstream json;
    std::string nl = pretty ? "\n" : "";
    std::string ind = pretty ? "  " : "";
    std::string ind2 = pretty ? "    " : "";

    json << ind << "{" << nl;

    json << ind2 << "\"operation\": \"";
    switch (operation) {
        case DELETE_OP: json << "DELETE"; break;
        case UPDATE:    json << "UPDATE"; break;
        case INSERT:    json << "INSERT"; break;
    }
    json << "\"," << nl;

    json << ind2 << "\"trx_id\": " << trx_id << "," << nl;
    json << ind2 << "\"undo_no\": " << undo_no << "," << nl;

    json << ind2 << "\"fields\": {" << nl;
    bool first = true;
    for (const auto& f : fields) {
        if (!first) json << "," << nl;
        json << ind2 << ind2 << "\"" << f.column_name << "\": ";
        if (f.is_null) {
            json << "null";
        } else {
            json << "\"" << escape_json_string(f.human_value) << "\"";
        }
        first = false;
    }
    if (!first) json << nl;
    json << ind2 << "}," << nl;

    json << ind2 << "\"completeness\": " << std::fixed << std::setprecision(4) << completeness << "," << nl;
    json << ind2 << "\"is_complete\": " << (is_complete ? "true" : "false") << nl;

    json << ind << "}";

    return json.str();
}

// ==================== FlashbackRecord::to_sql_helper ====================

std::string FlashbackRecord::to_sql_helper(
    const std::vector<FlashbackRecord>& records,
    const catalog::TableSchema& schema) {

    std::ostringstream sql;
    for (const auto& rec : records) {
        sql << rec.to_sql(schema) << "\n";
    }
    return sql.str();
}

} // namespace innodb
} // namespace dbrecovery