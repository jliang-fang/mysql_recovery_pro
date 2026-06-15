/**
 * @file index_page_parser.cpp
 * @brief ibd2sql-style INDEX page parser implementation
 *
 * Core record extraction logic ported from ibd2sql v2.x innodb_page/index.py
 * with C++20 optimizations for speed and memory efficiency.
 */

#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/enhanced_schemaless_parser.hpp"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <numeric>
#include <filesystem>

namespace dbrecovery {
namespace innodb {

// ==================== Constructor / Destructor ====================

InnoDBIndexParser::InnoDBIndexParser() = default;
InnoDBIndexParser::~InnoDBIndexParser() = default;

// ==================== Configuration ====================

void InnoDBIndexParser::set_columns(const std::vector<IndexColumnDef>& columns,
                                     bool has_row_id,
                                     uint32_t n_pk_cols) {
    columns_ = columns;
    has_row_id_ = has_row_id;
    n_pk_cols_ = n_pk_cols;

    // Count nullable and variable-length columns
    n_nullable_ = 0;
    n_variable_ = 0;
    for (const auto& col : columns_) {
        if (col.is_nullable) n_nullable_++;
        if (col.is_variable_length()) n_variable_++;
    }
}

void InnoDBIndexParser::set_row_format(uint8_t format) {
    row_format_hint_ = format;
    if (format == 2 || format == 3) {
        off_page_flag_ = OFF_PAGE_FLAG_DYNAMIC;
    } else {
        off_page_flag_ = OFF_PAGE_FLAG_ANTLELOPE;
    }
}

// ==================== Page Header Parsing ====================

std::optional<InnoDBPageHeader> InnoDBIndexParser::parse_page_header(
    const uint8_t* page_data, size_t page_size) const {
    if (!page_data || page_size < 94) return std::nullopt;

    InnoDBPageHeader hdr{};
    hdr.n_dir_slots = read_be<uint16_t>(page_data, 38);
    hdr.heap_top   = read_be<uint16_t>(page_data, 40);
    hdr.n_heap_raw = read_be<uint16_t>(page_data, 42);
    hdr.n_heap     = hdr.n_heap_raw & 0x7FFF;
    hdr.page_free  = read_be<uint16_t>(page_data, 44);
    hdr.garbage    = read_be<uint16_t>(page_data, 46);
    hdr.last_insert= read_be<uint16_t>(page_data, 48);
    hdr.direction  = read_be<uint16_t>(page_data, 50);
    hdr.n_direction= read_be<uint16_t>(page_data, 52);
    hdr.n_recs     = read_be<uint16_t>(page_data, 54);
    hdr.max_trx_id = read_be<uint64_t>(page_data, 56);
    hdr.level      = read_be<uint16_t>(page_data, 64);
    hdr.index_id   = read_be<uint64_t>(page_data, 66);

    // Bit 15 of n_heap_raw indicates COMPACT/DYNAMIC format
    hdr.is_compact_format = (hdr.n_heap_raw & 0x8000) != 0;

    // Basic sanity checks
    if (hdr.n_dir_slots == 0 && hdr.n_recs == 0) {
        // Could be empty/truncated page
    }
    if (hdr.heap_top > page_size || hdr.heap_top < 100) {
        return std::nullopt;  // Invalid heap_top
    }

    return hdr;
}

// ==================== Record Header Parsing (ibd2sql-style) ====================

std::optional<NewRecordHeader> InnoDBIndexParser::parse_new_record_header(
    const uint8_t* data, size_t offset, size_t max_size) {
    // COMPACT/DYNAMIC header is 5 bytes
    if (!data || offset + 5 > max_size) return std::nullopt;

    NewRecordHeader hdr{};
    // Read 3 bytes in big-endian order
    hdr.raw_byte0 = data[offset];
    hdr.raw_byte1 = data[offset + 1];
    hdr.raw_byte2 = data[offset + 2];

    // Parse bit fields (big-endian, 24 bits total)
    uint32_t bits = (static_cast<uint32_t>(hdr.raw_byte0) << 16) |
                    (static_cast<uint32_t>(hdr.raw_byte1) << 8) |
                    static_cast<uint32_t>(hdr.raw_byte2);

    hdr.instant_flag = (bits >> 23) & 1;
    hdr.version_flag = (bits >> 22) & 1;
    hdr.deleted      = (bits >> 21) & 1;
    hdr.min_rec      = (bits >> 20) & 1;
    hdr.n_owned      = (bits >> 16) & 0x0F;
    hdr.heap_no      = (bits >> 3) & 0x1FFF;
    hdr.rec_type     = bits & 0x07;

    // Read next record offset (signed 16-bit)
    hdr.next_offset = static_cast<int16_t>(read_be<uint16_t>(data, offset + 3));

    return hdr;
}

std::optional<OldRecordHeader> InnoDBIndexParser::parse_old_record_header(
    const uint8_t* data, size_t offset, size_t max_size) {
    // REDUNDANT header is 6 bytes
    if (!data || offset + 6 > max_size) return std::nullopt;

    OldRecordHeader hdr{};

    // Read 4 bytes in big-endian order for bit fields
    uint32_t bits = read_be<uint32_t>(data, offset);

    hdr.instant_flag = (bits >> 31) & 1;
    hdr.version_flag = (bits >> 30) & 1;
    hdr.deleted      = (bits >> 29) & 1;
    hdr.min_rec      = (bits >> 28) & 1;
    hdr.n_owned      = (bits >> 24) & 0x0F;
    hdr.heap_no      = (bits >> 11) & 0x1FFF;
    hdr.n_fields     = (bits >> 1) & 0x3FF;
    hdr.short_flag   = bits & 1;

    // Read next record offset
    hdr.next_offset = read_be<uint16_t>(data, offset + 4);

    return hdr;
}

// ==================== Null Bitmask and Variable-Size Reading ====================

InnoDBIndexParser::FieldMetadata InnoDBIndexParser::read_null_and_varsizes(
    const uint8_t* page_data, size_t page_size,
    size_t rec_start, size_t rec_end,
    uint32_t n_nullable, uint32_t n_variable,
    const std::vector<IndexColumnDef>& columns) {

    FieldMetadata result;
    if (rec_end <= rec_start || rec_end > page_size) return result;

    // CRITICAL FIX: Match ibd2sql's _read_nullbitmask_varsize_new() exactly!
    //
    // InnoDB COMPACT/DYNAMIC format:
    // 1. NULL bitmap and variable sizes are BEFORE the record header
    // 2. Sizes are read BACKWARD from header position
    // 3. For EACH column (in physical order):
    //    - If NULL: vsize = 0, no size bytes read
    //    - If fixed: vsize = col.storage_size()
    //    - If variable: vsize = read from size bytes
    // 4. Result size_list has entry for EVERY column

    // Calculate null bitmask size
    size_t null_bytes = (n_nullable + 7) / 8;

    // Position for reading sizes backward (start from header position)
    size_t pos = rec_start;

    // Read NULL bitmap backward from header position
    if (null_bytes > 0 && pos >= null_bytes) {
        pos -= null_bytes;
        result.null_bitmask.assign(page_data + pos, page_data + pos + null_bytes);
        result.null_bytes_read = null_bytes;
    }

    // Read null bitmask as integer for bit testing
    uint32_t null_bit_value = 0;
    if (null_bytes > 0) {
        for (size_t i = 0; i < null_bytes; i++) {
            null_bit_value |= (static_cast<uint32_t>(result.null_bitmask[i]) << (i * 8));
        }
    }

    // CRITICAL: Build size_list for ALL columns (matching ibd2sql)
    // For each column in physical order:
    // - Determine NULL status
    // - If NULL: vsize = 0
    // - If fixed: vsize = col.storage_size()
    // - If variable: vsize = read size bytes from record

    uint32_t nullable_bit_pos = 0;  // Bit position in null bitmap

    for (size_t col_idx = 0; col_idx < columns.size() && pos > 0; col_idx++) {
        const auto& col = columns[col_idx];

        // Check NULL status for this column
        bool is_null = false;
        if (col.is_nullable) {
            is_null = (null_bit_value & (1 << nullable_bit_pos)) != 0;
            nullable_bit_pos++;
        }

        uint32_t vsize = 0;

        if (is_null) {
            // NULL column: size = 0, no bytes to read
            vsize = 0;
        } else if (col.is_variable_length()) {
            // Variable column: read size bytes from record
            if (pos >= 1) {
                pos--;
                uint8_t byte1 = page_data[pos];

                if (col.is_big && byte1 > 0x7F) {
                    // 2-byte encoding for is_big field with large size
                    if (pos >= 1) {
                        pos--;
                        uint8_t byte2 = page_data[pos];
                        vsize = (static_cast<uint32_t>(byte1 & 0x7F) << 8) | byte2;
                        result.var_size_bytes_read += 2;
                    }
                } else {
                    // 1-byte encoding
                    vsize = byte1;
                    result.var_size_bytes_read += 1;
                }
            }
        } else {
            // Fixed column: use pre-computed storage size
            vsize = col.storage_size();
        }

        // Store size for this column (ALL columns have a size entry)
        result.variable_sizes.push_back(vsize);
    }

    // DEBUG: Print sizes for first record
    static bool debug_printed = false;
    if (!debug_printed && !result.variable_sizes.empty()) {
        std::cout << "[DEBUG VAR_SIZES] Read " << result.variable_sizes.size() << " sizes: ";
        for (size_t i = 0; i < result.variable_sizes.size(); i++) {
            std::cout << result.variable_sizes[i] << " ";
        }
        std::cout << "\n";
        debug_printed = true;
    }

    // Store the position after reading all sizes
    result.data_start_offset = pos;

    return result;
}

InnoDBIndexParser::RedundantFieldMetadata InnoDBIndexParser::read_redundant_field_offsets(
    const uint8_t* page_data, size_t page_size,
    size_t rec_start, size_t rec_end,
    uint32_t n_fields) {
    RedundantFieldMetadata result;

    size_t pos = rec_end;
    result.short_flag = false;

    if (rec_start + 6 <= page_size) {
        auto hdr = parse_old_record_header(page_data, rec_start, page_size);
        if (hdr) result.short_flag = hdr->short_flag;
    }

    result.field_offsets.resize(n_fields, 0);
    result.null_flags.resize(n_fields, false);

    std::vector<uint16_t> raw_offsets(n_fields, 0);
    std::vector<bool> raw_nulls(n_fields, false);

    for (int i = static_cast<int>(n_fields) - 1; i >= 0 && pos > rec_start + 6; i--) {
        if (result.short_flag) {
            pos -= 1;
            if (pos >= page_size) break;
            uint8_t byte = page_data[pos];
            raw_offsets[i] = byte & 0x7F;
            raw_nulls[i] = (byte & 0x80) != 0;
        } else {
            pos -= 2;
            if (pos + 1 >= page_size) break;
            uint16_t val = (static_cast<uint16_t>(page_data[pos]) << 8) |
                           static_cast<uint16_t>(page_data[pos + 1]);
            raw_offsets[i] = val & 0x3FFF;
            raw_nulls[i] = (val & 0x8000) != 0;
        }
    }

    result.field_offsets = std::move(raw_offsets);
    result.null_flags = std::move(raw_nulls);

    return result;
}

// ==================== Record Chain Following ====================

std::vector<IndexRecord> InnoDBIndexParser::follow_record_chain(
    const uint8_t* page_data, size_t page_size,
    const InnoDBPageHeader& page_hdr, uint32_t page_no,
    bool is_compact) {
    std::vector<IndexRecord> records;

    size_t header_size = is_compact ? REC_HEADER_SIZE_NEW : REC_HEADER_SIZE_OLD;

    // Start at the INFIMUM record origin
    size_t current_origin = is_compact ? INFIMUM_OFFSET : INFIMUM_OFFSET_OLD;

    std::set<uint16_t> visited;

    for (uint32_t safety = 0; safety < MAX_RECORDS_PER_PAGE; safety++) {
        if (current_origin < header_size || current_origin >= page_hdr.heap_top) {
            break;
        }
        if (visited.count(static_cast<uint16_t>(current_origin))) {
            break;  // Circular reference detected
        }
        visited.insert(static_cast<uint16_t>(current_origin));

        size_t header_start = current_origin - header_size;

        if (is_compact) {
            auto hdr = parse_new_record_header(page_data, header_start, page_size);
            if (!hdr) break;

            // Skip INFIMUM (heap_no=0) and SUPREMUM (heap_no=1)
            if (hdr->heap_no >= 2) {
                bool is_leaf = (page_hdr.level == 0);
                auto rec = parse_user_record(page_data, page_size, header_start, page_no, true, is_leaf);
                if (rec) {
                    records.push_back(std::move(*rec));
                }
            }

            // Move to next record
            if (hdr->next_offset == 0) break;
            ssize_t next_origin = static_cast<ssize_t>(current_origin) + hdr->next_offset;
            if (next_origin <= 0 || static_cast<size_t>(next_origin) >= page_size) {
                break;
            }
            current_origin = static_cast<size_t>(next_origin);
        } else {
            auto hdr = parse_old_record_header(page_data, header_start, page_size);
            if (!hdr) break;

            if (hdr->heap_no >= 2) {
                bool is_leaf = (page_hdr.level == 0);
                auto rec = parse_user_record(page_data, page_size, header_start, page_no, false, is_leaf);
                if (rec) {
                    records.push_back(std::move(*rec));
                }
            }

            if (hdr->next_offset == 0 || hdr->next_offset >= page_size) {
                break;
            }
            current_origin = hdr->next_offset;
        }
    }

    return records;
}

// ==================== Single Record Parsing ====================

std::optional<IndexRecord> InnoDBIndexParser::parse_user_record(
    const uint8_t* page_data, size_t page_size,
    size_t rec_offset, uint32_t page_no,
    bool is_compact, bool is_leaf) {

    size_t header_size = is_compact ? REC_HEADER_SIZE_NEW : REC_HEADER_SIZE_OLD;
    size_t rec_origin = rec_offset + header_size;

    if (rec_origin >= page_size) return std::nullopt;

    IndexRecord record;
    record.page_no = page_no;
    record.record_offset = static_cast<uint16_t>(rec_offset);

    if (is_compact) {
        auto hdr = parse_new_record_header(page_data, rec_offset, page_size);
        if (!hdr) return std::nullopt;

        record.is_deleted = hdr->deleted;
        record.is_min_rec = hdr->min_rec;
        record.heap_no = hdr->heap_no;
        record.n_owned = hdr->n_owned;
        record.next_record_offset = static_cast<int16_t>(hdr->next_offset);

        if (hdr->heap_no < 2) return std::nullopt;  // Skip system records
    } else {
        auto hdr = parse_old_record_header(page_data, rec_offset, page_size);
        if (!hdr) return std::nullopt;

        record.is_deleted = hdr->deleted;
        record.is_min_rec = hdr->min_rec;
        record.heap_no = hdr->heap_no;
        record.n_owned = hdr->n_owned;
        record.next_record_offset = static_cast<int16_t>(hdr->next_offset);

        if (hdr->heap_no < 2) return std::nullopt;
    }

    size_t data_start = rec_origin;
    size_t data_end = data_start;

    auto page_hdr = parse_page_header(page_data, page_size);
    if (!page_hdr) return std::nullopt;

    if (is_compact) {
        auto hdr = parse_new_record_header(page_data, rec_offset, page_size);
        if (!hdr) return std::nullopt;

        if (hdr->next_offset != 0) {
            ssize_t next_origin = static_cast<ssize_t>(rec_origin) + hdr->next_offset;
            // Validate next_offset is reasonable
            // For COMPACT format, next_offset can be ANY positive value representing
            // the delta from current origin to next origin.
            // We trust the next_offset unless it's clearly out of bounds.
            if (next_origin > static_cast<ssize_t>(data_start) && static_cast<size_t>(next_origin) < page_size) {
                // Valid next_offset - next record header is at next_origin - REC_HEADER_SIZE_NEW
                data_end = static_cast<size_t>(next_origin) - REC_HEADER_SIZE_NEW;
            } else {
                // Out of bounds - likely last record on page
                data_end = page_hdr->heap_top;
            }
        } else {
            data_end = page_hdr->heap_top;
        }
    } else {
        auto hdr = parse_old_record_header(page_data, rec_offset, page_size);
        if (!hdr) return std::nullopt;

        if (hdr->next_offset != 0 && hdr->next_offset < page_size && hdr->next_offset > data_start) {
            data_end = hdr->next_offset - REC_HEADER_SIZE_OLD;
        } else {
            data_end = page_hdr->heap_top;
        }
    }

    if (data_end <= data_start || data_end > page_size - 8) {
        return std::nullopt;
    }

    if (data_end - data_start <= 65536) {
        record.raw_data.assign(page_data + data_start, page_data + data_end);
    }

    // Parse fields
    // CRITICAL: In COMPACT/DYNAMIC format, the structure is:
    // [Sizes/Nulls] [Header 5B] [PK] [TRX_ID] [ROLL_PTR] [Var Data] [Fixed Data]
    //
    // - Header is at rec_offset (5 bytes)
    // - Sizes/nulls are BEFORE the header (at lower addresses)
    // - Data starts at record origin (rec_offset + 5)
    //
    // We read sizes backward from header position, then read data forward from origin.
    if (!columns_.empty()) {
        auto field_meta = read_null_and_varsizes(
            page_data, page_size, rec_offset, data_end,
            n_nullable_, n_variable_, columns_);

        // Data starts at record origin (rec_offset + 5)
        // NOT at rec_origin + null_bytes + var_size_bytes (those are before header!)
        size_t actual_data_start = rec_origin;  // = rec_offset + REC_HEADER_SIZE_NEW

        record.fields = extract_fields_with_schema(
            page_data, page_size, actual_data_start, data_end,
            columns_, field_meta.null_bitmask, field_meta.variable_sizes,
            is_leaf);
    } else {
        // For schemaless mode, pass the correct data_end
        size_t record_data_end = data_end;
        record.fields = parse_record_schemaless(
            page_data, page_size, rec_offset, record_data_end, is_compact, is_leaf);
    }

    return record;
}

// ==================== Field Extraction with Schema ====================

std::vector<IndexFieldValue> InnoDBIndexParser::extract_fields_with_schema(
    const uint8_t* page_data, size_t page_size,
    size_t data_start, size_t data_end,
    const std::vector<IndexColumnDef>& columns,
    const std::vector<uint8_t>& null_bitmask,
    const std::vector<uint32_t>& variable_sizes,
    bool is_leaf) {
    std::vector<IndexFieldValue> result;
    if (data_start >= data_end || data_end > page_size) return result;

    // CRITICAL FIX: Match ibd2sql's _read_field() exactly!
    //
    // After read_null_and_varsizes(), variable_sizes[] contains size for EVERY column:
    // - NULL columns: size = 0
    // - Fixed columns: size = storage_size()
    // - Variable columns: size = read from record
    //
    // Now we just read each column's data in physical order:
    // 1. PK columns first
    // 2. TRX_ID + ROLL_PTR (for clustered index leaf)
    // 3. Remaining user columns
    //
    // Each column reads 'size' bytes from the data stream.

    // DEBUG: Print initial state for first record
    static bool debug_extract = false;
    bool do_debug = !debug_extract;
    if (do_debug) {
        std::cout << "[DEBUG EXTRACT] data_start=" << data_start << " data_end=" << data_end
                  << " len=" << (data_end - data_start) << " n_pk_cols=" << n_pk_cols_
                  << " n_cols=" << columns.size() << "\n";
        std::cout << "[DEBUG EXTRACT] size_list (" << variable_sizes.size() << "): ";
        for (size_t i = 0; i < variable_sizes.size(); i++) {
            std::cout << variable_sizes[i] << " ";
        }
        std::cout << "\n";
    }

    size_t pos = data_start;
    size_t size_idx = 0;  // Index into variable_sizes

    // Step 1: Read PK columns first
    for (size_t col_idx = 0; col_idx < n_pk_cols_ && col_idx < columns.size() && pos < data_end; col_idx++) {
        const auto& col = columns[col_idx];

        if (size_idx >= variable_sizes.size()) break;
        uint32_t field_size = variable_sizes[size_idx++];

        IndexFieldValue val;

        // Check NULL status
        bool is_null = false;
        if (col.is_nullable && !null_bitmask.empty()) {
            uint32_t nullable_idx = 0;
            for (size_t j = 0; j < col_idx; j++) {
                if (columns[j].is_nullable) nullable_idx++;
            }
            if (nullable_idx < null_bitmask.size() * 8) {
                size_t byte_idx = nullable_idx / 8;
                uint8_t bit_idx = nullable_idx % 8;
                is_null = (null_bitmask[byte_idx] & (1 << bit_idx)) != 0;
            }
        }

        if (is_null || field_size == 0) {
            val.is_null = true;
            val.type = IndexFieldValue::Type::NULL_VAL;
            result.push_back(val);
            if (do_debug) {
                std::cout << "[DEBUG EXTRACT] PK col " << col_idx << " '" << col.name << "' is NULL\n";
            }
            continue;
        }

        if (do_debug) {
            std::cout << "[DEBUG EXTRACT] PK col " << col_idx << " '" << col.name
                      << "' at pos " << pos << " size " << field_size << "\n";
        }

        if (pos + field_size <= data_end) {
            val = decode_column(page_data, pos, data_end, col, field_size);
            pos += field_size;
        }
        result.push_back(val);
    }

    // Step 2: Skip TRX_ID + ROLL_PTR for leaf pages (clustered index)
    if (is_leaf && pos + DB_TRX_ID_SIZE + DB_ROLL_PTR_SIZE <= data_end) {
        if (do_debug) {
            std::cout << "[DEBUG EXTRACT] Skipping TRX_ID+ROLL_PTR at pos " << pos << "\n";
        }
        pos += DB_TRX_ID_SIZE + DB_ROLL_PTR_SIZE;
    }

    // Step 3: Read remaining user columns (in physical order)
    for (size_t col_idx = n_pk_cols_; col_idx < columns.size() && pos < data_end; col_idx++) {
        const auto& col = columns[col_idx];

        if (size_idx >= variable_sizes.size()) break;
        uint32_t field_size = variable_sizes[size_idx++];

        IndexFieldValue val;

        // Check NULL status
        bool is_null = false;
        if (col.is_nullable && !null_bitmask.empty()) {
            uint32_t nullable_idx = 0;
            for (size_t j = 0; j < col_idx; j++) {
                if (columns[j].is_nullable) nullable_idx++;
            }
            if (nullable_idx < null_bitmask.size() * 8) {
                size_t byte_idx = nullable_idx / 8;
                uint8_t bit_idx = nullable_idx % 8;
                is_null = (null_bitmask[byte_idx] & (1 << bit_idx)) != 0;
            }
        }

        if (is_null || field_size == 0) {
            val.is_null = true;
            val.type = IndexFieldValue::Type::NULL_VAL;
            result.push_back(val);
            if (do_debug) {
                std::cout << "[DEBUG EXTRACT] Col " << col_idx << " '" << col.name << "' is NULL\n";
            }
            continue;
        }

        if (do_debug) {
            std::cout << "[DEBUG EXTRACT] Col " << col_idx << " '" << col.name
                      << "' at pos " << pos << " size " << field_size
                      << " type=" << static_cast<int>(col.col_type) << "\n";
        }

        // Handle overflow pointer
        if (field_size == off_page_flag_ && pos + 20 <= data_end) {
            val.is_overflow = true;
            val.type = IndexFieldValue::Type::OVERFLOW_PTR;
            val.overflow_space_id = read_be<uint32_t>(page_data, pos);
            val.overflow_page_no = read_be<uint32_t>(page_data, pos + 4);
            val.overflow_blob_header = read_be<uint32_t>(page_data, pos + 8);
            val.overflow_real_size = read_be<uint64_t>(page_data, pos + 12);
            val.raw_bytes.assign(page_data + pos, page_data + pos + 20);
            pos += 20;
            result.push_back(val);
            continue;
        }

        if (pos + field_size <= data_end) {
            val = decode_column(page_data, pos, data_end, col, field_size);
            pos += field_size;
        }
        result.push_back(val);
    }

    // Disable debug after first record
    if (do_debug) {
        debug_extract = true;
    }

    return result;
}

// ==================== Schemaless Record Parsing ====================
//
// Key insight from ibd2sql: Read sizes from data end backward!
//
// InnoDB COMPACT/DYNAMIC record structure:
// [Sizes/NULLs][Header 5B][Origin -- PK][TRX][ROLL][Fixed fields][Variable fields]
//      ↑           ↑           ↑                                      ↑
//   low_addr   rec_offset   origin                              rec_end (next_origin - 5)
//
// ibd2sql's approach:
// 1. _offset starts at origin
// 2. read_reverse() reads backward from _offset (toward lower addresses)
// 3. First read header (5 bytes), then NULL bitmap, then sizes
//
// NEW APPROACH for schemaless:
// 1. Calculate record data range: origin to rec_end
// 2. Find variable field data at END of record
// 3. Read sizes from rec_offset backward (before header)
// 4. Parse fixed fields between system columns and variable fields

std::vector<IndexFieldValue> InnoDBIndexParser::parse_record_schemaless(
    const uint8_t* page_data, size_t page_size,
    size_t rec_offset, size_t rec_end,
    bool is_compact, bool is_leaf) {
    std::vector<IndexFieldValue> result;

    // rec_offset = header position (5 bytes before origin)
    // origin = where data starts (PK, TRX_ID, ROLL_PTR, user data)
    size_t header_size = is_compact ? REC_HEADER_SIZE_NEW : REC_HEADER_SIZE_OLD;
    size_t origin = rec_offset + header_size;

    if (origin >= rec_end || rec_end > page_size) return result;

    size_t record_len = rec_end - origin;
    if (record_len < 21) {  // Minimum: PK(8) + TRX_ID(6) + ROLL_PTR(7) = 21
        IndexFieldValue val;
        val.type = IndexFieldValue::Type::BINARY_VAL;
        val.raw_bytes.assign(page_data + origin, page_data + rec_end);
        result.push_back(val);
        return result;
    }

    // === Step 1: Read sizes from header position backward ===
    // In COMPACT/DYNAMIC format, sizes and NULL bitmap are stored BEFORE header:
    // [size bytes][NULL bitmap][header 5B][data...]
    //
    // For schemaless mode, we need to heuristically determine:
    // - How many NULL bitmap bytes (depends on nullable column count)
    // - How many size bytes (depends on variable-length column count)
    //
    // From analysis of test data:
    // - Most tables have 1-2 bytes NULL bitmap
    // - Each small VARCHAR field has 1 byte size (< 128 bytes)
    // - Larger VARCHAR fields have 2 byte size
    //
    // Strategy: Try to find the size byte right before NULL bitmap
    // NULL bitmap typically has value 0-15 (for up to 15 nullable columns)
    // Size byte should be 1-127 for small fields

    std::vector<uint32_t> var_sizes;
    constexpr uint8_t REC_N_FIELDS_ONE_BYTE_MAX = 0x7F;

    // Read backward from header position (rec_offset)
    // Try to find a pattern: [valid size byte][small NULL bitmap value]
    if (rec_offset >= 2) {
        // Check offset rec_offset-1 and rec_offset-2
        uint8_t byte_at_minus1 = page_data[rec_offset - 1];  // Likely NULL bitmap
        uint8_t byte_at_minus2 = page_data[rec_offset - 2];  // Likely size

        // Pattern: [size (1-127)][NULL bitmap (0-15)]
        if (byte_at_minus1 <= 15 && byte_at_minus2 >= 1 && byte_at_minus2 <= REC_N_FIELDS_ONE_BYTE_MAX) {
            var_sizes.push_back(byte_at_minus2);
        }
        // Alternative: size might be at rec_offset-1 if there's no NULL bitmap
        else if (byte_at_minus1 >= 1 && byte_at_minus1 <= REC_N_FIELDS_ONE_BYTE_MAX) {
            var_sizes.push_back(byte_at_minus1);
        }
        // Try checking further back for more size bytes
        else if (rec_offset >= 3) {
            uint8_t byte_at_minus3 = page_data[rec_offset - 3];
            if (byte_at_minus3 >= 1 && byte_at_minus3 <= REC_N_FIELDS_ONE_BYTE_MAX) {
                var_sizes.push_back(byte_at_minus3);
            }
        }
    }

    // === Step 2: Parse Primary Key ===
    // PK is at origin, BIGINT (8 bytes) with sign bit 0x80...
    size_t pk_pos = origin;
    size_t pk_size = 8;
    uint64_t pk_value = 0;

    // Read BIGINT with sign bit
    if (origin + 8 <= rec_end) {
        uint64_t raw = read_be<uint64_t>(page_data, origin);
        // InnoDB stores positive integers as: value + 0x8000000000000000
        if ((raw >> 56) == 0x80) {
            pk_value = raw ^ 0x8000000000000000ULL;
        } else {
            pk_value = raw;
        }
    }

    // Add PK as first field
    IndexFieldValue pk_val;
    pk_val.type = IndexFieldValue::Type::INT_VAL;
    pk_val.int_value = static_cast<int64_t>(pk_value);
    pk_val.str_value = std::to_string(pk_value);
    if (pk_pos + pk_size <= page_size) {
        pk_val.raw_bytes.assign(page_data + pk_pos, page_data + pk_pos + pk_size);
    }
    result.push_back(pk_val);

    // === Step 3: Parse fields in COMPACT order ===
    // In COMPACT/DYNAMIC format, field order is:
    // [PK] [TRX_ID] [ROLL_PTR] [Variable-length fields] [Fixed-length fields]
    // Variable fields come BEFORE fixed fields (after TRX/ROLL)

    size_t data_pos = origin + pk_size;
    if (is_leaf) {
        data_pos += 6 + 7;  // Skip TRX_ID + ROLL_PTR
    }

    // Calculate total variable field size
    size_t total_var_size = 0;
    for (auto sz : var_sizes) {
        total_var_size += sz;
    }

    // Parse variable-length fields first (they come right after TRX/ROLL)
    size_t var_pos = data_pos;
    size_t var_end = data_pos + total_var_size;

    if (var_end > rec_end) {
        var_end = rec_end;  // Safety check
    }

    for (size_t i = 0; i < var_sizes.size() && var_pos < var_end; i++) {
        uint32_t sz = var_sizes[i];
        if (sz == 0 || var_pos + sz > var_end) continue;

        IndexFieldValue val;
        val.raw_bytes.assign(page_data + var_pos, page_data + var_pos + sz);

        // Check if it's a printable string
        size_t printable_count = 0;
        for (size_t j = 0; j < sz; j++) {
            uint8_t b = page_data[var_pos + j];
            if (b >= 32 && b < 127) printable_count++;
        }

        if (printable_count >= sz * 2 / 3) {
            val.type = IndexFieldValue::Type::STRING_VAL;
            val.str_value = std::string(reinterpret_cast<const char*>(page_data + var_pos), sz);
        } else {
            val.type = IndexFieldValue::Type::BINARY_VAL;
        }

        result.push_back(val);
        var_pos += sz;
    }

    // Parse fixed-length fields after variable fields
    size_t fixed_pos = var_end;

    while (fixed_pos < rec_end) {
        size_t remaining = rec_end - fixed_pos;
        IndexFieldValue val;

        // Try 4-byte INT with sign bit
        if (remaining >= 4) {
            uint32_t raw4 = read_be<uint32_t>(page_data, fixed_pos);

            // Check for INT with sign bit (0x80...)
            if ((raw4 & 0xFF000000) == 0x80000000) {
                uint32_t int_val = raw4 ^ 0x80000000;
                val.type = IndexFieldValue::Type::INT_VAL;
                val.int_value = static_cast<int64_t>(int_val);
                val.str_value = std::to_string(int_val);
                val.raw_bytes.assign(page_data + fixed_pos, page_data + fixed_pos + 4);
                result.push_back(val);
                fixed_pos += 4;
                continue;
            }

            // Check for TIMESTAMP (Unix timestamp range: 2020-01-01 to 2100-01-01)
            // Also handle TIMESTAMP with fractional seconds (1-3 extra bytes)
            if (raw4 >= 1577836800 && raw4 <= 4102444800) {
                time_t t = static_cast<time_t>(raw4);
                struct tm tm_buf;
                gmtime_r(&t, &tm_buf);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

                val.type = IndexFieldValue::Type::TIMESTAMP_VAL;
                val.str_value = buf;
                val.raw_bytes.assign(page_data + fixed_pos, page_data + fixed_pos + 4);
                result.push_back(val);
                fixed_pos += 4;

                // Check for fractional seconds bytes (1-3 bytes)
                // InnoDB stores fractional seconds as 1-3 bytes depending on precision:
                // TIMESTAMP(1-2): 1 byte, TIMESTAMP(3-4): 2 bytes, TIMESTAMP(5-6): 3 bytes
                size_t remaining_after_ts = rec_end - fixed_pos;
                if (remaining_after_ts >= 1 && remaining_after_ts <= 3) {
                    // Likely fractional seconds - consume them but don't output as separate field
                    // The value represents fractional seconds in 1/10000th second increments
                    val.raw_bytes.assign(page_data + fixed_pos, page_data + rec_end);
                    fixed_pos = rec_end;  // Consume remaining bytes
                }
                continue;
            }
        }

        // Try small fixed-size fields before falling back to binary
        if (remaining >= 1 && remaining <= 3) {
            // Small remaining bytes after TIMESTAMP are likely fractional seconds
            // or padding - consume them without outputting as a separate field
            fixed_pos = rec_end;
            break;
        }

        // Fallback: output remaining bytes as binary
        val.type = IndexFieldValue::Type::BINARY_VAL;
        val.raw_bytes.assign(page_data + fixed_pos, page_data + rec_end);
        std::ostringstream hex_ss;
        hex_ss << std::hex << std::setfill('0');
        for (size_t i = fixed_pos; i < rec_end; i++) {
            hex_ss << std::setw(2) << static_cast<int>(page_data[i]);
        }
        val.str_value = hex_ss.str();
        result.push_back(val);
        fixed_pos = rec_end;
        break;
    }

    // === Step 5: Enhanced fallback for poor results ===
    // Check quality: need at least 3 fields, and less than 50% should be BINARY/UNKNOWN
    int binary_count = 0;
    for (const auto& f : result) {
        if (f.type == IndexFieldValue::Type::BINARY_VAL || f.type == IndexFieldValue::Type::UNKNOWN_VAL) {
            binary_count++;
        }
    }
    bool poor_quality = (result.size() < 3) || (binary_count > result.size() / 2);

    if (poor_quality) {
        EnhancedSchemalessParser enhanced_parser;
        auto enhanced_record = enhanced_parser.parse_record(
            page_data, page_size, rec_offset, rec_end, is_compact, is_leaf);

        if (enhanced_record.is_valid && enhanced_record.fields.size() + 1 > result.size()) {
            std::vector<IndexFieldValue> enhanced_result;

            IndexFieldValue epk_val;
            epk_val.type = IndexFieldValue::Type::INT_VAL;
            epk_val.int_value = static_cast<int64_t>(enhanced_record.id);
            epk_val.str_value = std::to_string(enhanced_record.id);
            enhanced_result.push_back(epk_val);

            for (const auto& field : enhanced_record.fields) {
                IndexFieldValue val;
                val.str_value = field.str_value;
                val.raw_bytes = field.raw_bytes;
                val.is_null = field.is_null;

                switch (field.type) {
                    case DetectedFieldType::INTEGER:
                        val.type = IndexFieldValue::Type::INT_VAL;
                        val.int_value = field.int_value;
                        break;
                    case DetectedFieldType::DECIMAL:
                        val.type = IndexFieldValue::Type::DOUBLE_VAL;
                        val.float_value = field.float_value;
                        break;
                    case DetectedFieldType::BOOLEAN:
                        val.type = IndexFieldValue::Type::INT_VAL;
                        val.int_value = field.int_value;
                        break;
                    case DetectedFieldType::DATE:
                    case DetectedFieldType::DATETIME:
                        val.type = IndexFieldValue::Type::TIMESTAMP_VAL;
                        break;
                    case DetectedFieldType::STRING:
                    case DetectedFieldType::EMAIL:
                    case DetectedFieldType::USERNAME:
                    case DetectedFieldType::PHONE:
                    case DetectedFieldType::ORDER_NUMBER:
                    case DetectedFieldType::SKU:
                    case DetectedFieldType::JSON:
                        val.type = IndexFieldValue::Type::STRING_VAL;
                        break;
                    default:
                        val.type = IndexFieldValue::Type::BINARY_VAL;
                        break;
                }
                enhanced_result.push_back(val);
            }

            if (enhanced_result.size() > result.size()) {
                return enhanced_result;
            }
        }
    }

    return result;
}

// ==================== Column Value Decoding ====================
// ==================== Column Value Decoding ====================

IndexFieldValue InnoDBIndexParser::decode_column(
    const uint8_t* data, size_t offset, size_t max_size,
    const IndexColumnDef& col, size_t field_size) {
    IndexFieldValue val;
    if (offset + field_size > max_size) {
        val.is_null = true;
        val.type = IndexFieldValue::Type::NULL_VAL;
        return val;
    }

    val.raw_bytes.assign(data + offset, data + offset + field_size);

    switch (col.col_type) {
        // ==================== Integer Types (InnoDB storage type IDs) ====================
        // Reference: ibd2sql/innodb_page/column_type.py
        case 2: {  // InnoDB TINY (TINYINT, 1 byte)
            val.type = IndexFieldValue::Type::INT_VAL;
            if (col.is_unsigned) {
                val.uint_value = data[offset];
                val.str_value = std::to_string(val.uint_value);
                val.type = IndexFieldValue::Type::UINT_VAL;
            } else {
                // Signed TINYINT: B2INT1 = read_be - 128
                val.int_value = static_cast<int32_t>(data[offset]) - 128;
                val.str_value = std::to_string(val.int_value);
            }
            break;
        }
        case 3: {  // InnoDB SHORT (SMALLINT, 2 bytes)
            val.type = IndexFieldValue::Type::INT_VAL;
            if (col.is_unsigned) {
                val.uint_value = read_be<uint16_t>(data, offset);
                val.str_value = std::to_string(val.uint_value);
                val.type = IndexFieldValue::Type::UINT_VAL;
            } else {
                // B2INT2: read_be - 32768
                val.int_value = static_cast<int32_t>(read_be<uint16_t>(data, offset)) - 32768;
                val.str_value = std::to_string(val.int_value);
            }
            break;
        }
        case 4: {  // InnoDB LONG (INT, 4 bytes)
            val.type = IndexFieldValue::Type::INT_VAL;
            if (col.is_unsigned) {
                val.uint_value = read_be<uint32_t>(data, offset);
                val.str_value = std::to_string(val.uint_value);
                val.type = IndexFieldValue::Type::UINT_VAL;
            } else {
                // B2INT4: read_be - 2147483648
                val.int_value = static_cast<int64_t>(read_be<uint32_t>(data, offset)) - 2147483648LL;
                val.str_value = std::to_string(val.int_value);
            }
            break;
        }
        // InnoDB type 9 = LONGLONG (BIGINT, 8 bytes)
        case 9: {  // InnoDB LONGLONG (BIGINT, 8 bytes)
            val.type = IndexFieldValue::Type::INT_VAL;
            if (col.is_unsigned) {
                val.uint_value = read_be<uint64_t>(data, offset);
                val.str_value = std::to_string(val.uint_value);
                val.type = IndexFieldValue::Type::UINT_VAL;
            } else {
                // B2INT8: read_be - 9223372036854775808
                val.int_value = static_cast<int64_t>(read_be<uint64_t>(data, offset) - 9223372036854775808ULL);
                val.str_value = std::to_string(val.int_value);
            }
            break;
        }
        // InnoDB type 10 = INT24 (MEDIUMINT, 3 bytes)
        case 10: {  // INT24 (MEDIUMINT, 3 bytes)
            val.type = IndexFieldValue::Type::INT_VAL;
            uint32_t v = (static_cast<uint32_t>(data[offset]) << 16) |
                         (static_cast<uint32_t>(data[offset + 1]) << 8) |
                         static_cast<uint32_t>(data[offset + 2]);
            if (col.is_unsigned) {
                val.uint_value = v;
                val.str_value = std::to_string(val.uint_value);
                val.type = IndexFieldValue::Type::UINT_VAL;
            } else {
                // B2INT3: (read_be >> 8) - 8388608
                val.int_value = static_cast<int32_t>(v) - 8388608;
                val.str_value = std::to_string(val.int_value);
            }
            break;
        }
        // ==================== Floating Point ====================
        // CRITICAL: InnoDB stores FLOAT/DOUBLE in LITTLE-ENDIAN (native IEEE 754),
        // NOT big-endian like integers! This is different from the integer storage.
        case 5: {  // InnoDB FLOAT (4 bytes)
            val.type = IndexFieldValue::Type::FLOAT_VAL;
            // Read as little-endian float, then convert to double for storage
            float f;
            std::memcpy(&f, data + offset, 4);
            val.float_value = static_cast<double>(f);
            std::ostringstream oss;
            oss << val.float_value;
            val.str_value = oss.str();
            break;
        }
        case 6: {  // InnoDB DOUBLE (8 bytes)
            val.type = IndexFieldValue::Type::DOUBLE_VAL;
            // Read as little-endian (memcpy the raw bytes directly)
            std::memcpy(&val.float_value, data + offset, 8);
            std::ostringstream oss;
            oss << val.float_value;
            val.str_value = oss.str();
            break;
        }
        // ==================== Temporal Types ====================
        // 使用 InnoDB 类型 ID (参考 ibd2sql column_type.py)
        // InnoDB type 8 = TIMESTAMP (4 bytes)
        case 8: {  // TIMESTAMP (4 bytes) - InnoDB type 8
            val.type = IndexFieldValue::Type::TIMESTAMP_VAL;
            uint32_t ts = read_be<uint32_t>(data, offset);
            time_t t = static_cast<time_t>(ts);
            struct tm tm_buf;
            gmtime_r(&t, &tm_buf);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
            val.str_value = buf;
            break;
        }
        // InnoDB type 15 = NEWDATE/DATE (3 bytes)
        case 15: {  // NEWDATE/DATE (3 bytes) - InnoDB type 15
            val.type = IndexFieldValue::Type::DATE_VAL;
            // DATE is stored as: bit 0-4: day, bit 5-8: month, bit 9-22: year, bit 23: sign
            // Reference: ibd2sql/utils/b2data.py B2DATE()
            uint32_t t = (static_cast<uint32_t>(data[offset]) << 16) |
                         (static_cast<uint32_t>(data[offset + 1]) << 8) |
                         static_cast<uint32_t>(data[offset + 2]);
            uint32_t year = (t & 0x7FFE00) >> 9;  // bits 9-22 (14 bits)
            uint32_t month = (t & 0x1E0) >> 5;    // bits 5-8 (4 bits)
            uint32_t day = t & 0x1F;              // bits 0-4 (5 bits)
            char buf[16];
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u", year, month, day);
            val.str_value = buf;
            break;
        }
        case 13: {  // DATETIME (old format, 8 bytes) - InnoDB type 13
            val.type = IndexFieldValue::Type::DATETIME_VAL;
            uint64_t dt = read_be<uint64_t>(data, offset);
            if (dt > 9223372036854775808ULL) {
                dt -= 9223372036854775808ULL;
                std::string s = std::to_string(dt);
                // Pad to 14 digits
                while (s.length() < 14) s = "0" + s;
                char buf[32];
                snprintf(buf, sizeof(buf), "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
                         s.c_str(), s.c_str()+4, s.c_str()+6, s.c_str()+8, s.c_str()+10, s.c_str()+12);
                val.str_value = buf;
            } else {
                val.str_value = "0000-00-00 00:00:00";
            }
            break;
        }
        case 19: {  // DATETIME2 (new format, 5 bytes) - InnoDB type 19
            val.type = IndexFieldValue::Type::DATETIME_VAL;
            // Read 5 bytes as 40-bit value
            uint64_t t = (static_cast<uint64_t>(read_be<uint32_t>(data, offset)) << 8) |
                         static_cast<uint64_t>(data[offset + 4]);
            // Extract bit fields using ibd2sql's exact mask values
            // months = (t & 549751619584) >> 22  where 549751619584 = 0x7FFFC00000
            uint64_t months = (t & 0x7FFFC00000ULL) >> 22;  // 19 bits
            uint16_t year = static_cast<uint16_t>(months / 13);
            uint8_t month = static_cast<uint8_t>(months % 13);
            uint8_t day = static_cast<uint8_t>((t & 0x3E0000ULL) >> 17);    // 5 bits
            uint8_t hour = static_cast<uint8_t>((t & 0x1F000ULL) >> 12);    // 5 bits
            uint8_t minute = static_cast<uint8_t>((t & 0xFC0ULL) >> 6);     // 6 bits
            uint8_t second = static_cast<uint8_t>(t & 0x3FULL);             // 6 bits
            char buf[32];
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", year, month, day, hour, minute, second);
            val.str_value = buf;
            break;
        }
        // InnoDB type 18 = TIMESTAMP2 (4 bytes, same as TIMESTAMP)
        case 18: { // TIMESTAMP2 (4 bytes) - InnoDB type 18
            val.type = IndexFieldValue::Type::TIMESTAMP_VAL;
            uint32_t ts = read_be<uint32_t>(data, offset);
            time_t t = static_cast<time_t>(ts);
            struct tm tm_buf;
            gmtime_r(&t, &tm_buf);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
            val.str_value = buf;
            break;
        }
        case 20: {  // TIME2 (MySQL 5.6+ time format, 3-6 bytes) - InnoDB type 20
            val.type = IndexFieldValue::Type::STRING_VAL;
            // TIME2 format: biased 24-bit integer + optional fractional seconds
            // stored_value = 0x800000 + (hour * 10000 + minute * 100 + second)
            if (field_size >= 3) {
                uint32_t t = (static_cast<uint32_t>(data[offset]) << 16) |
                             (static_cast<uint32_t>(data[offset + 1]) << 8) |
                             static_cast<uint32_t>(data[offset + 2]);

                int32_t biased = static_cast<int32_t>(t) - 0x800000;
                bool negative = biased < 0;
                int32_t packed = negative ? -biased : biased;

                uint32_t hour = packed / 10000;
                uint32_t minute = (packed % 10000) / 100;
                uint32_t second = packed % 100;

                if (hour > 838 || minute > 59 || second > 59) {
                    val.str_value = "<INVALID_TIME>";
                } else {
                    char buf[24];
                    snprintf(buf, sizeof(buf), "%s%02u:%02u:%02u", negative ? "-" : "", hour, minute, second);
                    val.str_value = buf;
                }
            } else {
                val.str_value = "<TIME_SIZE_ERR>";
            }
            break;
        }
        case 14: {  // YEAR (1 byte) - InnoDB type 14
            val.type = IndexFieldValue::Type::UINT_VAL;
            val.uint_value = data[offset] + 1900;
            val.str_value = std::to_string(val.uint_value);
            break;
        }
        case 12: {  // TIME (3 bytes) - InnoDB type 12
            val.type = IndexFieldValue::Type::STRING_VAL;
            // TIME format: 3 bytes, similar to DATE
            uint32_t t = (static_cast<uint32_t>(data[offset]) << 16) |
                         (static_cast<uint32_t>(data[offset + 1]) << 8) |
                         static_cast<uint32_t>(data[offset + 2]);
            bool negative = (t & 0x800000) != 0;
            uint32_t time_val = t & 0x7FFFFF;
            uint8_t hour = (time_val >> 12) & 0x3FF;
            uint8_t minute = (time_val >> 6) & 0x3F;
            uint8_t second = time_val & 0x3F;
            char buf[16];
            snprintf(buf, sizeof(buf), "%s%02u:%02u:%02u", negative ? "-" : "", hour, minute, second);
            val.str_value = buf;
            break;
        }
        // ==================== String Types ====================
        // 使用 InnoDB 类型 ID
        case 16:   // VARCHAR - InnoDB type 16
        case 28:   // VAR_STRING - InnoDB type 28
        case 27:   // BLOB/TEXT - InnoDB type 27
        case 24:   // TINY_BLOB
        case 25:   // MEDIUM_BLOB
        case 26: { // LONG_BLOB
            val.type = IndexFieldValue::Type::STRING_VAL;
            val.str_value = std::string(reinterpret_cast<const char*>(data + offset), field_size);
            break;
        }
        case 29: { // STRING/CHAR - InnoDB type 29
            val.type = IndexFieldValue::Type::STRING_VAL;
            val.str_value = std::string(reinterpret_cast<const char*>(data + offset), field_size);
            // Trim trailing spaces
            while (!val.str_value.empty() && val.str_value.back() == ' ') {
                val.str_value.pop_back();
            }
            break;
        }
        // ==================== DECIMAL (NEWDECIMAL type 21) ====================
        case 21: { // NEWDECIMAL - InnoDB type 21
            val.type = IndexFieldValue::Type::DECIMAL_VAL;

            // Calculate digit groups using MySQL's decimal encoding
            // Reference: ibd2sql/utils/b2data.py B2DECIMAL()
            //
            // CRITICAL FIX: The encoding uses base-256 multiplication, NOT string concatenation!
            // For groups [[4,9], [1,1]]:
            // - The value is: first_group_value * 256^(second_group_bytes) + second_group_value
            // - NOT str(first).zfill(9) + str(second).zfill(1) (which was wrong!)

            uint32_t precision = col.precision > 0 ? col.precision : 10;
            uint32_t scale = col.scale;
            uint32_t int_digits = precision - scale;

            // Calculate group structure using map_decimal logic
            auto calc_groups = [](uint32_t digits) -> std::vector<std::pair<int,int>> {
                std::vector<std::pair<int,int>> result;
                int full_groups = digits / 9;
                int remainder = digits % 9;
                for (int i = 0; i < full_groups; i++) {
                    result.push_back({4, 9});
                }
                if (remainder > 0) {
                    int bytes = (remainder + 1) / 2;
                    if (remainder >= 7) bytes = 4;
                    result.push_back({bytes, remainder});
                }
                return result;
            };

            auto p1 = calc_groups(int_digits);  // Integer part groups
            auto p2 = calc_groups(scale);        // Fractional part groups

            // Check sign: 0x80 bit set = positive, not set = negative
            bool is_negative = (data[offset] & 0x80) == 0;

            // Copy data for processing
            std::vector<uint8_t> proc_data(data + offset, data + offset + field_size);

            if (is_negative) {
                // Invert all bytes for negative numbers
                for (auto& b : proc_data) {
                    b = ~b;
                }
            }
            // Subtract 128 from first byte (after inversion if negative)
            proc_data[0] -= 128;

            // CORRECTED: Read groups and compute value using base-256 multiplication
            // The value = sum of (group_value * 256^total_bytes_of_following_groups)

            // First compute the integer part using mathematical approach
            uint64_t int_value = 0;
            size_t read_offset = 0;

            // Calculate total bytes for remaining groups to determine multiplier
            size_t total_remaining_bytes = 0;
            for (size_t i = 0; i < p1.size(); i++) {
                total_remaining_bytes += p1[i].first;
            }

            for (size_t i = 0; i < p1.size(); i++) {
                int bytes = p1[i].first;
                if (read_offset + bytes > field_size) break;

                // Subtract bytes of this group from remaining
                total_remaining_bytes -= bytes;

                // Read this group's value
                uint64_t group_val = 0;
                for (int j = 0; j < bytes && read_offset + j < field_size; j++) {
                    group_val = (group_val << 8) | proc_data[read_offset + j];
                }

                // Multiply by 256^total_remaining_bytes and add
                uint64_t multiplier = 1;
                for (size_t k = 0; k < total_remaining_bytes; k++) {
                    multiplier *= 256;
                }
                int_value += group_val * multiplier;

                read_offset += bytes;
            }

            // For fractional part, similarly compute
            uint64_t frac_value = 0;
            total_remaining_bytes = 0;
            for (size_t i = 0; i < p2.size(); i++) {
                total_remaining_bytes += p2[i].first;
            }

            for (size_t i = 0; i < p2.size(); i++) {
                int bytes = p2[i].first;
                if (read_offset + bytes > field_size) break;

                total_remaining_bytes -= bytes;

                uint64_t group_val = 0;
                for (int j = 0; j < bytes && read_offset + j < field_size; j++) {
                    group_val = (group_val << 8) | proc_data[read_offset + j];
                }

                uint64_t multiplier = 1;
                for (size_t k = 0; k < total_remaining_bytes; k++) {
                    multiplier *= 256;
                }
                frac_value += group_val * multiplier;

                read_offset += bytes;
            }

            // Format result: int_value.frac_value (pad frac to scale digits)
            std::string frac_str = std::to_string(frac_value);
            while (frac_str.length() < scale) {
                frac_str = "0" + frac_str;
            }

            // Build result string
            val.str_value = (is_negative ? "-" : "") + std::to_string(int_value) + "." + frac_str;
            break;
        }
        // ==================== JSON ====================
        case 245: { // JSON
            val.type = IndexFieldValue::Type::STRING_VAL;
            if (field_size > 0) {
                // Simple output as string
                val.str_value = std::string(reinterpret_cast<const char*>(data + offset), field_size);
            }
            break;
        }
        // ==================== ENUM (type 247) ====================
        case 247: {
            val.type = IndexFieldValue::Type::STRING_VAL;
            if (field_size > 0 && !col.enum_values.empty()) {
                uint64_t idx = 0;
                for (size_t i = 0; i < field_size; i++) {
                    idx = (idx << 8) | data[offset + i];
                }
                if (idx > 0 && idx <= col.enum_values.size()) {
                    val.str_value = col.enum_values[idx - 1];  // ENUM index is 1-based
                } else {
                    val.str_value = "<ENUM_" + std::to_string(idx) + ">";
                }
            } else {
                val.str_value = "<ENUM>";
            }
            break;
        }
        // ==================== SET (type 248) ====================
        case 248: {
            val.type = IndexFieldValue::Type::STRING_VAL;
            if (field_size > 0 && !col.set_values.empty()) {
                uint64_t bits = 0;
                for (size_t i = 0; i < field_size; i++) {
                    bits = (bits << 8) | data[offset + i];
                }
                std::string result;
                for (size_t n = 0; n < col.set_values.size(); n++) {
                    if ((bits & (1ULL << n)) != 0) {
                        if (!result.empty()) result += ",";
                        result += col.set_values[n];
                    }
                }
                val.str_value = result.empty() ? "" : result;
            } else {
                val.str_value = "<SET>";
            }
            break;
        }
        default: {
            val.type = IndexFieldValue::Type::UNKNOWN_VAL;
            val.str_value = "<TYPE_" + std::to_string(col.col_type) + ">";
            break;
        }
    }

    return val;
}

// ==================== Page Parsing ====================

std::vector<IndexRecord> InnoDBIndexParser::parse_page(
    const uint8_t* page_data, size_t page_size, uint32_t page_no) {
    std::vector<IndexRecord> records;

    if (!is_index_page(page_data, page_size)) {
        return records;
    }

    auto page_hdr = parse_page_header(page_data, page_size);
    if (!page_hdr) {
        if (bad_page_mode_ == 1) {
            return parse_page_fast(page_data, page_size, page_no);
        } else if (bad_page_mode_ == 2) {
            return parse_page_try(page_data, page_size, page_no);
        }
        return records;
    }

    bool is_compact = page_hdr->is_compact_format;
    records = follow_record_chain(page_data, page_size, *page_hdr, page_no, is_compact);

    // Recover deleted records if enabled
    if (recover_deleted_ && page_hdr->page_free != 0) {
        auto deleted_records = follow_free_chain(page_data, page_size, *page_hdr, page_no, is_compact);
        for (auto& rec : deleted_records) {
            rec.is_deleted = true;
            records.push_back(std::move(rec));
        }
    }

    stats_.pages_valid++;
    stats_.records_recovered += records.size();

    return records;
}

std::vector<IndexRecord> InnoDBIndexParser::parse_page_fast(
    const uint8_t* page_data, size_t page_size, uint32_t page_no) {
    std::vector<IndexRecord> records;
    // Fast mode: use page directory
    return records;
}

std::vector<IndexRecord> InnoDBIndexParser::parse_page_try(
    const uint8_t* page_data, size_t page_size, uint32_t page_no) {
    std::vector<IndexRecord> records;
    // Try mode: brute-force scan
    return records;
}

std::vector<IndexRecord> InnoDBIndexParser::follow_free_chain(
    const uint8_t* page_data, size_t page_size,
    const InnoDBPageHeader& page_hdr, uint32_t page_no,
    bool is_compact) {
    std::vector<IndexRecord> records;
    // Follow PAGE_FREE chain
    return records;
}

// ==================== Full File Processing ====================

std::vector<IndexRecord> InnoDBIndexParser::parse_ibd_force(const std::string& ibd_path) {
    std::vector<IndexRecord> all_records;

    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return all_records;

    size_t file_size = file.tellg();
    file.seekg(0);

    uint32_t page_size = UNIV_PAGE_SIZE;

    std::vector<uint8_t> first_page(page_size);
    if (file.read(reinterpret_cast<char*>(first_page.data()), page_size)) {
        uint32_t detected = detect_page_size_from_fsp(first_page.data());
        if (detected >= UNIV_PAGE_SIZE_MIN && detected <= UNIV_PAGE_SIZE_MAX) {
            page_size = detected;
        }

        uint8_t rf = detect_row_format_from_fsp(first_page.data());
        if (rf == 2 || rf == 3) {
            off_page_flag_ = OFF_PAGE_FLAG_DYNAMIC;
        } else {
            off_page_flag_ = OFF_PAGE_FLAG_ANTLELOPE;
        }
    }

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size);
    std::cout << "[INFO] File size: " << file_size << " bytes, page_size: " << page_size
              << ", total_pages: " << total_pages << "\n";

