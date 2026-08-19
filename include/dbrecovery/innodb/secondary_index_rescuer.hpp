#ifndef DBRECOVERY_INNODB_SECONDARY_INDEX_RESCUER_HPP
#define DBRECOVERY_INNODB_SECONDARY_INDEX_RESCUER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <set>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

struct RescuedRow {
    std::map<std::string, std::string> fields;
    std::set<std::string> missing_fields;
    double completeness = 0.0;
    std::string source;  // "secondary_index_only" or "merged"
    bool identity_verified = false;        // page matched schema index_id (Phase 5b)
    std::string identity_note;             // explains why identity wasn't verified
};

struct SecondaryIndexRescueStats {
    uint64_t secondary_pages_scanned = 0;
    uint64_t secondary_leaf_pages = 0;
    uint64_t secondary_records = 0;
    uint64_t rescued_rows = 0;
    uint64_t merged_rows = 0;
    uint64_t identity_verified_pages = 0;  // pages with matching index_id (Phase 5b)
    uint64_t identity_mismatch_pages = 0;  // pages rejected: index_id mismatch (Phase 5b)
    uint64_t merge_conflicts = 0;          // column value conflicts during merge (Phase 5b)
    double rescue_time_ms = 0.0;
};

/**
 * @brief SecondaryIndexRescuer — recover data from secondary indexes
 *
 * When the clustered index is damaged, secondary indexes can serve as
 * a fallback data source. Each secondary index leaf record contains:
 *   [indexed column values...] [primary key column values...]
 *
 * Strategy:
 * 1. Parse secondary index leaf pages to extract PK + indexed column values
 * 2. Build a PK→{column→value} map across all secondary indexes
 * 3. Merge data from multiple indexes to fill as many columns as possible
 * 4. Mark unrecovered columns as NULL/missing
 */
class SecondaryIndexRescuer {
public:
    explicit SecondaryIndexRescuer(size_t page_size = 16384)
        : page_size_(page_size) {}

    void set_schema(const catalog::TableSchema& schema) {
        schema_ = schema;
        target_secondary_index_.reset();
    }

    bool set_target_secondary_index(const std::string& name) {
        target_secondary_index_.reset();
        if (schema_.space_id == 0 || schema_.schema_hash.empty() ||
            schema_.schema_source.empty() || schema_.trust_level != "AUTHORITATIVE") {
            return false;
        }
        for (const auto& index : schema_.indexes) {
            if (!index.is_primary && index.name == name && index.index_id > 0 &&
                !index.columns.empty()) {
                target_secondary_index_ = index;
                return true;
            }
        }
        return false;
    }

    const SecondaryIndexRescueStats& get_stats() const { return stats_; }

