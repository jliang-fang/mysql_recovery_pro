/**
 * @file deep_undo_analyzer.cpp
 * @brief Deep Undo Analysis Engine implementation
 *
 * Implements the 44-step trace_version_chain() algorithm from the
 * architecture v2.0 blueprint. Parses DB_ROLL_PTR, traverses the
 * full undo version chain, and reconstructs row version history.
 */

#include "dbrecovery/innodb/deep_undo_analyzer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/trx_sys_timestamp_index.hpp"

#include <cstring>
#include <cstdio>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <set>
#include <limits>
#include <filesystem>

namespace dbrecovery {
namespace innodb {

// ==================== Undo Log Constants ====================

// Undo log types (from MySQL trx0undo.h)
namespace {
constexpr uint8_t TRX_UNDO_INSERT = 1;
constexpr uint8_t TRX_UNDO_UPDATE = 2;

constexpr uint8_t TRX_UNDO_UPD_EXIST_REC = 11;
constexpr uint8_t TRX_UNDO_UPD_DEL_REC   = 12;
constexpr uint8_t TRX_UNDO_DEL_REC       = 13;
constexpr uint8_t TRX_UNDO_INS_REC       = 14;

// Undo page header offsets
constexpr uint16_t TRX_UNDO_PAGE_START   = 28;
constexpr uint16_t TRX_UNDO_PAGE_FREE    = 30;
constexpr uint16_t TRX_UNDO_PAGE_N_RECS  = 32;
constexpr uint16_t TRX_UNDO_PAGE_HDR_SIZE = 40;

// Rollback segment array in TRX_SYS page
constexpr uint32_t TRX_SYS_RSEG_ARRAY      = 108;
constexpr uint32_t TRX_SYS_RSEG_SLOT_SIZE  = 8;
constexpr uint32_t TRX_SYS_N_RSEGS         = 128;
constexpr uint32_t TRX_SYS_PAGE_NO         = 5;

// Max chain traversal depth
constexpr uint32_t MAX_CHAIN_DEPTH = 1000;

// Sentinel values
// FIL_NULL is defined in page_defs.hpp

// ==================== SQL value formatting helper ====================

/**
 * @brief Format a raw value for SQL output based on column type
 *
 * Produces type-aware SQL literals:
 *   - Numeric types: unquoted (e.g. 42, 3.14)
 *   - String/temporal types: quoted with escaping (e.g. 'hello')
 *   - BLOB/JSON types: hex literal (e.g. x'deadbeef')
 *   - NULL: unquoted NULL
 */
static std::string format_sql_value(const std::vector<uint8_t>& raw_value,
                                     const catalog::ColumnInfo& col) {
    if (raw_value.empty()) return "NULL";

    int type_id = static_cast<int>(col.mysql_type_id);

    // Build hex representation for fallback
    std::string hex;
    for (auto b : raw_value) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", b);
        hex += buf;
    }