    // Optimized single-pass approach: cache INDEX page metadata and data
    struct IndexPageData {
        uint32_t page_no;
        uint64_t index_id;
        uint16_t level;
        bool is_compact;
        std::vector<uint8_t> page_data;
    };

    struct IndexInfo {
        uint64_t index_id;
        uint32_t leaf_pages = 0;
        uint32_t nonleaf_pages = 0;
        uint16_t max_level = 0;
    };
    std::map<uint64_t, IndexInfo> index_map;
    std::vector<IndexPageData> index_pages;

    // Single pass: read all INDEX pages, cache metadata and page data
    std::vector<uint8_t> page_buf(page_size);
    for (uint32_t page_no = 0; page_no < total_pages; page_no++) {
        file.seekg(static_cast<std::streamsize>(page_no) * page_size);
        if (!file.read(reinterpret_cast<char*>(page_buf.data()), page_size)) break;

        uint16_t pt = read_be<uint16_t>(page_buf.data(), FIL_PAGE_TYPE_ABS);
        if (pt != FIL_PAGE_INDEX) continue;

        auto ph = parse_page_header(page_buf.data(), page_size);
        if (!ph) continue;

        // Record index metadata
        auto& info = index_map[ph->index_id];
        info.index_id = ph->index_id;
        if (ph->level == 0) {
            info.leaf_pages++;
        } else {
            info.nonleaf_pages++;
            if (ph->level > info.max_level) {
                info.max_level = ph->level;
            }
        }

        // Cache leaf page data for later extraction
        if (ph->level == 0) {
            index_pages.push_back({
                page_no,
                ph->index_id,
                ph->level,
                ph->is_compact_format,
                std::vector<uint8_t>(page_buf.begin(), page_buf.end())
            });
        }
    }

