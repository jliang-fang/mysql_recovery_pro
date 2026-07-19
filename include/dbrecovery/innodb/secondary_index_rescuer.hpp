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
};

struct SecondaryIndexRescueStats {
    uint64_t secondary_pages_scanned = 0;
    uint64_t secondary_leaf_pages = 0;
    uint64_t secondary_records = 0;
    uint64_t rescued_rows = 0;
    uint64_t merged_rows = 0;
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

    void set_schema(const catalog::TableSchema& schema) { schema_ = schema; }

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

        uint64_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
        std::vector<uint8_t> page_buf(page_size_);

        for (uint32_t pg = 0; pg < total_pages; pg++) {
            file.seekg(static_cast<std::streamoff>(pg) * page_size_, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_buf.data()), page_size_);
            if (file.gcount() < static_cast<std::streamsize>(page_size_)) break;

            stats_.secondary_pages_scanned++;

            uint16_t page_type = read_le<uint16_t>(page_buf.data(), 24);
            if (page_type != FIL_PAGE_INDEX) continue;

            // Skip non-leaf pages
            uint16_t page_level = read_page_level(page_buf.data());
            if (page_level != 0) continue;

            stats_.secondary_leaf_pages++;

            auto records = parse_secondary_leaf_records(page_buf.data(), page_size_);
            for (auto& rec : records) {
                std::string pk_hex;
                for (const auto& col : rec.pk_columns) {
                    pk_hex += col.second;
                }

                if (pk_hex.empty()) continue;

                auto& existing = pk_data[pk_hex];
                for (auto& col : rec.index_columns) {
                    if (existing.find(col.first) == existing.end()) {
                        existing[col.first] = col.second;
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

        for (const auto& pk_hex : all_pks) {
            RescuedRow row;

            // Collect all column values across indexes
            for (const auto& idx_data : index_data_list) {
                auto it = idx_data.find(pk_hex);
                if (it == idx_data.end()) continue;
                for (const auto& [col_name, value] : it->second) {
                    if (row.fields.find(col_name) == row.fields.end()) {
                        row.fields[col_name] = value;
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

private:
    size_t page_size_;
    catalog::TableSchema schema_;
    SecondaryIndexRescueStats stats_;

    struct SecLeafRecord {
        std::vector<std::pair<std::string, std::string>> index_columns;
        std::vector<std::pair<std::string, std::string>> pk_columns;
    };

    uint16_t read_page_level(const uint8_t* page_data) {
        // PAGE_LEVEL is at PAGE_HEADER + 26 = 38 + 26 = 64
        return read_le<uint16_t>(page_data, 64);
    }

    std::vector<SecLeafRecord> parse_secondary_leaf_records(
        const uint8_t* page_data, size_t page_size) {

        std::vector<SecLeafRecord> records;
        const size_t PAGE_HEADER_OFFSET = 38;

        uint16_t page_n_recs = read_le<uint16_t>(page_data, PAGE_HEADER_OFFSET + 16);
        uint16_t heap_top = read_le<uint16_t>(page_data, PAGE_HEADER_OFFSET + 20);

        if (page_n_recs == 0 || heap_top < PAGE_HEADER_OFFSET + 56) return records;

        // INFIMUM record is at PAGE_HEADER + 56 + 10 (after page directory)
        uint16_t pos = PAGE_HEADER_OFFSET + 56 + 10;

        for (uint16_t i = 0; i < page_n_recs && pos + 10 < heap_top && pos < page_size; i++) {
            // Skip INFIMUM (first record)
            if (i == 0) {
                // INFIMUM: [2B next_offset] [1B info]
                uint16_t next_off = read_le<uint16_t>(page_data, pos);
                if (next_off == 0 || next_off <= pos) break;
                pos = next_off;
                continue;
            }

            // Read offset table at record start
            uint16_t first_offset = read_le<uint16_t>(page_data, pos);
            if (first_offset <= pos || first_offset > page_size) {
                pos += 5;
                continue;
            }

            // Count offset entries
            uint16_t prev_off = first_offset;
            uint16_t num_offsets = 1;
            for (uint16_t o = 2; pos + o + 2 <= page_size && num_offsets < 50; o += 2) {
                uint16_t nxt = read_le<uint16_t>(page_data, pos + o);
                if (nxt == 0 || nxt >= prev_off) break;
                prev_off = nxt;
                num_offsets++;
            }

            // Field count = offset entries - 2 (TRX_ID + ROLL_PTR)
            uint16_t num_fields = (num_offsets >= 3) ? num_offsets - 2 : num_offsets;

            // Record data starts at first_offset
            if (first_offset + 10 > page_size) {
                pos = first_offset;
                continue;
            }

            // Extract field values as hex strings at record data position
            SecLeafRecord rec;
            uint16_t data_pos = first_offset;

            // Determine how many index columns vs PK columns
            // Without schema, we extract all and let caller figure it out
            uint16_t sec_col_count = schema_.indexes.empty() ? 1 :
                std::min(num_fields, static_cast<uint16_t>(
                    schema_.indexes.front().columns.size()));

            for (uint16_t f = 0; f < num_fields && data_pos + 1 < page_size; f++) {
                // Simple extraction: read VARCHAR-like field (length-prefixed)
                uint8_t len_byte = page_data[data_pos];
                data_pos++;

                std::string val;
                if (len_byte < 128) {
                    // Single byte length
                    if (data_pos + len_byte <= page_size) {
                        val = std::string(reinterpret_cast<const char*>(page_data + data_pos), len_byte);
                        data_pos += len_byte;
                    }
                } else {
                    // Two-byte length
                    if (data_pos + 1 <= page_size) {
                        uint16_t len2 = ((len_byte & 0x7F) << 8) | page_data[data_pos];
                        data_pos++;
                        if (data_pos + len2 <= page_size) {
                            val = std::string(reinterpret_cast<const char*>(page_data + data_pos), len2);
                            data_pos += len2;
                        }
                    }
                }

                if (val.empty()) val = "?";

                // Assign to column names
                std::string col_name;
                if (f < sec_col_count && f < schema_.columns.size()) {
                    // Find the column by ordinal
                    for (const auto& [ord, col] : schema_.columns) {
                        if (ord == f + 1) { col_name = col.name; break; }
                    }
                    if (col_name.empty()) col_name = "sec_col_" + std::to_string(f);
                    rec.index_columns.push_back({col_name, val});
                } else {
                    uint16_t pk_idx = f - sec_col_count;
                    if (pk_idx < schema_.columns.size()) {
                        for (const auto& [ord, col] : schema_.columns) {
                            if (ord == pk_idx + 1) { col_name = col.name; break; }
                        }
                    }
                    if (col_name.empty()) col_name = "pk_col_" + std::to_string(pk_idx);
                    rec.pk_columns.push_back({col_name, val});
                }
            }

            if (!rec.index_columns.empty() || !rec.pk_columns.empty()) {
                records.push_back(rec);
            }

            // Advance to next record
            uint16_t next_offset = read_le<uint16_t>(page_data, first_offset - 2);
            if (next_offset <= first_offset || next_offset >= heap_top) break;
            pos = next_offset;
        }

        return records;
    }
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SECONDARY_INDEX_RESCUER_HPP