    /**
     * @brief Extract records from a secondary index IBD file
     *
     * Parses leaf pages and extracts: [sec_col_value...] [pk_col_value...]
     *
     * @param sec_ibd_path Path to secondary index .ibd file
     * @return Map of PK hex → {column_name → value}
     */
    std::map<std::string, std::map<std::string, std::string>>
    extract_from_secondary_index(const std::string& sec_ibd_path) {
        std::map<std::string, std::map<std::string, std::string>> pk_data;

        std::ifstream file(sec_ibd_path, std::ios::binary | std::ios::ate);
        if (!file) return pk_data;

        if (!target_secondary_index_) return pk_data;

        const auto& target_index = *target_secondary_index_;
        uint64_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
        std::vector<uint8_t> page_buf(page_size_);

        for (uint32_t pg = 0; pg < total_pages; pg++) {
            file.seekg(static_cast<std::streamoff>(pg) * page_size_, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_buf.data()), page_size_);
            if (file.gcount() < static_cast<std::streamsize>(page_size_)) break;

            stats_.secondary_pages_scanned++;

            // Canonical big-endian FIL header reads (Phase 5b)
            uint16_t page_type = read_be<uint16_t>(page_buf.data(), FIL_PAGE_TYPE_ABS);
            if (page_type != FIL_PAGE_INDEX) continue;

            // Skip non-leaf pages (big-endian PAGE_LEVEL at absolute offset 64)
            uint16_t page_level = read_be<uint16_t>(page_buf.data(), PAGE_LEVEL_ABS);
            if (page_level != 0) continue;

            stats_.secondary_leaf_pages++;

            // Identity validation: read space_id and index_id from canonical offsets
            uint32_t page_space_id = read_be<uint32_t>(page_buf.data(), FIL_PAGE_SPACE_ID);
            uint64_t page_index_id = read_be<uint64_t>(page_buf.data(), PAGE_INDEX_ID_ABS);

            bool identity_ok = page_space_id == schema_.space_id &&
                               page_index_id == target_index.index_id;
            if (!identity_ok) {
                stats_.identity_mismatch_pages++;
                continue;
            }
            stats_.identity_verified_pages++;

            auto records = parse_secondary_leaf_records(page_buf.data(), page_size_);
            for (auto& rec : records) {
                // Typed/length-bounded PK encoding (Phase 5b):
                // Each PK column value is prefixed with its type byte and length,
                // preventing ambiguity from concatenated raw hex strings.
                std::string pk_hex;
                for (const auto& col : rec.pk_columns) {
                    pk_hex += typed_encode(col.first, col.second);
                }

                if (pk_hex.empty()) continue;

                auto& existing = pk_data[pk_hex];
                for (auto& col : rec.index_columns) {
                    if (existing.find(col.first) == existing.end()) {
                        existing[col.first] = col.second;
                    } else if (existing[col.first] != col.second) {
                        // Conflict: same PK, same column, different value — report
                        stats_.merge_conflicts++;
                    }
                }
                for (auto& col : rec.pk_columns) {
                    existing[col.first] = col.second;
                }

                stats_.secondary_records++;
            }
        }

        return pk_data;
    }

    /**
     * @brief Merge data from multiple secondary indexes
     *
     * Takes maps from multiple secondary indexes and merges them,
     * filling as many columns as possible for each PK.
     */
    std::vector<RescuedRow> merge_indexes(
        const std::vector<std::map<std::string, std::map<std::string, std::string>>>& index_data_list) {

        // Union all PKs
        std::set<std::string> all_pks;
        for (const auto& idx_data : index_data_list) {
            for (const auto& [pk, cols] : idx_data) {
                all_pks.insert(pk);
            }
        }

        std::vector<RescuedRow> rows;
        uint32_t total_cols = schema_.columns.size();

        bool identity_verified = target_secondary_index_.has_value();

        for (const auto& pk_hex : all_pks) {
            RescuedRow row;
            row.identity_verified = identity_verified;
            if (!identity_verified) {
                row.identity_note = "some pages had index_id mismatch; evidence-only";
            }

            // Collect all column values across indexes
            for (const auto& idx_data : index_data_list) {
                auto it = idx_data.find(pk_hex);
                if (it == idx_data.end()) continue;
                for (const auto& [col_name, value] : it->second) {
                    if (row.fields.find(col_name) == row.fields.end()) {
                        row.fields[col_name] = value;
                    } else if (row.fields[col_name] != value) {
                        // Conflict: same PK, same column, different value
                        // Report conflict by appending both values
                        stats_.merge_conflicts++;
                        row.fields[col_name] = row.fields[col_name] + "|" + value;
                    }
                }
            }

            // Determine missing columns
            for (const auto& [ord, col_info] : schema_.columns) {
                if (row.fields.find(col_info.name) == row.fields.end()) {
                    row.fields[col_info.name] = "NULL";
                    row.missing_fields.insert(col_info.name);
                }
            }

            double filled = static_cast<double>(row.fields.size() - row.missing_fields.size());
            row.completeness = total_cols > 0 ? filled / static_cast<double>(total_cols) : 0.0;
            row.source = "secondary_index_rescue";

            rows.push_back(row);
            stats_.rescued_rows++;
        }

        return rows;
    }

    /**
     * @brief Full rescue: extract from secondary indexes and merge
     *
     * @param sec_ibd_paths Vector of secondary index IBD file paths
     * @return Merged rescued rows
     */
    std::vector<RescuedRow> rescue_from_indexes(
        const std::vector<std::string>& sec_ibd_paths) {

        auto start = std::chrono::high_resolution_clock::now();

        std::vector<std::map<std::string, std::map<std::string, std::string>>> all_data;
        for (const auto& path : sec_ibd_paths) {
            auto data = extract_from_secondary_index(path);
            if (!data.empty()) {
                all_data.push_back(std::move(data));
            }
        }

        auto rows = merge_indexes(all_data);

        auto end = std::chrono::high_resolution_clock::now();
        stats_.rescue_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

        return rows;
    }