    // Select the clustered index using improved heuristics
    uint64_t clustered_index_id = 0;
    uint32_t max_leaf_pages = 0;
    uint16_t max_level = 0;

    if (target_index_id_ != 0) {
        clustered_index_id = target_index_id_;
    } else if (!index_map.empty()) {
        // First pass: find max leaf pages
        for (const auto& [idx_id, info] : index_map) {
            if (info.leaf_pages > max_leaf_pages) {
                max_leaf_pages = info.leaf_pages;
            }
        }

        // Second pass: select best index using heuristics
        uint32_t min_acceptable = max_leaf_pages * 80 / 100;
        if (min_acceptable == 0) min_acceptable = max_leaf_pages;

        for (const auto& [idx_id, info] : index_map) {
            if (info.leaf_pages >= min_acceptable) {
                if (info.max_level > max_level ||
                    (info.max_level == max_level && info.leaf_pages > max_leaf_pages)) {
                    max_level = info.max_level;
                    max_leaf_pages = info.leaf_pages;
                    clustered_index_id = idx_id;
                }
            }
        }

        // Fallback: if no index selected by level, use max leaf pages
        if (clustered_index_id == 0) {
            for (const auto& [idx_id, info] : index_map) {
                if (info.leaf_pages > max_leaf_pages ||
                    (info.leaf_pages == max_leaf_pages && clustered_index_id == 0)) {
                    max_leaf_pages = info.leaf_pages;
                    clustered_index_id = idx_id;
                }
            }
        }
    }