    switch (type_id) {
        // Numeric types — unquoted, decode from raw bytes
        case mysql_types::TINY: {
            int8_t v = static_cast<int8_t>(raw_value[0]);
            return std::to_string(static_cast<int>(v));
        }
        case mysql_types::SHORT: {
            if (raw_value.size() < 2) return hex;
            int16_t v = static_cast<int16_t>(
                raw_value[0] | (raw_value[1] << 8));
            return std::to_string(v);
        }
        case mysql_types::LONG: {
            if (raw_value.size() < 4) return hex;
            uint32_t v = 0;
            for (size_t i = 0; i < 4 && i < raw_value.size(); i++)
                v |= static_cast<uint32_t>(raw_value[i]) << (i * 8);
            return std::to_string(static_cast<int32_t>(v));
        }
        case mysql_types::LONGLONG: {
            if (raw_value.size() < 8) return hex;
            uint64_t v = 0;
            for (size_t i = 0; i < 8 && i < raw_value.size(); i++)
                v |= static_cast<uint64_t>(raw_value[i]) << (i * 8);
            return std::to_string(static_cast<int64_t>(v));
        }
        case mysql_types::INT24: {
            if (raw_value.size() < 3) return hex;
            uint32_t v = raw_value[0] | (raw_value[1] << 8) | (raw_value[2] << 16);
            if (v & 0x800000) v |= 0xFF000000;
            return std::to_string(static_cast<int32_t>(v));
        }
        case mysql_types::FLOAT: {
            if (raw_value.size() < 4) return hex;
            float f;
            std::memcpy(&f, raw_value.data(), 4);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.15g", static_cast<double>(f));
            return buf;
        }
        case mysql_types::DOUBLE: {
            if (raw_value.size() < 8) return hex;
            double d;
            std::memcpy(&d, raw_value.data(), 8);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.15g", d);
            return buf;
        }
        case mysql_types::YEAR: {
            uint32_t y = 1900 + raw_value[0];
            return std::to_string(y);
        }
        case mysql_types::BIT: {
            uint64_t val = 0;
            for (size_t i = 0; i < raw_value.size() && i < 8; i++)
                val = (val << 8) | raw_value[i];
            return std::to_string(val);
        }

        // BLOB/JSON/GEOMETRY/DECIMAL — hex literal
        case mysql_types::BLOB:
        case mysql_types::TINY_BLOB:
        case mysql_types::MEDIUM_BLOB:
        case mysql_types::LONG_BLOB:
        case mysql_types::JSON:
        case mysql_types::GEOMETRY:
        case mysql_types::NEWDECIMAL:
        case mysql_types::DECIMAL:
            return "x'" + hex + "'";

        // String/temporal types — quoted with escaping
        default: {
            bool printable = true;
            for (auto b : raw_value) {
                if (b < 0x20 && b != '\t' && b != '\n') {
                    printable = false;
                    break;
                }
            }
            if (printable) {
                std::string s(raw_value.begin(), raw_value.end());
                // Escape single quotes for SQL
                size_t pos = 0;
                while ((pos = s.find('\'', pos)) != std::string::npos) {
                    s.insert(pos, "'");
                    pos += 2;
                }
                return "'" + s + "'";
            }
            // Non-printable string data — fall back to hex
            return "x'" + hex + "'";
        }
    }
}

} // anonymous namespace

// ==================== UndoPageManager Implementation ====================

bool UndoPageManager::ensure_open() {
    if (opened_) return true;
    if (data_file_path_.empty()) return false;

    file_.open(data_file_path_, std::ios::binary | std::ios::ate);
    if (!file_) return false;

    const auto end_pos = file_.tellg();
    if (end_pos < 0) { file_.close(); return false; }

    page_count_ = static_cast<uint64_t>(end_pos) / page_size_;
    if (page_count_ == 0) { file_.close(); return false; }

    opened_ = true;
    file_.seekg(0, std::ios::beg);
    return true;
}

std::vector<uint8_t> UndoPageManager::read_undo_page(uint32_t page_no) {
    // First try the main data file (ibdata1)
    if (ensure_open()) {
        if (static_cast<uint64_t>(page_no) < page_count_) {
            const uint64_t offset = static_cast<uint64_t>(page_no) * page_size_;
            if (offset <= static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
                file_.clear();
                file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

                std::vector<uint8_t> page_data(page_size_);
                file_.read(reinterpret_cast<char*>(page_data.data()),
                           static_cast<std::streamsize>(page_size_));

                if (file_.gcount() == static_cast<std::streamsize>(page_size_)) {
                    return page_data;
                }
            }
        }
    }

    // If not found in main file, try independent undo tablespaces
    return read_from_undo_tablespace(page_no);
}

bool UndoPageManager::ensure_undo_tablespaces_open() {
    if (undo_tablespaces_opened_) return true;
    if (undo_tablespace_paths_.empty()) return false;

    for (size_t i = 0; i < undo_tablespace_paths_.size(); i++) {
        std::ifstream ts_file(undo_tablespace_paths_[i], std::ios::binary | std::ios::ate);
        if (!ts_file) continue;

        const auto end_pos = ts_file.tellg();
        if (end_pos < 0) continue;

        uint64_t ts_pages = static_cast<uint64_t>(end_pos) / page_size_;
        if (ts_pages == 0) continue;

        // Auto-detect page range if not specified
        if (undo_tablespace_ends_[i] == 0) {
            undo_tablespace_ends_[i] = undo_tablespace_starts_[i] + static_cast<uint32_t>(ts_pages);
        }

        undo_tablespace_files_.push_back(std::move(ts_file));
        undo_tablespace_pages_.push_back(ts_pages);
    }

    undo_tablespaces_opened_ = !undo_tablespace_files_.empty();
    return undo_tablespaces_opened_;
}

std::vector<uint8_t> UndoPageManager::read_from_undo_tablespace(uint32_t page_no) {
    if (!ensure_undo_tablespaces_open()) return {};

    // Find which undo tablespace contains this page number
    for (size_t i = 0; i < undo_tablespace_files_.size(); i++) {
        uint32_t start = undo_tablespace_starts_[i];
        uint32_t end = undo_tablespace_ends_[i];

        if (page_no >= start && page_no < end) {
            uint32_t local_page = page_no - start;
            if (local_page >= undo_tablespace_pages_[i]) return {};

            const uint64_t offset = static_cast<uint64_t>(local_page) * page_size_;
            if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
                return {};
            }

            undo_tablespace_files_[i].clear();
            undo_tablespace_files_[i].seekg(static_cast<std::streamoff>(offset), std::ios::beg);

            std::vector<uint8_t> page_data(page_size_);
            undo_tablespace_files_[i].read(reinterpret_cast<char*>(page_data.data()),
                                            static_cast<std::streamsize>(page_size_));

            if (undo_tablespace_files_[i].gcount() == static_cast<std::streamsize>(page_size_)) {
                return page_data;
            }
            break;
        }
    }

    return {};
}

bool UndoPageManager::is_open() const {
    return opened_;
}

uint64_t UndoPageManager::get_page_count() const {
    return page_count_;
}

// ==================== UndoRecordV2Parser Implementation ====================

std::optional<std::pair<std::vector<uint8_t>, size_t>>
UndoRecordV2Parser::read_field(
    const uint8_t* data, size_t data_size, size_t offset) {

    if (offset >= data_size) return std::nullopt;

    // Read field length prefix
    // Format: if byte < 128, it's the length (1-byte prefix)
    //         if byte >= 128, it's 0x80 | (len_high << 8) and next byte is len_low (2-byte prefix)
    const uint8_t prefix = data[offset];
    uint16_t field_size;
    size_t prefix_size;

    if (prefix & 0x80) {
        // 2-byte length prefix (14 bits for length)
        if (offset + 2 > data_size) return std::nullopt;
        field_size = ((static_cast<uint16_t>(prefix & 0x7F)) << 8) | data[offset + 1];
        prefix_size = 2;
    } else {
        // 1-byte length prefix (7 bits for length)
        field_size = prefix;
        prefix_size = 1;
    }

    // Read field data
    if (offset + prefix_size + field_size > data_size) return std::nullopt;

    std::vector<uint8_t> field_data(
        data + offset + prefix_size,
        data + offset + prefix_size + field_size);

    return std::make_pair(std::move(field_data), prefix_size + field_size);
}

