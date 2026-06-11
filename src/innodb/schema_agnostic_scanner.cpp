/**
 * @file schema_agnostic_scanner.cpp
 * @brief Schema-Agnostic InnoDB Record Scanner Implementation
 *
 * This module implements a universal record scanner that:
 * 1. Uses Page Directory for precise record boundaries
 * 2. Dynamically parses Null Bitmap and variable-length sizes
 * 3. Works without prior schema knowledge
 * 4. Handles corrupted records gracefully
 */

#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

#include <fstream>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace dbrecovery {
namespace innodb {

// ==================== Constants ====================

// Use constants from page_defs.hpp
// FIL_HEADER_SIZE is defined there as uint16_t

// Additional constants for this module
constexpr size_t INFIMUM_OFFSET = 99;     // Infimum record origin
constexpr size_t SUPREMUM_OFFSET = 112;   // Approximate supremum position
constexpr size_t PAGE_HEADER_SIZE = 56;   // After FIL header

// System column sizes for leaf pages
constexpr size_t DB_TRX_ID_SIZE = 6;
constexpr size_t DB_ROLL_PTR_SIZE = 7;
constexpr size_t DB_ROW_ID_SIZE = 6;
constexpr size_t SYSTEM_COLS_SIZE = DB_TRX_ID_SIZE + DB_ROLL_PTR_SIZE;  // 13 bytes

// Record header sizes
constexpr size_t REC_HEADER_COMPACT = 5;
constexpr size_t REC_HEADER_REDUNDANT = 6;

// Off-page storage flags
constexpr uint32_t OFF_PAGE_FLAG_DYNAMIC = 16384;   // 0x4000
constexpr uint32_t OFF_PAGE_FLAG_ANTLELOPE = 17172; // 0x4314

// ==================== Helper Functions ====================

namespace {

/**
 * @brief Read big-endian value
 */
template<typename T>
T read_be_val(const uint8_t* data) {
    T val = 0;
    for (size_t i = 0; i < sizeof(T); i++) {
        val = (val << 8) | data[i];
    }
    return val;
}

/**
 * @brief Check if bytes look like a printable string
 */
bool looks_like_string(const uint8_t* data, size_t size) {
    if (size == 0) return false;
    size_t printable = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t c = data[i];
        if (c >= 32 && c < 127) printable++;
    }
    return printable >= size * 0.6;
}

/**
 * @brief Check if value could be a Unix timestamp
 */
bool looks_like_timestamp(uint32_t val) {
    // 2000-01-01 to 2100-01-01
    return val >= 946684800 && val <= 4102444800;
}

/**
 * @brief Format timestamp as string
 */
std::string format_timestamp(uint32_t ts) {
    time_t t = static_cast<time_t>(ts);
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

} // anonymous namespace

// ==================== SchemalessField Implementation ====================

std::string SchemalessField::to_string() const {
    if (is_null) return "NULL";

    switch (type) {
        case Type::INT_VAL:
            return std::to_string(int_value);
        case Type::UINT_VAL:
            return std::to_string(static_cast<uint64_t>(int_value));
        case Type::FLOAT_VAL:
        case Type::DOUBLE_VAL:
            return std::to_string(float_value);
        case Type::STRING_VAL:
        case Type::TIMESTAMP_VAL:
        case Type::DATETIME_VAL:
        case Type::DATE_VAL:
            return str_value;
        case Type::OVERFLOW_PTR:
            return "<OFFPAGE space=" + std::to_string(overflow_space_id) +
                   " page=" + std::to_string(overflow_page_no) +
                   " size=" + std::to_string(overflow_size) + ">";
        case Type::BINARY_VAL: {
            std::string hex = "0x";
            for (uint8_t b : raw_bytes) {
                char buf[3];
                snprintf(buf, sizeof(buf), "%02x", b);
                hex += buf;
            }
            return hex;
        }
        default:
            return "<UNKNOWN>";
    }
}

std::string SchemalessField::to_csv() const {
    if (is_null) return "NULL";

    std::string val = to_string();

    // Escape CSV special characters
    if (val.find(',') != std::string::npos ||
        val.find('"') != std::string::npos ||
        val.find('\n') != std::string::npos) {
        std::string escaped;
        for (char c : val) {
            if (c == '"') escaped += "\"\"";
            else escaped += c;
        }
        return "\"" + escaped + "\"";
    }

    return val;
}

// ==================== SchemalessRecord Implementation ====================

std::string SchemalessRecord::to_csv() const {
    std::ostringstream oss;
    oss << record_id;
    for (const auto& field : fields) {
        oss << "," << field.to_csv();
    }
    return oss.str();
}

// ==================== VarSizeArrayInfo Implementation ====================

size_t VarSizeArrayInfo::total_data_size() const {
    size_t total = 0;
    for (uint32_t s : sizes) {
        if (s < OFF_PAGE_FLAG_DYNAMIC) {
            total += s;
        } else if (s == OFF_PAGE_FLAG_DYNAMIC || s == OFF_PAGE_FLAG_ANTLELOPE) {
            total += 20;  // Overflow pointer size
        }
    }
    return total;
}

// ==================== PageDirSlot Implementation ====================

std::optional<PageDirSlot> PageDirSlot::parse(const uint8_t* page_data, size_t page_size,
                                               size_t slot_index, uint16_t n_dir_slots) {
    // Page directory is at the end of the page
    // Slot 0 is at the highest address (page_size - 2)
    // Slots are stored in reverse order

    size_t slot_offset = page_size - 2 - (slot_index * 2);

    if (slot_offset < FIL_HEADER_SIZE + PAGE_HEADER_SIZE || slot_offset >= page_size) {
        return std::nullopt;
    }

    PageDirSlot slot;
    slot.offset = read_be_val<uint16_t>(page_data + slot_offset);

    // Validate offset is within page and in user record area
    if (slot.offset < INFIMUM_OFFSET || slot.offset >= page_size - 2) {
        return std::nullopt;
    }

    // n_owned is read from the record at this offset (in its header)
    // We'll read it separately when processing records

    return slot;
}

// ==================== DynamicRecordHeader Implementation ====================

std::optional<DynamicRecordHeader> DynamicRecordHeader::parse_compact(const uint8_t* data, size_t max_size) {
    if (!data || max_size < REC_HEADER_COMPACT) return std::nullopt;

    DynamicRecordHeader hdr;
    hdr.header_size = REC_HEADER_COMPACT;

    // Parse 5-byte COMPACT header
    // Byte 0-2: info_and_heap (24 bits, big-endian)
    uint32_t info_heap = (static_cast<uint32_t>(data[0]) << 16) |
                         (static_cast<uint32_t>(data[1]) << 8) |
                         data[2];

    // Extract fields from 24-bit value
    hdr.is_deleted = (info_heap & 0x200000) != 0;  // bit 21
    hdr.is_min_rec  = (info_heap & 0x100000) != 0;  // bit 20
    hdr.n_owned     = (info_heap >> 16) & 0x0F;     // bits 19-16
    hdr.heap_no     = (info_heap >> 3) & 0x1FFF;    // bits 15-3 (13 bits)
    hdr.rec_type    = info_heap & 0x07;             // bits 2-0

    // Bytes 3-4: next_record_offset (signed 16-bit)
    // Note: This offset is relative to the "origin" of the record
    // which is the start of data, NOT the header!
    hdr.next_offset = static_cast<int16_t>(
        (static_cast<uint16_t>(data[3]) << 8) | data[4]);

    return hdr;
}

std::optional<DynamicRecordHeader> DynamicRecordHeader::parse_redundant(const uint8_t* data, size_t max_size) {
    if (!data || max_size < REC_HEADER_REDUNDANT) return std::nullopt;

    DynamicRecordHeader hdr;
    hdr.header_size = REC_HEADER_REDUNDANT;

    // REDUNDANT format: 6 bytes
    // Byte 0-2: info_bits + n_owned + heap_no (similar layout)
    // Byte 3-5: next_record_offset (3 bytes for REDUNDANT)
    // This is rarely used in modern MySQL (COMPACT is default since 5.6)

    // Simplified parsing - focus on extracting key fields
    uint32_t info = (static_cast<uint32_t>(data[0]) << 16) |
                    (static_cast<uint32_t>(data[1]) << 8) |
                    data[2];

    hdr.rec_type    = info & 0x07;
    hdr.is_deleted  = (data[0] & 0x10) != 0;  // Different bit position in REDUNDANT
    hdr.is_min_rec  = (data[0] & 0x08) != 0;
    hdr.n_owned     = data[0] & 0x0F;
    hdr.heap_no     = (info >> 3) & 0x1FFF;

    // Next offset in REDUNDANT is 3 bytes
    hdr.next_offset = static_cast<int16_t>(
        (static_cast<uint16_t>(data[4]) << 8) | data[5]);

    return hdr;
}

// ==================== SchemaAgnosticScanner Implementation ====================

SchemaAgnosticScanner::SchemaAgnosticScanner(const Config& config)
    : config_(config) {}

// ==================== Phase 1: Page Structure Analysis ====================

bool SchemaAgnosticScanner::analyze_page_header(const uint8_t* page_data, size_t page_size,
                                                 PageAnalysisResult& result) {
    if (!page_data || page_size < 128) return false;

    // Check page type
    uint16_t page_type = read_be_val<uint16_t>(page_data + FIL_PAGE_TYPE_ABS);
    if (page_type != FIL_PAGE_INDEX) {
        return false;
    }

    result.is_valid = true;

    // Parse page header (at offset 38)
    result.n_dir_slots = read_be_val<uint16_t>(page_data + 38);
    result.heap_top    = read_be_val<uint16_t>(page_data + 40);
    result.n_heap      = read_be_val<uint16_t>(page_data + 42) & 0x7FFF;
    result.n_recs      = read_be_val<uint16_t>(page_data + 54);

    // Get index_id and level
    result.index_id = read_be_val<uint64_t>(page_data + 44);
    result.level    = read_be_val<uint16_t>(page_data + 42 + 8) >> 16;  // level is in high byte

    // Actually level is at offset 42+4 = 46, in the high byte of n_heap
    // Let's re-read correctly
    uint16_t n_heap_raw = read_be_val<uint16_t>(page_data + 42);
    result.level = (n_heap_raw >> 13) & 0x7;  // level is bits 15-13
    result.is_leaf = (result.level == 0);

    // Determine row format
    // COMPACT format: PAGE_N_HEAP has format bit in high bit
    // We assume COMPACT for modern MySQL (5.6+)
    result.is_compact = true;

    return true;
}

std::vector<uint16_t> SchemaAgnosticScanner::parse_page_directory(
    const uint8_t* page_data, size_t page_size, uint16_t n_dir_slots) {

    std::vector<uint16_t> offsets;
    offsets.reserve(n_dir_slots);

    // Page Directory is at page end, slots stored in reverse order
    // slot[0] (supremum) at page_size - 2
    // slot[n-1] (infimum) at page_size - 2 - (n-1)*2

    for (uint16_t i = 0; i < n_dir_slots; i++) {
        size_t slot_pos = page_size - 2 - (i * 2);
        if (slot_pos < FIL_HEADER_SIZE) break;

        uint16_t offset = read_be_val<uint16_t>(page_data + slot_pos);

        // Validate offset is in valid range
        if (offset >= INFIMUM_OFFSET && offset < page_size - 2) {
            offsets.push_back(offset);
        }
    }

    // Sort ascending for easier processing
    std::sort(offsets.begin(), offsets.end());

    return offsets;
}

// ==================== Phase 2: Record Chain Traversal ====================

std::vector<std::pair<size_t, size_t>> SchemaAgnosticScanner::find_record_boundaries(
    const uint8_t* page_data, size_t page_size,
    const std::vector<uint16_t>& dir_slots,
    uint16_t heap_top) {

    std::vector<std::pair<size_t, size_t>> boundaries;

    if (dir_slots.empty()) return boundaries;

    // Each directory slot owns a range of records
    // The record at the slot offset is the "pivot" record for that slot
    // Records before the pivot belong to previous slot

    for (size_t i = 0; i < dir_slots.size(); i++) {
        size_t start = dir_slots[i];

        // End boundary: either next slot or heap_top
        size_t end = (i + 1 < dir_slots.size()) ? dir_slots[i + 1] : heap_top;

        // Validate bounds
        if (start < end && end <= page_size) {
            boundaries.emplace_back(start, end);
        }
    }

    return boundaries;
}

std::vector<size_t> SchemaAgnosticScanner::follow_record_chain(
    const uint8_t* page_data, size_t page_size, bool is_compact) {

    std::vector<size_t> record_offsets;

    size_t header_size = is_compact ? REC_HEADER_COMPACT : REC_HEADER_REDUNDANT;

    // Start from infimum (offset 99 is the "origin", header is 5 bytes before)
    size_t infimum_origin = INFIMUM_OFFSET;
    size_t infimum_header = infimum_origin - header_size;  // 94 for COMPACT

    if (infimum_header >= page_size) return record_offsets;

    // Parse infimum header to get next offset
    auto hdr = is_compact ?
        DynamicRecordHeader::parse_compact(page_data + infimum_header, page_size - infimum_header) :
        DynamicRecordHeader::parse_redundant(page_data + infimum_header, page_size - infimum_header);

    if (!hdr) return record_offsets;

    // Follow chain: next_offset is relative to record origin
    size_t current_origin = infimum_origin;

    while (hdr->next_offset != 0) {
        // Calculate next record position
        // next_offset is relative to CURRENT record's data start (origin)
        ssize_t next_pos = static_cast<ssize_t>(current_origin) + hdr->next_offset;

        if (next_pos <= 0 || static_cast<size_t>(next_pos) >= page_size - 2) {
            break;  // Invalid offset
        }

        size_t next_origin = static_cast<size_t>(next_pos);
        size_t next_header = next_origin - header_size;

        if (next_header < FIL_HEADER_SIZE + PAGE_HEADER_SIZE) {
            break;  // Before user data area
        }

        // Skip system records (infimum heap_no=0, supremum heap_no=1)
        if (hdr->heap_no >= 2) {
            record_offsets.push_back(next_origin);
        }

        // Parse next header
        current_origin = next_origin;
        hdr = is_compact ?
            DynamicRecordHeader::parse_compact(page_data + next_header, page_size - next_header) :
            DynamicRecordHeader::parse_redundant(page_data + next_header, page_size - next_header);

        if (!hdr) break;

        // Stop at supremum (rec_type == 3)
        if (hdr->rec_type == 3) break;

        // Safety: limit iterations
        if (record_offsets.size() > 5000) break;
    }

    return record_offsets;
}

// ==================== Phase 3: Single Record Parsing ====================

std::optional<SchemalessRecord> SchemaAgnosticScanner::parse_record(
    const uint8_t* page_data, size_t page_size,
    size_t rec_offset, size_t rec_end,
    bool is_compact, bool is_leaf,
    uint32_t page_no) {

    size_t header_size = is_compact ? REC_HEADER_COMPACT : REC_HEADER_REDUNDANT;
    size_t rec_header_pos = rec_offset - header_size;

    if (rec_header_pos < 54 || rec_end > page_size) {
        return std::nullopt;
    }

    // Parse record header
    auto hdr = parse_record_header(page_data, rec_header_pos, page_size - rec_header_pos, is_compact);
    if (!hdr) return std::nullopt;

    // Skip system records
    if (hdr->is_system_record()) {
        return std::nullopt;
    }

    SchemalessRecord record;
    record.page_no = page_no;
    record.record_offset = static_cast<uint16_t>(rec_header_pos);
    record.data_offset = static_cast<uint16_t>(rec_offset);
    record.is_deleted = hdr->is_deleted;

    // Store raw header
    record.raw_header.assign(page_data + rec_header_pos, page_data + rec_header_pos + header_size);

    // Calculate data boundaries
    // Data starts at rec_offset (origin)
    // Data ends at: next_record_origin - header_size OR heap_top
    size_t data_end = rec_end;

    // Adjust for next record offset
    if (hdr->next_offset != 0) {
        ssize_t next_origin = static_cast<ssize_t>(rec_offset) + hdr->next_offset;
        if (next_origin > 0 && static_cast<size_t>(next_origin) > rec_offset) {
            data_end = static_cast<size_t>(next_origin) - header_size;
        }
    }

    if (data_end <= rec_offset || data_end > page_size) {
        data_end = rec_end;
    }

    record.data_length = static_cast<uint16_t>(data_end - rec_offset);

    if (record.data_length < 8) {
        return std::nullopt;  // Too small for valid record
    }

    // Store raw data
    record.raw_data.assign(page_data + rec_offset, page_data + data_end);

    // ==================== Phase 4: Field Extraction ====================

    size_t pos = rec_offset;  // Data start
    size_t remaining = data_end - pos;

    // For leaf pages: first 8 bytes is likely PK (BIGINT)
    if (is_leaf && remaining >= 8) {
        // Parse PK
        uint64_t raw_pk = read_be_val<uint64_t>(page_data + pos);
        raw_pk ^= 0x8000000000000000ULL;  // InnoDB signed format conversion

        SchemalessField pk_field;
        pk_field.type = SchemalessField::Type::INT_VAL;
        pk_field.int_value = static_cast<int64_t>(raw_pk);
        pk_field.str_value = std::to_string(pk_field.int_value);
        pk_field.raw_bytes.assign(page_data + pos, page_data + pos + 8);
        pk_field.confidence = 0.95;
        record.fields.push_back(pk_field);
        record.record_id = pk_field.int_value;

        pos += 8;
        remaining -= 8;
    }

    // For leaf pages: skip TRX_ID (6 bytes) + ROLL_PTR (7 bytes) = 13 bytes
    if (is_leaf && remaining >= 13) {
        pos += SYSTEM_COLS_SIZE;
        remaining -= 13;
    }

    // Now parse the rest of the data
    // Key insight: variable-length field sizes are stored at END of record

    if (remaining == 0) {
        record.is_valid = true;
        return record;
    }

    // ==================== Variable-Size Array Detection ====================

    // Scan backward from data_end to find variable-size array
    VarSizeArrayInfo var_info = parse_var_size_array(page_data, data_end - rec_offset, pos - rec_offset);

    // ==================== Field Data Extraction ====================

    size_t var_data_start = pos;
    size_t null_bytes_estimated = 0;

    // Estimate null bytes: typically 0-4 bytes for modern tables
    // Try different null byte counts and score each

    std::vector<SchemalessField> best_fields;
    double best_score = 0;

    for (size_t null_bytes = 0; null_bytes <= 4; null_bytes++) {
        size_t trial_end = data_end - null_bytes;

        std::vector<SchemalessField> trial_fields;

        // First try: use variable sizes if detected
        size_t field_pos = var_data_start;

        if (!var_info.sizes.empty() && var_info.confidence_score > 50) {
            for (uint32_t field_size : var_info.sizes) {
                if (field_pos >= trial_end) break;

                if (field_size == OFF_PAGE_FLAG_DYNAMIC || field_size == OFF_PAGE_FLAG_ANTLELOPE) {
                    // Overflow pointer (20 bytes)
                    if (field_pos + 20 <= trial_end) {
                        SchemalessField overflow_field;
                        overflow_field.type = SchemalessField::Type::OVERFLOW_PTR;
                        overflow_field.overflow_space_id = read_be_val<uint32_t>(page_data + field_pos);
                        overflow_field.overflow_page_no = read_be_val<uint32_t>(page_data + field_pos + 4);
                        overflow_field.overflow_size = read_be_val<uint64_t>(page_data + field_pos + 12);
                        overflow_field.raw_bytes.assign(page_data + field_pos, page_data + field_pos + 20);
                        trial_fields.push_back(overflow_field);
                        field_pos += 20;
                    }
                } else if (field_size > 0 && field_size < OFF_PAGE_FLAG_DYNAMIC && field_pos + field_size <= trial_end) {
                    // Regular field
                    SchemalessField field = parse_field_value(page_data + field_pos, field_size);
                    trial_fields.push_back(field);
                    field_pos += field_size;
                }
            }
        }

        // Parse remaining fixed-size fields
        while (field_pos < trial_end) {
            size_t rem = trial_end - field_pos;

            SchemalessField field;

            if (rem >= 8) {
                // Try 8-byte value
                uint64_t v8 = read_be_val<uint64_t>(page_data + field_pos);
                uint32_t v4 = read_be_val<uint32_t>(page_data + field_pos);

                if (looks_like_timestamp(v4)) {
                    field.type = SchemalessField::Type::TIMESTAMP_VAL;
                    field.str_value = format_timestamp(v4);
                    field.int_value = v4;
                    field.raw_bytes.assign(page_data + field_pos, page_data + field_pos + 4);
                    trial_fields.push_back(field);
                    field_pos += 4;
                } else if (looks_like_string(page_data + field_pos, rem)) {
                    // Looks like string, take as much as printable
                    size_t str_len = 0;
                    for (size_t i = 0; i < rem && i < 256; i++) {
                        uint8_t c = page_data[field_pos + i];
                        if (c >= 32 && c < 127) str_len++;
                        else if (str_len > 0) break;
                    }
                    if (str_len > 3) {
                        field = parse_field_value(page_data + field_pos, str_len);
                        trial_fields.push_back(field);
                        field_pos += str_len;
                    } else {
                        field = parse_field_value(page_data + field_pos, 8);
                        trial_fields.push_back(field);
                        field_pos += 8;
                    }
                } else {
                    field = parse_field_value(page_data + field_pos, 8);
                    trial_fields.push_back(field);
                    field_pos += 8;
                }
            } else if (rem >= 4) {
                uint32_t v4 = read_be_val<uint32_t>(page_data + field_pos);
                if (looks_like_timestamp(v4)) {
                    field.type = SchemalessField::Type::TIMESTAMP_VAL;
                    field.str_value = format_timestamp(v4);
                    field.int_value = v4;
                } else {
                    field.type = SchemalessField::Type::UINT_VAL;
                    field.int_value = v4;
                    field.str_value = std::to_string(v4);
                }
                field.raw_bytes.assign(page_data + field_pos, page_data + field_pos + 4);
                trial_fields.push_back(field);
                field_pos += 4;
            } else if (rem >= 2) {
                field.type = SchemalessField::Type::UINT_VAL;
                field.int_value = read_be_val<uint16_t>(page_data + field_pos);
                field.str_value = std::to_string(field.int_value);
                field.raw_bytes.assign(page_data + field_pos, page_data + field_pos + 2);
                trial_fields.push_back(field);
                field_pos += 2;
            } else {
                field.type = SchemalessField::Type::UINT_VAL;
                field.int_value = page_data[field_pos];
                field.str_value = std::to_string(field.int_value);
                field.raw_bytes.assign(page_data + field_pos, page_data + field_pos + 1);
                trial_fields.push_back(field);
                field_pos += 1;
            }
        }

        // Score this trial
        double score = 0;
        for (const auto& f : trial_fields) {
            if (f.type == SchemalessField::Type::STRING_VAL) score += 10;
            else if (f.type == SchemalessField::Type::TIMESTAMP_VAL) score += 8;
            else if (f.type == SchemalessField::Type::INT_VAL) score += 5;
            else if (f.type == SchemalessField::Type::NULL_VAL) score += 2;
            else if (f.type == SchemalessField::Type::UNKNOWN) score -= 3;
        }

        if (score > best_score) {
            best_score = score;
            best_fields = std::move(trial_fields);
        }
    }

    // Append parsed fields to record
    for (auto& field : best_fields) {
        record.fields.push_back(std::move(field));
    }

    record.is_valid = true;
    record.confidence = best_score / (record.fields.size() * 10 + 1);

    return record;
}

std::optional<DynamicRecordHeader> SchemaAgnosticScanner::parse_record_header(
    const uint8_t* data, size_t offset, size_t max_size, bool is_compact) {

    if (offset + REC_HEADER_COMPACT > max_size) return std::nullopt;

    return is_compact ?
        DynamicRecordHeader::parse_compact(data + offset, max_size - offset) :
        DynamicRecordHeader::parse_redundant(data + offset, max_size - offset);
}

// ==================== Phase 4: Field Extraction ====================

VarSizeArrayInfo SchemaAgnosticScanner::parse_var_size_array(
    const uint8_t* data, size_t data_len, size_t var_start_offset) {

    VarSizeArrayInfo info;
    info.confidence_score = 0;

    if (data_len < 10) return info;

    // Variable sizes are stored at END of record data, read backward
    // We scan backward from data_len to find size bytes

    // Try multiple starting positions (accounting for null bytes)
    for (size_t null_bytes = 0; null_bytes <= 4; null_bytes++) {
        size_t scan_pos = data_len - null_bytes;
        if (scan_pos <= var_start_offset + 8) continue;

        std::vector<uint32_t> trial_sizes;
        size_t total_size = 0;
        bool valid = true;

        // Read backward
        while (scan_pos > var_start_offset + 8 && trial_sizes.size() < 30) {
            if (scan_pos < 1) { valid = false; break; }

            uint8_t byte1 = data[scan_pos - 1];

            if (byte1 <= 0x7F) {
                // 1-byte size
                trial_sizes.push_back(byte1);
                total_size += byte1;
                scan_pos -= 1;
            } else {
                // 2-byte size
                if (scan_pos < 2) { valid = false; break; }
                uint8_t byte2 = data[scan_pos - 2];
                uint32_t vsize = (static_cast<uint32_t>(byte1 & 0x7F) << 8) | byte2;

                if (vsize == OFF_PAGE_FLAG_DYNAMIC || vsize == OFF_PAGE_FLAG_ANTLELOPE) {
                    trial_sizes.push_back(vsize);
                    info.has_overflow = true;
                    total_size += 20;  // Overflow pointer size
                    scan_pos -= 2;
                } else if (vsize < 65535) {
                    trial_sizes.push_back(vsize);
                    total_size += vsize;
                    scan_pos -= 2;
                } else {
                    valid = false;
                    break;
                }
            }

            // Check total doesn't exceed available space
            if (total_size > (scan_pos - var_start_offset)) {
                valid = false;
                break;
            }
        }

        if (!valid) continue;

        // Calculate score
        size_t data_available = scan_pos - var_start_offset;
        int score = 0;

        if (total_size <= data_available) {
            score += 20;
            size_t remaining = data_available - total_size;

            // Prefer some remaining space for fixed fields
            if (remaining >= 8 && remaining <= 100) score += 10;

            // Score individual sizes
            for (uint32_t s : trial_sizes) {
                if (s == 0) score -= 5;
                else if (s >= 4 && s <= 64) score += 5;  // Common string lengths
                else if (s >= 65 && s <= 255) score += 2;
                else if (s > 255) score -= 2;  // Suspicious
            }

            score += static_cast<int>(trial_sizes.size()) * 3;
        }

        if (score > info.confidence_score) {
            info.confidence_score = score;
            info.sizes = std::move(trial_sizes);
            info.bytes_read = data_len - null_bytes - scan_pos;
        }
    }

    return info;
}

SchemalessField::Type SchemaAgnosticScanner::detect_field_type(const uint8_t* data, size_t size) {
    if (size == 0) return SchemalessField::Type::NULL_VAL;

    // Check for string
    if (looks_like_string(data, size)) {
        // Try to detect specific patterns
        std::string str(reinterpret_cast<const char*>(data), size);

        // Timestamp pattern
        if (str.find("20") == 0 && (str.find("-") == 4 || str.find(":") != std::string::npos)) {
            return SchemalessField::Type::STRING_VAL;
        }

        return SchemalessField::Type::STRING_VAL;
    }

    // Numeric types
    if (size == 8) {
        uint64_t v = read_be_val<uint64_t>(data);
        v ^= 0x8000000000000000ULL;

        // Check if it's a reasonable integer
        if (v < 1000000000ULL) {
            return SchemalessField::Type::INT_VAL;
        }
        return SchemalessField::Type::BINARY_VAL;
    }

    if (size == 4) {
        uint32_t v = read_be_val<uint32_t>(data);
        if (looks_like_timestamp(v)) {
            return SchemalessField::Type::TIMESTAMP_VAL;
        }
        return SchemalessField::Type::UINT_VAL;
    }

    if (size == 1) {
        return SchemalessField::Type::UINT_VAL;
    }

    return SchemalessField::Type::BINARY_VAL;
}

SchemalessField SchemaAgnosticScanner::parse_field_value(const uint8_t* data, size_t size) {
    SchemalessField field;
    field.raw_bytes.assign(data, data + size);

    if (size == 0) {
        field.is_null = true;
        field.type = SchemalessField::Type::NULL_VAL;
        return field;
    }

    field.type = detect_field_type(data, size);

    switch (field.type) {
        case SchemalessField::Type::STRING_VAL:
            field.str_value = std::string(reinterpret_cast<const char*>(data), size);
            break;

        case SchemalessField::Type::TIMESTAMP_VAL:
            field.int_value = read_be_val<uint32_t>(data);
            field.str_value = format_timestamp(field.int_value);
            break;

        case SchemalessField::Type::INT_VAL:
        case SchemalessField::Type::UINT_VAL:
            if (size == 8) {
                uint64_t v = read_be_val<uint64_t>(data);
                v ^= 0x8000000000000000ULL;
                field.int_value = static_cast<int64_t>(v);
            } else if (size == 4) {
                field.int_value = read_be_val<uint32_t>(data);
            } else if (size == 2) {
                field.int_value = read_be_val<uint16_t>(data);
            } else {
                field.int_value = data[0];
            }
            field.str_value = std::to_string(field.int_value);
            break;

        default:
            // Binary - convert to hex
            break;
    }

    return field;
}

// ==================== Phase 5: Heuristic Validation ====================

double SchemaAgnosticScanner::score_record_quality(const SchemalessRecord& record) {
    double score = 0;

    // Has valid PK
    if (record.record_id > 0 && record.record_id < 10000000000) {
        score += 20;
    }

    // Has reasonable number of fields
    if (record.fields.size() >= 3 && record.fields.size() <= 50) {
        score += 10;
    }

    // Has string fields
    for (const auto& field : record.fields) {
        if (field.type == SchemalessField::Type::STRING_VAL) {
            if (!field.str_value.empty() && field.str_value.size() >= 4) {
                score += 5;
            }
        }
    }

    // Not deleted
    if (!record.is_deleted) {
        score += 5;
    }

    return score;
}

bool SchemaAgnosticScanner::validate_record_bounds(size_t start, size_t end, size_t heap_top) {
    return start >= INFIMUM_OFFSET && start < end && end <= heap_top + 100;
}

// ==================== Full Page Parsing ====================

PageAnalysisResult SchemaAgnosticScanner::parse_page(
    const uint8_t* page_data, size_t page_size, uint32_t page_no) {

    PageAnalysisResult result;
    result.page_no = page_no;

    // Phase 1: Validate page structure
    if (!analyze_page_header(page_data, page_size, result)) {
        result.is_valid = false;
        result.errors.push_back("Not a valid INDEX page");
        return result;
    }

    // Phase 2: Parse Page Directory
    std::vector<uint16_t> dir_slots = parse_page_directory(page_data, page_size, result.n_dir_slots);

    if (dir_slots.empty()) {
        // Fallback: use record chain
        std::vector<size_t> chain_offsets = follow_record_chain(page_data, page_size, result.is_compact);

        for (size_t offset : chain_offsets) {
            auto rec = parse_record(page_data, page_size, offset, result.heap_top,
                                    result.is_compact, result.is_leaf, page_no);
            if (rec && rec->is_valid) {
                result.records.push_back(std::move(*rec));
            }
        }
    } else {
        // Use Page Directory for precise boundaries
        std::vector<std::pair<size_t, size_t>> boundaries =
            find_record_boundaries(page_data, page_size, dir_slots, result.heap_top);

        for (auto& [start, end] : boundaries) {
            // Parse record chain within this boundary
            size_t header_size = result.is_compact ? REC_HEADER_COMPACT : REC_HEADER_REDUNDANT;
            size_t origin = start + header_size;

            auto rec = parse_record(page_data, page_size, origin, end,
                                    result.is_compact, result.is_leaf, page_no);
            if (rec && rec->is_valid) {
                result.records.push_back(std::move(*rec));
            }
        }
    }

    total_pages_scanned_++;
    total_records_parsed_ += result.records.size();

    return result;
}

// ==================== Full File Parsing ====================

std::vector<SchemalessRecord> SchemaAgnosticScanner::parse_ibd(const std::string& ibd_path) {
    std::vector<SchemalessRecord> all_records;

    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return all_records;
    }

    size_t file_size = file.tellg();
    file.seekg(0);

    // Detect page size from file size
    std::vector<size_t> page_sizes = {16384, 8192, 32768, 4096, 65536};
    size_t page_size = 16384;

    for (uint16_t ps : page_sizes) {
        if (file_size % ps == 0 && file_size >= ps * 10) {
            page_size = ps;
            break;
        }
    }

    size_t total_pages = file_size / page_size;

    std::vector<uint8_t> page_buf(page_size);

    for (uint32_t page_no = 0; page_no < total_pages; page_no++) {
        file.seekg(static_cast<std::streamsize>(page_no) * page_size);
        if (!file.read(reinterpret_cast<char*>(page_buf.data()), page_size)) break;

        auto result = parse_page(page_buf.data(), page_size, page_no);

        if (result.is_valid && result.is_leaf) {
            for (auto& rec : result.records) {
                all_records.push_back(std::move(rec));
            }
        }

        if (progress_cb_) {
            progress_cb_(page_no, all_records.size());
        }
    }

    return all_records;
}

} // namespace innodb
} // namespace dbrecovery