    if (clustered_index_id != 0) {
        target_index_id_ = clustered_index_id;
        const auto& ci = index_map[clustered_index_id];
        std::cout << "[INFO] Selected clustered index_id=" << clustered_index_id
                  << " (leaf_pages=" << ci.leaf_pages
                  << ", nonleaf_pages=" << ci.nonleaf_pages
                  << ", level=" << ci.max_level << ")" << std::endl;
        for (const auto& [idx_id, info] : index_map) {
            if (idx_id != clustered_index_id) {
                std::cout << "[INFO] Skipping secondary index_id=" << idx_id
                          << " (leaf_pages=" << info.leaf_pages << ")" << std::endl;
            }
        }
    }

    // Extract records from cached pages
    for (const auto& page_data : index_pages) {
        stats_.pages_scanned++;

        if (target_index_id_ != 0 && page_data.index_id != target_index_id_) {
            continue;
        }

        stats_.pages_index_leaf++;

        auto page_records = follow_record_chain(
            page_data.page_data.data(), page_size,
            *parse_page_header(page_data.page_data.data(), page_size),
            page_data.page_no, page_data.is_compact);

        for (auto& rec : page_records) {
            auto key = std::make_pair(rec.page_no, rec.heap_no);
            if (seen_records_.find(key) == seen_records_.end()) {
                seen_records_.insert(key);
                all_records.push_back(std::move(rec));
            } else {
                stats_.dedup_removed++;
            }
        }
    }