uint64_t UndoRecordV2Parser::extract_prev_ptr_from_field(
    const std::vector<uint8_t>& field_data) {

    // Delegate to RollPtrDecoder for unified multi-version support
    return RollPtrDecoder::extract_prev_ptr_from_field(field_data);
}

uint64_t UndoRecordV2Parser::extract_prev_ptr_from_field(
    const std::vector<uint8_t>& field_data,
    UndoFormatProfile profile) {
    return RollPtrDecoder::extract_prev_ptr_from_field(field_data, profile);
}

std::optional<UndoRecordV2> UndoRecordV2Parser::parse(
    const uint8_t* page_data,
    size_t page_size,
    size_t offset,
    const catalog::TableSchema* table_schema,
    size_t* consumed_size) {

    if (!page_data || offset >= page_size || page_size < TRX_UNDO_PAGE_HDR_SIZE) {
        return std::nullopt;
    }

    // Validate that this is an undo page
    uint16_t page_type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);
    if (page_type != FIL_PAGE_UNDO_LOG) {
        return std::nullopt;
    }

    // Read page free offset to validate the record offset
    const uint16_t page_free = read_le<uint16_t>(page_data, TRX_UNDO_PAGE_FREE);
    if (offset >= page_free || offset < TRX_UNDO_PAGE_HDR_SIZE) {
        return std::nullopt;
    }
    const size_t data_size = page_free;

    // Read the undo record header
    if (offset >= data_size || data_size - offset < 2) {
        return std::nullopt;
    }

    const uint8_t type_byte = page_data[offset];
    const uint8_t field_count = page_data[offset + 1];

    // Validate type
    if (type_byte != TRX_UNDO_INSERT &&
        type_byte != TRX_UNDO_UPDATE &&
        type_byte != TRX_UNDO_UPD_EXIST_REC &&
        type_byte != TRX_UNDO_UPD_DEL_REC &&
        type_byte != TRX_UNDO_DEL_REC &&
        type_byte != TRX_UNDO_INS_REC) {
        return std::nullopt;
    }

    if (field_count > 50) {
        return std::nullopt;
    }

    // Build result
    UndoRecordV2 rec;
    rec.undo_page_no = 0; // Will be set by caller
    rec.undo_page_offset = static_cast<uint16_t>(offset);
    rec.undo_no = 0;
    rec.trx_id = 0;

    // Map to our type enum
    switch (type_byte) {
        case TRX_UNDO_INSERT:
        case TRX_UNDO_INS_REC:
            rec.type = UndoRecordV2::INSERT;
            break;
        case TRX_UNDO_UPDATE:
        case TRX_UNDO_UPD_EXIST_REC:
            rec.type = UndoRecordV2::UPDATE;
            break;
        case TRX_UNDO_DEL_REC:
            rec.type = UndoRecordV2::DELETE_MARK;
            break;
        case TRX_UNDO_UPD_DEL_REC:
            // This is an "undelete" — record was deleted then updated
            rec.type = UndoRecordV2::UPDATE;
            break;
        default:
            rec.type = UndoRecordV2::UPDATE;
            break;
    }

    // Parse header fields based on record type
    size_t header_size = 2; // minimum: type + field_count

    if (rec.type == UndoRecordV2::INSERT) {
        // INSERT undo records have only type + field_count as header
        // Page_no/offset are not stored (INSERT creates a new record)
        rec.page_no = 0;
        rec.page_offset = 0;
        rec.info_bits = 0;
        // No prev_undo_ptr for INSERT (this is the oldest version)
        rec.prev_undo_ptr = 0;
    } else {
        // UPDATE/DELETE records have extended header
        // MySQL 5.6/5.7 format:
        //   offset+0: type_cmpl (1 byte)
        //   offset+1: field_count (1 byte)
        //   offset+2-5: rec_no (4 bytes, page_no approximation)
        //   offset+6-7: page_offset (2 bytes)
        //
        // MySQL 8.0 format:
        //   offset+0: type_cmpl (1 byte)
        //   offset+1: field_count (1 byte)
        //   offset+2-5: page_no (4 bytes)
        //   offset+6-7: page_offset (2 bytes)
        //   offset+8: info_bits (1 byte)
        //   offset+9-13: trx_id (5 bytes)
        //   offset+14-19: old_roll_ptr (6 bytes, MySQL 5.6/5.7)
        //   offset+14-20: old_roll_ptr (7 bytes, MySQL 8.0+)
        //   offset+20/21+: field data

        if (data_size - offset < 8) {
            return std::nullopt;
        }

        // page_no (4 bytes) at offset+2, page_offset (2 bytes) at offset+6
        rec.page_no = read_le<uint32_t>(page_data, offset + 2);
        rec.page_offset = read_le<uint16_t>(page_data, offset + 6);

        // Try to read info_bits, trx_id, and old_roll_ptr
        // Only if the data is large enough
        static constexpr size_t MIN_UPDATE_HEADER = 14; // 2 + 4 + 2 + 1 + 5
        static constexpr size_t ROLL_PTR_OFFSET = 14;   // offset for old_roll_ptr (both 5.6/5.7 and 8.0+)
        static constexpr size_t ROLL_PTR_LEN_56 = 6;    // 6-byte roll_ptr in 5.6/5.7
        static constexpr size_t ROLL_PTR_LEN_80 = 7;    // 7-byte roll_ptr in 8.0+

        // Read trx_id (5 bytes at offset+9)
        if (data_size - offset >= 14) {
            rec.trx_id = read_le<uint32_t>(page_data, offset + 9);
            rec.trx_id |= static_cast<uint64_t>(page_data[offset + 13]) << 32;
        }

        // Read prev_undo_ptr using the resolved undo format profile.
        // The profile is set externally from MySQL version evidence — never
        // inferred from remaining bytes. UNRESOLVED (strict mode) rejects
        // the record because the roll-pointer width is untrusted.
        const auto profile = undo_format_profile_;
        if (profile == UndoFormatProfile::UNRESOLVED) {
            // Fail-closed: no resolved format evidence → refuse to guess.
            return std::nullopt;
        }

        const bool is_8byte_profile = (profile == UndoFormatProfile::MYSQL8_7BYTE);
        const size_t roll_ptr_len = is_8byte_profile ? ROLL_PTR_LEN_80 : ROLL_PTR_LEN_56;
        if (data_size - offset >= ROLL_PTR_OFFSET + roll_ptr_len) {
            header_size = ROLL_PTR_OFFSET + roll_ptr_len;
            rec.prev_undo_ptr = RollPtrDecoder::decode_storage_to_internal(
                page_data + offset + ROLL_PTR_OFFSET, roll_ptr_len, profile);
        } else {
            // Data too short for the selected profile — the record header is
            // truncated. Return nullopt (fail-closed) rather than returning a
            // record with prev_undo_ptr=0 that the caller interprets as the
            // end of the version chain. Silent chain truncation would cause
            // the recovery to produce a logically incomplete result without
            // any diagnostic signal.
            return std::nullopt;
        }
    }

    // Parse field data
    size_t field_offset = offset + header_size;
    if (field_offset >= data_size) {
        if (field_count != 0) return std::nullopt;
        if (consumed_size) {
            *consumed_size = header_size;
        }
        return rec;
    }

    // Read all fields from the undo record. A partial record is not a valid
    // historical version and must never be admitted to structured recovery.
    uint8_t fields_read = 0;
    while (fields_read < field_count) {
        auto field_opt = read_field(page_data, data_size, field_offset);
        if (!field_opt) return std::nullopt;

        auto& [field_data, bytes_consumed] = *field_opt;
        field_offset += bytes_consumed;

        UndoFieldValue fv;
        fv.field_no = fields_read + 1;
        fv.old_value = std::move(field_data);
        fv.is_null = false;
        fv.is_extern = false;

        rec.old_fields.push_back(std::move(fv));
        fields_read++;
    }

    // Compute total consumed bytes for the caller to advance to the next record
    if (consumed_size) {
        *consumed_size = (field_offset - offset); // header_size + sum of all field data
    }

    return rec;
}