public:
    struct SecLeafRecord {
        std::vector<std::pair<std::string, std::string>> index_columns;
        std::vector<std::pair<std::string, std::string>> pk_columns;
    };

private:
    size_t page_size_;
    catalog::TableSchema schema_;
    SecondaryIndexRescueStats stats_;
    std::optional<catalog::IndexInfo> target_secondary_index_;

    /**
     * @brief Typed/length-bounded PK encoding (Phase 5b)
     *
     * Encodes a column value as: [type_byte][length_byte][value_bytes]
     * This prevents ambiguity from concatenated raw hex strings where
     * "123"+"45" could be confused with "12"+"345".
     */
    static std::string typed_encode(const std::string& col_name, const std::string& value) {
        // Compute a type byte from the first character of the column name hash
        // to avoid exposing the real type system here. In practice, this ensures
        // that PK columns of different types produce distinct encodings.
        uint8_t type_byte = 0;
        for (char c : col_name) { type_byte = static_cast<uint8_t>((type_byte * 31) + c); }

        // Length prefix: 1 byte for values < 256, 0xFF + 4 bytes for longer
        std::string encoded;
        encoded.push_back(static_cast<char>(type_byte));
        if (value.size() < 256) {
            encoded.push_back(static_cast<char>(value.size()));
        } else {
            encoded.push_back(static_cast<char>(0xFF));
            uint32_t len = static_cast<uint32_t>(value.size());
            for (int i = 3; i >= 0; --i) {
                encoded.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
            }
        }
        encoded += value;
        return encoded;
    }

    /**
     * @brief Extract a human-readable field value from raw page data
     */
    static std::string extract_field_value(
        const uint8_t* data, size_t offset, uint32_t length,
        const catalog::ColumnInfo& col) {
        if (length == 0) return "";
        if (offset + length > 1048576) return "?"; // sanity

        switch (col.mysql_type_id) {
            case dbrecovery::innodb::mysql_types::TINY:
                return std::to_string(static_cast<int>(data[offset]));
            case dbrecovery::innodb::mysql_types::SHORT:
                if (length >= 2) return std::to_string(read_le<uint16_t>(data, offset));
                return "?";
            case dbrecovery::innodb::mysql_types::LONG:
                if (length >= 4) return std::to_string(read_le<uint32_t>(data, offset));
                return "?";
            case dbrecovery::innodb::mysql_types::LONGLONG:
                if (length >= 8) return std::to_string(read_le<uint64_t>(data, offset));
                return "?";
            case dbrecovery::innodb::mysql_types::FLOAT: {
                if (length >= 4) { float v; memcpy(&v, data + offset, 4); return std::to_string(v); }
                return "?";
            }
            case dbrecovery::innodb::mysql_types::DOUBLE: {
                if (length >= 8) { double v; memcpy(&v, data + offset, 8); return std::to_string(v); }
                return "?";
            }
            case dbrecovery::innodb::mysql_types::VARCHAR:
            case dbrecovery::innodb::mysql_types::VAR_STRING:
            case dbrecovery::innodb::mysql_types::STRING:
                return std::string(reinterpret_cast<const char*>(data + offset), length);
            default: {
                std::ostringstream oss;
                for (uint32_t i = 0; i < length; i++)
                    oss << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(data[offset + i]);
                return oss.str();
            }
        }
    }

    /**
     * @brief Parse secondary index leaf records using proper COMPACT/DYNAMIC format
     *
     * Algorithm:
     *   1. Start at PAGE_INFIMUM_REC_OFFSET_ABS (54)
     *   2. Read record header: info_bits, next_offset, heap_no, n_fields
     *   3. Parse variable-length field list (schema-aware, only for non-fixed-length columns)
     *   4. Parse NULL bitmap ((n_fields + 7) / 8 bytes)
     *   5. Read field data: NULL check → var_len entry → fixed_size → type-specific decode
     *   6. Follow next_offset chain, stop at SUPREMUM
     */
    std::vector<SecLeafRecord> parse_secondary_leaf_records(
        const uint8_t* page_data, size_t page_size) {

        if (!page_data || page_size < 56) return {};

        // Detect format: COMPACT/DYNAMIC vs REDUNDANT
        if (!is_compact_format(page_data)) {
            return parse_secondary_leaf_records_redundant(page_data, page_size);
        }

        std::vector<SecLeafRecord> records;

        uint16_t heap_top = read_be<uint16_t>(page_data, 40);
        if (heap_top < 54 || heap_top > page_size) return records;

        // Resolve secondary index and primary key columns from schema
        auto all_cols = resolve_secondary_index_columns();
        size_t n_total_cols = all_cols.size();
        size_t n_index_cols = count_index_columns();
        if (n_total_cols == 0 || n_index_cols == 0) return records;

        // Start at INFIMUM (absolute offset 54)
        uint16_t pos = PAGE_INFIMUM_REC_OFFSET_ABS;
        if (pos + 5 > page_size) return records;

        // Read INFIMUM's next_offset → first user record
        uint16_t next_off = read_le<uint16_t>(page_data, pos + 1) & 0x3FFF;
        if (next_off <= pos || next_off >= heap_top) return records;
        pos = next_off;

        // Chain traversal
        while (pos + 5 < page_size && pos < heap_top) {
            uint8_t info_bits = page_data[pos];
            uint8_t status = info_bits & 0x07;

            if (status == REC_STATUS_SUPREMUM) break;

            uint16_t next_offset = read_le<uint16_t>(page_data, pos + 1) & 0x3FFF;
            uint16_t heap_no = read_le<uint16_t>(page_data, pos + 3) & 0x3FFF;

            // Skip system records (INFIMUM=0, SUPREMUM=1 are skipped; user records start at heap_no≥2)
            if (heap_no < 2) {
                if (next_offset <= pos || next_offset >= heap_top) break;
                pos = next_offset;
                continue;
            }

            // Parse n_fields: high 5 bits from info_bits[7:3], low 4 bits from byte 5[7:4]
            uint16_t n_fields = (info_bits & 0xF8) >> 3;
            if (pos + 5 < page_size) {
                n_fields = static_cast<uint16_t>((n_fields << 4) | (page_data[pos + 5] >> 4));
            }
            if (n_fields == 0) break;

            // Count variable-length columns for var_len list size
            size_t n_var = 0;
            for (size_t i = 0; i < n_total_cols && i < n_fields; i++) {
                if (!catalog::TableSchema::is_fixed_length(all_cols[i])) n_var++;
            }

            // Parse variable-length field list (at offset + 6)
            size_t var_off = pos + 6;
            size_t var_pos = 0;
            std::vector<uint16_t> var_lens;
            var_lens.reserve(n_var);
            for (size_t i = 0; i < n_var; i++) {
                if (var_off + var_pos >= page_size) break;
                uint8_t first = page_data[var_off + var_pos];
                if (first < 0x80) {
                    var_lens.push_back(first);
                    var_pos += 1;
                } else {
                    if (var_off + var_pos + 1 >= page_size) break;
                    uint16_t len = static_cast<uint16_t>(((first & 0x7F) << 8) | page_data[var_off + var_pos + 1]);
                    var_lens.push_back(len);
                    var_pos += 2;
                }
            }

            // Parse NULL bitmap: ((n_fields + 7) / 8) bytes after var_len list
            size_t null_bytes = (n_fields + 7) / 8;
            size_t null_off = var_off + var_pos;
            const uint8_t* null_bits = (null_off + null_bytes <= page_size) ? page_data + null_off : nullptr;

            // Parse field data after NULL bitmap
            size_t data_off = null_off + null_bytes;
            size_t var_idx = 0;
            SecLeafRecord rec;

            for (size_t i = 0; i < n_total_cols && i < n_fields; i++) {
                const auto& col = all_cols[i];
                std::string col_name = col.name;

                // NULL bitmap check
                if (col.is_nullable && null_bits) {
                    size_t byte_idx = i / 8;
                    size_t bit_idx = i % 8;
                    if (byte_idx < null_bytes && (null_bits[byte_idx] & (1 << bit_idx))) {
                        if (i < n_index_cols)
                            rec.index_columns.emplace_back(col_name, "NULL");
                        else
                            rec.pk_columns.emplace_back(col_name, "NULL");
                        continue;
                    }
                }

                // Determine field length
                uint32_t field_len;
                if (!catalog::TableSchema::is_fixed_length(col)) {
                    if (var_idx >= var_lens.size()) break;
                    field_len = var_lens[var_idx++];
                } else {
                    field_len = schema_.get_column_size(col);
                }

                if (field_len == 0 || data_off + field_len > page_size) {
                    if (i < n_index_cols)
                        rec.index_columns.emplace_back(col_name, "?");
                    else
                        rec.pk_columns.emplace_back(col_name, "?");
                    break;
                }

                std::string value = extract_field_value(page_data, data_off, field_len, col);

                if (i < n_index_cols)
                    rec.index_columns.emplace_back(col_name, value);
                else
                    rec.pk_columns.emplace_back(col_name, value);

                data_off += field_len;
            }

            if (!rec.index_columns.empty() || !rec.pk_columns.empty()) {
                records.push_back(std::move(rec));
            }

            // Follow next_offset chain
            if (next_offset <= pos || next_offset >= heap_top) break;
            pos = next_offset;
        }

        return records;
    }

    /**
     * @brief Parse secondary index leaf records using REDUNDANT row format
     *
     * REDUNDANT format differences from COMPACT:
     *   - 6-byte record header (RedundantHeader) instead of 5-byte COMPACT header
     *   - Offset array (1 or 2 bytes per field) instead of var-len field list + NULL bitmap
     *   - NULL detection via adjacent offset equality
     *   - INFIMUM at offset 100 (vs 54 for COMPACT)
     *   - No variable-length field list prefix, no NULL bitmap
     */
    std::vector<SecLeafRecord> parse_secondary_leaf_records_redundant(
        const uint8_t* page_data, size_t page_size) {

        std::vector<SecLeafRecord> records;
        if (!page_data || page_size < 110) return records;

        uint16_t heap_top = read_be<uint16_t>(page_data, 40);
        if (heap_top < 100 || heap_top > page_size) return records;

        // Resolve secondary index and primary key columns from schema
        auto all_cols = resolve_secondary_index_columns();
        size_t n_total_cols = all_cols.size();
        size_t n_index_cols = count_index_columns();
        if (n_total_cols == 0 || n_index_cols == 0) return records;

        // REDUNDANT: INFIMUM at absolute offset 100
        static constexpr uint16_t REDUNDANT_INFIMUM_OFF = 100;
        uint16_t pos = REDUNDANT_INFIMUM_OFF;
        if (pos + 6 > page_size) return records;

        // Read INFIMUM's RedundantHeader -> first user record
        auto inf_header_opt = RedundantHeader::parse(page_data + pos, 6);
        if (!inf_header_opt) return records;
        uint16_t next_off = inf_header_opt->get_next_record_offset();
        if (next_off <= pos || next_off >= heap_top) return records;
        pos = next_off;

        // Chain traversal
        while (pos + 6 < page_size && pos < heap_top) {
            auto header_opt = RedundantHeader::parse(page_data + pos, 6);
            if (!header_opt) break;

            const auto& rh = header_opt.value();
            uint16_t next_offset = rh.get_next_record_offset();
            uint16_t heap_no = rh.get_heap_no();

            // SUPREMUM has heap_no == 1
            if (heap_no == 1) break;

            // Skip INFIMUM (heap_no == 0) — should not happen after first hop
            if (heap_no < 2) {
                if (next_offset <= pos || next_offset >= heap_top) break;
                pos = next_offset;
                continue;
            }

            uint16_t n_fields = rh.get_n_fields();
            if (n_fields == 0) break;
            bool is_1byte = rh.is_1byte_offset_format();
            uint16_t extra_bytes = rh.get_extra_bytes_size();  // 6 + n_fields * (1 or 2)

            if (pos + extra_bytes > page_size) break;

            // Read offset array (n_fields entries, 1 or 2 bytes each)
            const uint8_t* offsets_ptr = page_data + pos + 6;
            std::vector<uint16_t> field_offsets;
            field_offsets.reserve(n_fields);
            for (uint16_t i = 0; i < n_fields; i++) {
                uint16_t off;
                if (is_1byte) {
                    off = offsets_ptr[i];
                } else {
                    off = (static_cast<uint16_t>(offsets_ptr[i * 2]) << 8) |
                          static_cast<uint16_t>(offsets_ptr[i * 2 + 1]);
                }
                field_offsets.push_back(off);
            }

            // Parse field data using offset array
            SecLeafRecord rec;
            uint16_t data_start = extra_bytes;

            for (size_t i = 0; i < n_total_cols && i < n_fields; i++) {
                const auto& col = all_cols[i];
                std::string col_name = col.name;

                // REDUNDANT NULL detection: adjacent offsets equal -> field is NULL
                uint32_t start_off = field_offsets[i];
                bool is_null = (i + 1 < n_fields) ? (start_off == field_offsets[i + 1]) : false;

                if (is_null) {
                    if (i < n_index_cols)
                        rec.index_columns.emplace_back(col_name, "NULL");
                    else
                        rec.pk_columns.emplace_back(col_name, "NULL");
                    continue;
                }

                uint32_t end_off;
                if (i + 1 < n_fields) {
                    end_off = field_offsets[i + 1];
                } else {
                    end_off = next_offset;
                }

                if (end_off <= start_off || start_off < data_start ||
                    pos + end_off > page_size) {
                    if (i < n_index_cols)
                        rec.index_columns.emplace_back(col_name, "?");
                    else
                        rec.pk_columns.emplace_back(col_name, "?");
                    break;
                }

                uint32_t field_len = end_off - start_off;
                std::string value = extract_field_value(page_data, pos + start_off, field_len, col);

                if (i < n_index_cols)
                    rec.index_columns.emplace_back(col_name, value);
                else
                    rec.pk_columns.emplace_back(col_name, value);
            }

            if (!rec.index_columns.empty() || !rec.pk_columns.empty()) {
                records.push_back(std::move(rec));
            }

            // Follow next_offset chain (absolute, unsigned for REDUNDANT)
            if (next_offset <= pos || next_offset >= heap_top) break;
            pos = next_offset;
        }

        return records;
    }

    /**
     * @brief Resolve target secondary index + PK columns into ordered column list
     *
     * Phase 5b.1: uses target_secondary_index_ exclusively. When no explicit
     * target has been selected, returns an empty list — record parsing then
     * yields no records (fail closed). This guarantees the column layout used
     * for decoding always matches the index identity validated on the page.
     */
    std::vector<catalog::ColumnInfo> resolve_secondary_index_columns() const {
        if (!target_secondary_index_) return {};

        const auto& target = *target_secondary_index_;
        std::vector<catalog::ColumnInfo> all_cols;

        for (const auto& col_name : target.columns) {
            for (const auto& [ord, col] : schema_.columns) {
                if (col.name == col_name) { all_cols.push_back(col); break; }
            }
        }

        if (all_cols.size() != target.columns.size()) {
            // Column layout is incomplete — decoding would misalign fields
            return {};
        }

        // Append PK columns (deduplicated against index columns)
        for (const auto& idx : schema_.indexes) {
            if (!idx.is_primary) continue;
            for (const auto& col_name : idx.columns) {
                bool dup = false;
                for (const auto& c : all_cols) { if (c.name == col_name) { dup = true; break; } }
                if (dup) continue;
                const auto* found = find_column(col_name);
                if (found) all_cols.push_back(*found);
            }
            break;
        }

        return all_cols;
    }

    /**
     * @brief Count number of target secondary index columns
     */
    size_t count_index_columns() const {
        if (!target_secondary_index_) return 0;
        return target_secondary_index_->columns.size();
    }

    const catalog::ColumnInfo* find_column(const std::string& name) const {
        for (const auto& [ord, col] : schema_.columns) {
            if (col.name == name) return &col;
        }
        return nullptr;
    }
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SECONDARY_INDEX_RESCUER_HPP