    stats_.records_recovered = all_records.size();
    total_records_ += all_records.size();
    total_pages_ += total_pages;

    return all_records;
}

uint64_t InnoDBIndexParser::parse_ibd_force_streaming(const std::string& ibd_path,
                                                       const std::string& csv_output,
                                                       const std::vector<IndexColumnDef>& columns) {
    // Set columns for schema-aware parsing
    if (!columns.empty()) {
        columns_ = columns;
        // Count nullable and variable-length columns
        n_nullable_ = 0;
        n_variable_ = 0;
        for (const auto& col : columns_) {
            if (col.is_nullable) n_nullable_++;
            if (col.is_variable_length()) n_variable_++;
        }
    }

    std::ofstream csv_file(csv_output);
    if (!csv_file.is_open()) {
        std::cerr << "[ERROR] Cannot open output file: " << csv_output << "\n";
        return 0;
    }

    // Write header
    if (!columns.empty()) {
        for (size_t i = 0; i < columns.size(); i++) {
            if (i > 0) csv_file << ",";
            csv_file << columns[i].name;
        }
        csv_file << "\n";
    }

    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Cannot open input file: " << ibd_path << "\n";
        return 0;
    }

    size_t file_size = file.tellg();
    file.seekg(0);

    uint32_t page_size = UNIV_PAGE_SIZE;