// ==================== DeepUndoAnalyzer Implementation ====================

DeepUndoAnalyzer::DeepUndoAnalyzer(size_t page_size)
    : page_size_(page_size), page_manager_(page_size) {}

std::optional<UndoRecordV2> DeepUndoAnalyzer::parse_verified_undo_record(
    const std::vector<uint8_t>& page_data,
    uint32_t undo_page_no,
    size_t offset,
    const catalog::TableSchema& schema,
    size_t* consumed_size) {
    if (undo_page_no == 0 || undo_page_no == FIL_NULL ||
        page_data.size() < FIL_HEADER_SIZE || offset > UINT16_MAX) {
        return std::nullopt;
    }

    if (read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS) != FIL_PAGE_UNDO_LOG) {
        return std::nullopt;
    }

    const uint16_t page_free = read_le<uint16_t>(page_data.data(), TRX_UNDO_PAGE_FREE);
    if (offset < TRX_UNDO_PAGE_HDR_SIZE || offset >= page_free || page_free > page_data.size()) {
        return std::nullopt;
    }

    size_t parsed_size = 0;
    auto record = record_parser_.parse(
        page_data.data(), page_data.size(), offset, &schema, &parsed_size);
    if (!record || parsed_size == 0 || parsed_size > page_free - offset) {
        return std::nullopt;
    }

    const uint32_t physical_space_id =
        read_be<uint32_t>(page_data.data(), FIL_PAGE_SPACE_ID);
    if (target_tablespace_id_ != 0 && physical_space_id != target_tablespace_id_) {
        return std::nullopt;
    }

    record->undo_page_no = undo_page_no;
    record->undo_page_offset = static_cast<uint16_t>(offset);
    record->tablespace_id = physical_space_id;
    record->has_valid_provenance = true;
    if (consumed_size) {
        *consumed_size = parsed_size;
    }
    return record;
}