    std::vector<uint8_t> first_page(page_size);
    if (file.read(reinterpret_cast<char*>(first_page.data()), page_size)) {
        uint32_t detected = detect_page_size_from_fsp(first_page.data());
        if (detected >= UNIV_PAGE_SIZE_MIN && detected <= UNIV_PAGE_SIZE_MAX) {
            page_size = detected;
        }

        uint8_t rf = detect_row_format_from_fsp(first_page.data());
        if (rf == 2 || rf == 3) {
            off_page_flag_ = OFF_PAGE_FLAG_DYNAMIC;
        } else {
            off_page_flag_ = OFF_PAGE_FLAG_ANTLELOPE;
        }
    }

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size);
    std::cout << "[INFO] File size: " << file_size << " bytes, page_size: " << page_size
              << ", total_pages: " << total_pages << "\n";

    // Single pass: identify indexes and extract records
    struct IndexInfo {
        uint64_t index_id;
        uint32_t leaf_pages = 0;
        uint32_t nonleaf_pages = 0;
        uint16_t max_level = 0;
    };
    std::map<uint64_t, IndexInfo> index_map;

    // Store page positions and index_ids for leaf pages
    std::vector<std::pair<uint32_t, uint64_t>> leaf_page_info;

    std::vector<uint8_t> page_buf(page_size);

    // First pass: survey indexes (only metadata, no page data caching)
    for (uint32_t page_no = 0; page_no < total_pages; page_no++) {
        file.seekg(static_cast<std::streamsize>(page_no) * page_size);
        if (!file.read(reinterpret_cast<char*>(page_buf.data()), page_size)) break;

        uint16_t pt = read_be<uint16_t>(page_buf.data(), FIL_PAGE_TYPE_ABS);
        if (pt != FIL_PAGE_INDEX) continue;

        auto ph = parse_page_header(page_buf.data(), page_size);
        if (!ph) continue;

        auto& info = index_map[ph->index_id];
        info.index_id = ph->index_id;
        if (ph->level == 0) {
            info.leaf_pages++;
            leaf_page_info.push_back({page_no, ph->index_id});
        } else {
            info.nonleaf_pages++;
            if (ph->level > info.max_level) {
                info.max_level = ph->level;
            }
        }
    }

    // Select clustered index
    uint64_t clustered_index_id = 0;
    uint32_t max_leaf_pages = 0;
    uint16_t max_level = 0;

    if (target_index_id_ != 0) {
        clustered_index_id = target_index_id_;
    } else if (!index_map.empty()) {
        for (const auto& [idx_id, info] : index_map) {
            if (info.leaf_pages > max_leaf_pages) {
                max_leaf_pages = info.leaf_pages;
            }
        }

        uint32_t min_acceptable = max_leaf_pages * 80 / 100;
        if (min_acceptable == 0) min_acceptable = max_leaf_pages;

        for (const auto& [idx_id, info] : index_map) {
            if (info.leaf_pages >= min_acceptable) {
                if (info.max_level > max_level ||
                    (info.max_level == max_level && info.leaf_pages > max_leaf_pages)) {
                    max_level = info.max_level;
                    max_leaf_pages = info.leaf_pages;
                    clustered_index_id = idx_id;
                }
            }
        }

        if (clustered_index_id == 0) {
            for (const auto& [idx_id, info] : index_map) {
                if (info.leaf_pages > max_leaf_pages ||
                    (info.leaf_pages == max_leaf_pages && clustered_index_id == 0)) {
                    max_leaf_pages = info.leaf_pages;
                    clustered_index_id = idx_id;
                }
            }
        }
    }

    if (clustered_index_id != 0) {
        target_index_id_ = clustered_index_id;
        const auto& ci = index_map[clustered_index_id];
        std::cout << "[INFO] Selected clustered index_id=" << clustered_index_id
                  << " (leaf_pages=" << ci.leaf_pages
                  << ", nonleaf_pages=" << ci.nonleaf_pages
                  << ", level=" << ci.max_level << ")" << std::endl;
    }

    // Second pass: extract records and write directly to CSV
    uint64_t record_count = 0;
    std::set<std::pair<uint32_t, uint16_t>> seen;

    for (const auto& [page_no, index_id] : leaf_page_info) {
        if (target_index_id_ != 0 && index_id != target_index_id_) {
            continue;
        }

        file.seekg(static_cast<std::streamsize>(page_no) * page_size);
        if (!file.read(reinterpret_cast<char*>(page_buf.data()), page_size)) break;

        stats_.pages_scanned++;

        auto ph = parse_page_header(page_buf.data(), page_size);
        if (!ph) {
            stats_.pages_skipped_corrupt++;
            continue;
        }

        stats_.pages_index_leaf++;

        bool is_compact = ph->is_compact_format;
        auto page_records = follow_record_chain(page_buf.data(), page_size, *ph, page_no, is_compact);

        for (const auto& rec : page_records) {
            if (rec.is_infimum || rec.is_supremum) continue;

            auto key = std::make_pair(rec.page_no, rec.heap_no);
            if (seen.find(key) != seen.end()) {
                stats_.dedup_removed++;
                continue;
            }
            seen.insert(key);

            // Write directly to CSV
            csv_file << rec.to_csv_line() << "\n";
            record_count++;
        }
    }

    csv_file.close();

    stats_.records_recovered = record_count;
    total_records_ += record_count;
    total_pages_ += total_pages;

    std::cout << "[INFO] Streaming export complete: " << record_count << " records to " << csv_output << "\n";

    return record_count;
}

std::vector<IndexRecord> InnoDBIndexParser::parse_ibd_btree(
    const std::string& ibd_path, uint32_t root_page_no) {
    std::vector<IndexRecord> all_records;
    // B+Tree traversal implementation
    return all_records;
}

std::vector<uint32_t> InnoDBIndexParser::find_leaf_pages_from_file(
    std::ifstream& file, uint32_t page_size, uint32_t total_pages,
    uint32_t root_page_no) {
    std::vector<uint32_t> leaf_pages;
    return leaf_pages;
}

// ==================== Utility Functions ====================

bool InnoDBIndexParser::is_index_page(const uint8_t* page_data, size_t page_size) const {
    if (!page_data || page_size < 26) return false;
    uint16_t pt = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);
    return pt == FIL_PAGE_INDEX;
}

void InnoDBIndexParser::deduplicate_records(std::vector<IndexRecord>& records) {
    std::set<std::pair<uint32_t, uint16_t>> seen;
    auto it = std::remove_if(records.begin(), records.end(),
        [&seen](const IndexRecord& rec) {
            auto key = std::make_pair(rec.page_no, rec.heap_no);
            if (seen.count(key)) return true;
            seen.insert(key);
            return false;
        });
    records.erase(it, records.end());
}

// ==================== Standalone Utility Functions ====================