void DeepUndoAnalyzer::enrich_transaction_metadata(UndoRecordV2& record) {
    if (record.trx_id == 0 || ibdata_path_.empty()) {
        return;
    }

    if (!timestamp_index_.is_built()) {
        timestamp_index_.build(ibdata_path_, page_size_);
    }

    const auto entry = timestamp_index_.lookup(record.trx_id);
    if (!entry || !entry->is_valid || entry->commit_lsn == 0) {
        return;
    }

    record.commit_lsn = entry->commit_lsn;
    record.approx_timestamp = std::chrono::system_clock::time_point(
        std::chrono::nanoseconds(entry->commit_lsn));
}

RowVersionHistory DeepUndoAnalyzer::trace_version_chain(
    const std::vector<uint8_t>& current_record,
    uint64_t roll_ptr,
    const catalog::TableSchema& schema,
    uint64_t index_id) {

    // ===== Algorithm 2.4 from architecture v2.0 blueprint =====
    //
    // 1. history ← RowVersionHistory()
    // 2. history.current_data ← current_record
    // 3. current_roll_ptr ← roll_ptr
    // 4. visited ← HashSet<uint64_t>()

    RowVersionHistory history;
    history.current_data = current_record;
    history.current_is_deleted = false;
    history.index_id = index_id;
    history.integrity = HistoryIntegrity::COMPLETE;
    // Extract primary key from current_record if available
    // (will be filled in from the undo chain if not)

    uint64_t current_roll_ptr = roll_ptr;
    std::set<uint64_t> visited;  // Cycle detection

    // 5. WHILE current_roll_ptr != 0 AND current_roll_ptr ∉ visited:
    while (current_roll_ptr != 0) {
        if (visited.find(current_roll_ptr) != visited.end()) {
            stats_.broken_chains++;
            history.integrity = HistoryIntegrity::CYCLE;
            history.diagnostic =
                "cycle detected at roll_ptr " + std::to_string(current_roll_ptr);
            break;
        }

        // 6. visited.insert(current_roll_ptr)
        visited.insert(current_roll_ptr);

        // 7. Parse roll_ptr: [31:0] undo_page_no | [49:32] undo_offset (18 bits) | [55:50] flags (6 bits)
        auto [undo_page_no, undo_offset] = parse_roll_ptr(current_roll_ptr);

        // 8. Read undo page and validate provenance
        //    Fail-closed: if the page cannot be read, the chain is broken.
        //    We do NOT emit partial results because the version between the
        //    current record and the next readable record is untrustworthy.
        if (undo_page_no == 0 || undo_page_no == FIL_NULL) {
            stats_.broken_chains++;
            history.integrity = HistoryIntegrity::BROKEN_PAGE;
            history.diagnostic =
                "roll_ptr " + std::to_string(current_roll_ptr) + " has no page provenance";
            break;  // No provenance — cannot trust the chain
        }
        auto undo_page = page_manager_.read_undo_page(undo_page_no);
        if (undo_page.empty()) {
            stats_.broken_chains++;
            history.integrity = HistoryIntegrity::BROKEN_PAGE;
            history.diagnostic =
                "undo page " + std::to_string(undo_page_no) + " unreadable";
            break;  // Page not readable → chain break
        }

        // 9. Parse undo record only after physical provenance is established.
        auto undo_rec_opt = parse_verified_undo_record(
            undo_page, undo_page_no, undo_offset, schema);
        if (!undo_rec_opt) {
            stats_.broken_chains++;
            history.integrity = HistoryIntegrity::INVALID_RECORD;
            history.diagnostic =
                "undo record at page " + std::to_string(undo_page_no) +
                " offset " + std::to_string(undo_offset) + " failed verification";
            break;
        }

        auto& undo_rec = *undo_rec_opt;
        undo_rec.roll_ptr = current_roll_ptr;

        // 10. Extract old field values based on undo record type
        if (undo_rec.type == UndoRecordV2::UPDATE) {
            // UPDATE type: extract modified field values
            // The first field in the undo record is typically the old DB_ROLL_PTR
            // Subsequent fields are old values of modified columns
            // We store them all as old_fields for the caller to interpret
        } else if (undo_rec.type == UndoRecordV2::INSERT) {
            // INSERT type: record primary key, mark as insertion version
            // Extract PK from the undo record's field data
            if (!undo_rec.old_fields.empty()) {
                for (const auto& fv : undo_rec.old_fields) {
                    undo_rec.primary_key_raw.insert(
                        undo_rec.primary_key_raw.end(),
                        fv.old_value.begin(),
                        fv.old_value.end());
                }
                history.primary_key = undo_rec.primary_key_raw;
            }
        }

        // 11. Push to undo chain
        history.undo_chain.push_back(std::move(undo_rec));

        // 12. Move to previous version
        auto& last_rec = history.undo_chain.back();
        const uint64_t header_prev_ptr = last_rec.prev_undo_ptr;
        const uint64_t field_prev_ptr = last_rec.old_fields.empty()
            ? 0
            : UndoRecordV2Parser::extract_prev_ptr_from_field(
                  last_rec.old_fields.front().old_value,
                  undo_format_profile_);

        if (header_prev_ptr != 0 &&
            field_prev_ptr != 0 &&
            header_prev_ptr != field_prev_ptr) {
            stats_.broken_chains++;
            if (history.integrity == HistoryIntegrity::COMPLETE) {
                history.integrity = HistoryIntegrity::POINTER_CONFLICT;
                history.diagnostic =
                    "header prev_undo_ptr (" + std::to_string(header_prev_ptr) +
                    ") != first-field prev pointer (" +
                    std::to_string(field_prev_ptr) + ") at undo page " +
                    std::to_string(last_rec.undo_page_no) + " offset " +
                    std::to_string(last_rec.undo_page_offset);
            }
            break;
        }

        current_roll_ptr = header_prev_ptr;

        // 13. If reached INSERT version, stop
        if (last_rec.type == UndoRecordV2::INSERT) {
            current_roll_ptr = 0;  // Force termination
        }

        // 14. Safety limit: max 1000 versions
        if (history.undo_chain.size() >= max_chain_depth_) {
            history.truncated = true;
            if (history.integrity == HistoryIntegrity::COMPLETE) {
                history.integrity = HistoryIntegrity::TRUNCATED;
                history.diagnostic =
                    "undo chain exceeded max depth " +
                    std::to_string(max_chain_depth_) + " at undo page " +
                    std::to_string(last_rec.undo_page_no) + " offset " +
                    std::to_string(last_rec.undo_page_offset);
            }
            break;
        }

        stats_.total_undo_records++;
    }

    // 15. Order oldest-first (INSERT first, newest undo last)
    std::reverse(history.undo_chain.begin(), history.undo_chain.end());

    // 15a. Populate transaction metadata through the analyzer-owned index.
    for (auto& rec : history.undo_chain) {
        enrich_transaction_metadata(rec);
    }

    // 16. Set metadata
    history.n_versions = 1 + static_cast<uint32_t>(history.undo_chain.size());

    // If PK wasn't set from INSERT record, try to extract from the chain
    if (history.primary_key.empty()) {
        for (const auto& rec : history.undo_chain) {
            if (!rec.primary_key_raw.empty()) {
                history.primary_key = rec.primary_key_raw;
                break;
            }
        }
    }

    stats_.chains_traced++;
    return history;
}

std::vector<std::pair<std::vector<uint8_t>, uint32_t>>
DeepUndoAnalyzer::scan_undo_pages() {

    // Ensure index is available (lazy-build or use external)
    ensure_index_built();

    // Use the index for O(1) lookup
    const auto* idx = undo_page_index_ ? undo_page_index_ : &internal_index_;
    if (!idx->is_built() || idx->undo_pages().empty()) {
        return {};
    }

    // Open ibdata1
    if (!page_manager_.is_open()) {
        std::vector<uint8_t> test_page = page_manager_.read_undo_page(0);
        if (test_page.empty()) return {};
    }

    std::vector<std::pair<std::vector<uint8_t>, uint32_t>> undo_pages;

    for (uint32_t pg : idx->undo_pages()) {
        auto page_data = page_manager_.read_undo_page(pg);
        if (page_data.empty()) continue;
        undo_pages.push_back({std::move(page_data), pg});
        stats_.pages_scanned++;
    }

    return undo_pages;
}

void DeepUndoAnalyzer::ensure_index_built() {
    // If an external index is set, use it — no lazy build needed
    if (undo_page_index_) return;
    // If already built, skip
    if (index_built_) return;

    // Build the internal index and only mark it ready after a successful scan.
    index_built_ = internal_index_.build(ibdata_path_, page_size_, 0);
}

uint32_t DeepUndoAnalyzer::extract_table_id(
    const std::vector<UndoFieldValue>& fields) {

    // Table ID is typically stored in the first few bytes of undo record fields
    if (fields.empty()) return 0;

    const auto& first_field = fields[0];
    if (first_field.old_value.size() >= 4) {
        return read_le<uint32_t>(first_field.old_value.data(), 0);
    }

    return 0;
}

std::vector<UndoRecordV2> DeepUndoAnalyzer::scan_records(
    const catalog::TableSchema& schema,
    const FlashbackFilter& filter,
    std::initializer_list<UndoRecordV2::Type> types) {

    std::vector<UndoRecordV2> records;
    auto undo_pages = scan_undo_pages();
    if (undo_pages.empty()) return records;

    for (const auto& [page_data, page_no] : undo_pages) {
        const uint16_t page_free = read_le<uint16_t>(page_data.data(), TRX_UNDO_PAGE_FREE);
        const uint16_t n_recs = read_le<uint16_t>(page_data.data(), TRX_UNDO_PAGE_N_RECS);

        size_t offset = read_le<uint16_t>(page_data.data(), TRX_UNDO_PAGE_START);
        for (uint16_t ri = 0; ri < n_recs && offset < page_free; ri++) {
            size_t consumed = 0;
            auto rec_opt = parse_verified_undo_record(
                page_data, page_no, offset, schema, &consumed);
            if (!rec_opt) break;

            auto& rec = *rec_opt;
            enrich_transaction_metadata(rec);

            // Filter by type (empty = keep all)
            if (types.size() > 0) {
                bool type_match = false;
                for (auto t : types) {
                    if (rec.type == t) { type_match = true; break; }
                }
                if (!type_match) {
                    offset += consumed;
                    continue;
                }
            }

            // Filter by table_id
            uint32_t rec_table_id = extract_table_id(rec.old_fields);
            if (rec_table_id != static_cast<uint32_t>(schema.table_id) && schema.table_id != 0) {
                offset += consumed;
                continue;
            }

            // Filter by trx_id
            if (filter.trx_id.has_value() && rec.trx_id != *filter.trx_id) {
                offset += consumed;
                continue;
            }

            // Fail-closed LSN range: when an LSN filter is given, a record
            // without resolved commit_lsn cannot be proven in-range and is
            // rejected rather than admitted by default.
            if (filter.start_lsn.has_value() || filter.end_lsn.has_value()) {
                if (!rec.commit_lsn.has_value()) {
                    offset += consumed;
                    continue;
                }
                if (filter.start_lsn.has_value() && *rec.commit_lsn < *filter.start_lsn) {
                    offset += consumed;
                    continue;
                }
                if (filter.end_lsn.has_value() && *rec.commit_lsn > *filter.end_lsn) {
                    offset += consumed;
                    continue;
                }
            }

            // Fail-closed time range: same policy as LSN — missing metadata
            // under an active time filter rejects the record.
            if (filter.start_time.has_value() || filter.end_time.has_value()) {
                if (!rec.approx_timestamp.has_value()) {
                    offset += consumed;
                    continue;
                }
                if (filter.start_time.has_value() &&
                    *rec.approx_timestamp < *filter.start_time) {
                    offset += consumed;
                    continue;
                }
                if (filter.end_time.has_value() &&
                    *rec.approx_timestamp > *filter.end_time) {
                    offset += consumed;
                    continue;
                }
            }

            records.push_back(std::move(rec));

            if (filter.max_records > 0 && records.size() >= filter.max_records) {
                return records;
            }

            offset += consumed;
        }
        if (filter.max_records > 0 && records.size() >= filter.max_records) break;
    }

    return records;
}