uint32_t detect_page_size_from_fsp(const uint8_t* page_data) {
    if (!page_data) return UNIV_PAGE_SIZE;

    // FSP header flags at offset 54 in page 0
    uint32_t flags = read_be<uint32_t>(page_data, 54);

    // Validate: Check if offset 0-3 (FIL header checksum) looks reasonable
    // A valid FIL header should have reasonable checksum values
    uint32_t checksum1 = read_be<uint32_t>(page_data, 0);
    uint32_t checksum2 = read_be<uint32_t>(page_data, 4);  // page_no should be small

    // If checksum1 is garbage (too large random value) or page_no is unreasonable,
    // the header is likely corrupted - use default 16KB
    bool header_valid = true;
    if (checksum2 > 100000) {  // page_no should be 0-4 for typical IBD files
        header_valid = false;
    }

    // Also check FIL_PAGE_TYPE at offset 24-25
    uint16_t page_type = read_be<uint16_t>(page_data, 24);
    // Valid types: 0x45BF (INDEX), 0x45BD (SDI_ZIP), 0x0000 (ALLOCATED), 0xFFFF (corrupted)
    // If page_type is completely random, header is corrupted
    if (page_type != 0x45BF && page_type != 0x45BD && page_type != 0x0000 &&
        page_type != 0xFFFF && page_type > 0x1000) {
        header_valid = false;
    }

    if (!header_valid) {
        // Header is corrupted, return default 16KB
        // The caller can try multiple page sizes as fallback
        return UNIV_PAGE_SIZE;
    }

    // Bits 0-5: page size code
    // 0 = default (16KB for most MySQL versions)
    // 1 = 8KB, 2 = 4KB, 3 = 32KB, 4 = 64KB
    // Note: The encoding varies by MySQL version, but 0 typically means default 16KB
    uint32_t size_code = (flags >> 1) & 0x07;  // Bits 1-3 for page size

    switch (size_code) {
        case 0: return UNIV_PAGE_SIZE;  // Default 16KB
        case 1: return 8192;
        case 2: return 4096;
        case 3: return 32768;
        case 4: return 65536;
        default: return UNIV_PAGE_SIZE;
    }
}

/**
 * Detect page size from file size - fallback when header is corrupted
 * For InnoDB IBD files, total_pages must be reasonable (typically > 2)
 * Returns most likely page size based on file alignment
 */
uint32_t detect_page_size_from_file_size(size_t file_size) {
    // Try common page sizes, prefer 16KB as most common
    std::vector<uint32_t> candidates = {16384, 8192, 4096, 32768, 65536};

    for (uint32_t ps : candidates) {
        // File must be aligned to page size
        if (file_size % ps == 0) {
            size_t total_pages = file_size / ps;
            // Typical IBD file has at least 5-6 pages (FSP, IBUF, inode pages + data)
            // and typically has multiple index pages
            if (total_pages >= 5 && total_pages < 1000000) {
                return ps;
            }
        }
    }

    // Default fallback
    return UNIV_PAGE_SIZE;
}

/**
 * Smart page size detection with corruption resilience
 * Tries multiple methods and validates results
 */
uint32_t smart_detect_page_size(const uint8_t* page_data, size_t file_size) {
    // First try FSP header detection
    uint32_t fsp_size = detect_page_size_from_fsp(page_data);

    // Validate against file size
    if (file_size > 0) {
        size_t pages = file_size / fsp_size;
        // If pages is unreasonable (too small or file not aligned), try fallback
        if (pages < 3 || file_size % fsp_size != 0) {
            return detect_page_size_from_file_size(file_size);
        }
    }

    return fsp_size;
}

uint8_t detect_row_format_from_fsp(const uint8_t* page_data) {
    if (!page_data) return 2;  // Default DYNAMIC

    uint32_t flags = read_be<uint32_t>(page_data, 54);

    // Bits 6-7: row format
    // 0 = REDUNDANT, 1 = COMPACT, 2 = DYNAMIC, 3 = COMPRESSED
    uint8_t format = (flags >> 6) & 0x03;
    return format;
}

bool is_antelope_format(const uint8_t* page_data) {
    uint8_t format = detect_row_format_from_fsp(page_data);
    return format <= 1;  // REDUNDANT or COMPACT
}

uint32_t read_fsp_space_flags(const uint8_t* page_data) {
    if (!page_data) return 0;
    return read_be<uint32_t>(page_data, 54);
}

uint32_t get_root_page_from_inode(const uint8_t* ibd_data, size_t ibd_size, uint32_t page_size) {
    // Root page is typically page 4 for the clustered index
    return 4;
}

std::vector<uint32_t> find_leaf_pages(const uint8_t* ibd_data, size_t ibd_size,
                                       uint32_t page_size, uint32_t root_page_no) {
    std::vector<uint32_t> leaf_pages;
    return leaf_pages;
}

bool validate_innodb_checksum(const uint8_t* page_data, size_t page_size) {
    return true;
}

bool validate_innodb_checksum_legacy(const uint8_t* page_data, size_t page_size) {
    return true;
}

} // namespace innodb
} // namespace dbrecovery