std::string DeepUndoAnalyzer::pk_to_hex(const std::vector<uint8_t>& pk_raw) {
    if (pk_raw.empty()) return "";
    std::ostringstream oss;
    for (auto b : pk_raw) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

std::map<std::string, RowVersionHistory> DeepUndoAnalyzer::rebuild_all_histories(
    uint32_t space_id,
    const catalog::TableSchema& schema,
    uint64_t index_id,
    std::optional<std::pair<uint64_t, uint64_t>> lsn_range) {

    std::map<std::string, RowVersionHistory> histories;
    auto start_time = std::chrono::high_resolution_clock::now();

    FlashbackFilter filter;
    filter.max_records = 0;
    filter.include_incomplete = true;
    if (lsn_range.has_value()) {
        filter.start_lsn = lsn_range->first;
        filter.end_lsn = lsn_range->second;
    }
    auto records = scan_records(schema, filter);

    struct UndoRef {
        uint32_t page_no;
        uint16_t offset;
        UndoRecordV2 record;
    };
    std::map<std::string, std::vector<UndoRef>> pk_groups;

    for (auto& rec : records) {
        if (space_id != 0 && rec.tablespace_id != space_id) continue;
        std::string pk;
        if (!rec.old_fields.empty()) {
            for (size_t i = 1; i < rec.old_fields.size() && i < 4; i++) {
                pk += pk_to_hex(rec.old_fields[i].old_value);
            }
        }
        if (pk.empty()) {
            pk = "pk_" + std::to_string(rec.undo_page_no) + "_" + std::to_string(rec.undo_page_offset);
        }
        uint32_t pg = rec.undo_page_no;
        uint16_t off = rec.undo_page_offset;
        pk_groups[pk].push_back({pg, off, std::move(rec)});
    }

    if (pk_groups.empty()) return histories;

    // Step 3: Build version histories for each PK group
    for (auto& [pk, refs] : pk_groups) {
        RowVersionHistory history;
        history.integrity = HistoryIntegrity::HEURISTIC;
        history.index_id = index_id;
        history.primary_key.clear();
        for (auto& ref : refs) {
            history.primary_key.insert(
                history.primary_key.end(),
                ref.record.primary_key_raw.begin(),
                ref.record.primary_key_raw.end());
        }

        // Sort by undo_no (insert first, then updates, then deletes)
        std::sort(refs.begin(), refs.end(),
            [](const UndoRef& a, const UndoRef& b) {
                int type_a = (a.record.type == UndoRecordV2::INSERT) ? 0
                           : (a.record.type == UndoRecordV2::DELETE_MARK) ? 2
                           : 1;
                int type_b = (b.record.type == UndoRecordV2::INSERT) ? 0
                           : (b.record.type == UndoRecordV2::DELETE_MARK) ? 2
                           : 1;
                if (type_a != type_b) return type_a < type_b;
                return a.record.undo_page_no < b.record.undo_page_no;
            });

        for (auto& ref : refs) {
            history.undo_chain.push_back(std::move(ref.record));
        }

        history.n_versions = static_cast<uint32_t>(history.undo_chain.size());
        histories[pk] = std::move(history);
        stats_.chains_traced++;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.analysis_time_ms = std::chrono::duration<double, std::milli>(
        end_time - start_time).count();

    return histories;
}

std::vector<RowVersionHistory> DeepUndoAnalyzer::heuristic_reconstruct_chains(
    uint32_t space_id,
    const catalog::TableSchema& schema) {

    std::vector<RowVersionHistory> chains;
    auto start_time = std::chrono::high_resolution_clock::now();

    FlashbackFilter filter;
    filter.max_records = 0;
    filter.include_incomplete = true;
    auto scanned = scan_records(schema, filter);

    struct TaggedRecord {
        UndoRecordV2 record;
        uint32_t page_no;
    };
    std::vector<TaggedRecord> all_records;
    all_records.reserve(scanned.size());
    for (auto& rec : scanned) {
        if (space_id != 0 && rec.tablespace_id != space_id) continue;
        uint32_t pg = rec.undo_page_no;
        all_records.push_back({std::move(rec), pg});
    }

    if (all_records.empty()) return chains;

    // Step 2: Group by PK prefix
    std::map<std::string, std::vector<TaggedRecord>> groups;
    for (auto& tr : all_records) {
        std::string pk;
        // Skip first field (old DB_ROLL_PTR), use next 2 fields for PK
        for (size_t i = 1; i < tr.record.old_fields.size() && i < 3; i++) {
            pk += pk_to_hex(tr.record.old_fields[i].old_value);
            if (i == 1 && tr.record.old_fields.size() > 2) pk += "_";
        }
        if (pk.empty()) {
            pk = "orphan_" + std::to_string(tr.page_no) + "_" +
                 std::to_string(tr.record.undo_page_offset);
        }
        groups[pk].push_back(std::move(tr));
    }

    // Step 3: Build chains for each group
    for (auto& [pk, entries] : groups) {
        if (entries.empty()) continue;

        RowVersionHistory history;
        history.integrity = HistoryIntegrity::HEURISTIC;
        history.index_id = 0;
        history.primary_key.clear();

        // Sort by type: INSERT first, then UPDATE, then DELETE
        std::sort(entries.begin(), entries.end(),
            [](const TaggedRecord& a, const TaggedRecord& b) {
                int type_a = (a.record.type == UndoRecordV2::INSERT) ? 0
                           : (a.record.type == UndoRecordV2::DELETE_MARK) ? 2
                           : 1;
                int type_b = (b.record.type == UndoRecordV2::INSERT) ? 0
                           : (b.record.type == UndoRecordV2::DELETE_MARK) ? 2
                           : 1;
                if (type_a != type_b) return type_a < type_b;
                return a.record.undo_page_no < b.record.undo_page_no;
            });

        for (auto& tr : entries) {
            history.undo_chain.push_back(std::move(tr.record));
        }

        history.n_versions = static_cast<uint32_t>(history.undo_chain.size());
        chains.push_back(std::move(history));
        stats_.chains_traced++;
    }

    // Sort by chain length (most versions first = most reliable)
    std::sort(chains.begin(), chains.end(),
        [](const RowVersionHistory& a, const RowVersionHistory& b) {
            return a.n_versions > b.n_versions;
        });

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.analysis_time_ms += std::chrono::duration<double, std::milli>(
        end_time - start_time).count();

    return chains;
}

// ==================== RowVersionHistory Methods ====================

std::optional<std::vector<uint8_t>> RowVersionHistory::get_version_at(
    std::chrono::system_clock::time_point point) const {

    // Walk the undo chain oldest-first, finding the newest version at/before point
    // Since we don't have precise timestamps for each version, this is approximate

    // If no undo chain, return current data (if it's not deleted)
    if (undo_chain.empty()) {
        if (!current_is_deleted) return current_data;
        return std::nullopt;
    }

    // The newest version in the chain is the one just before the current state
    // Walk backwards to find the version closest to the target time
    // Without precise per-version timestamps, we return the first version
    // that has an approx_timestamp <= point

    // Try the newest undo record first (last in oldest-first ordering)
    for (auto it = undo_chain.rbegin(); it != undo_chain.rend(); ++it) {
        if (it->approx_timestamp.has_value() && *it->approx_timestamp <= point) {
            // Return the old field values as the row state at that time
            std::vector<uint8_t> version_data;
            for (const auto& fv : it->old_fields) {
                version_data.insert(version_data.end(),
                    fv.old_value.begin(), fv.old_value.end());
            }
            if (!version_data.empty()) return version_data;
        }
    }

    // Fallback: return current data
    if (!current_is_deleted) return current_data;
    return std::nullopt;
}

std::string RowVersionHistory::to_insert_sql(const catalog::TableSchema& schema) const {
    if (!export_safe()) return "";

    // Build INSERT statement from the latest non-deleted version
    // If current_is_deleted, use the newest undo record's data
    // If not deleted, we can't reconstruct full row from just the undo chain

    std::ostringstream sql;
    sql << "INSERT INTO " << schema.table_name << " (";

    // Column names
    bool first = true;
    for (const auto& [ord, col] : schema.columns) {
        if (!first) sql << ", ";
        sql << "`" << col.name << "`";
        first = false;
    }
    sql << ") VALUES (";

    // Use the newest undo record's fields for values
    if (!undo_chain.empty()) {
        const auto& newest = undo_chain.back(); // oldest-first, so back() is newest
        first = true;
        for (const auto& [ord, col] : schema.columns) {
            if (!first) sql << ", ";
            first = false;

            // Match by field_no: field_no=1 is DB_ROLL_PTR (skip), field_no=2+ maps to ord+1
            bool found = false;
            for (const auto& fv : newest.old_fields) {
                if (fv.field_no == static_cast<uint16_t>(ord + 1)) {
                    if (fv.is_null) {
                        sql << "NULL";
                    } else {
                        sql << format_sql_value(fv.old_value, col);
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                sql << "DEFAULT";
            }
        }
    } else {
        // No undo data — use DEFAULT for all columns
        first = true;
        for (size_t i = 0; i < schema.columns.size(); i++) {
            if (!first) sql << ", ";
            sql << "DEFAULT";
            first = false;
        }
    }

    sql << ");";
    return sql.str();
}

} // namespace innodb
} // namespace dbrecovery