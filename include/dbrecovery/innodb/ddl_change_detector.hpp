#ifndef DBRECOVERY_INNODB_DDL_CHANGE_CHAIN_DETECTOR_HPP
#define DBRECOVERY_INNODB_DDL_CHANGE_CHAIN_DETECTOR_HPP

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

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

struct SchemaVersion {
    uint32_t column_count = 0;
    uint32_t page_count = 0;
    std::vector<uint32_t> page_numbers;
    std::string description;
};

struct DDLAnalysisResult {
    std::vector<SchemaVersion> versions;
    uint32_t total_pages_scanned = 0;
    uint32_t total_pages_with_records = 0;
    bool has_instant_ddl = false;
    std::string summary;
};

class DDLChangeChainDetector {
public:
    explicit DDLChangeChainDetector(size_t page_size = 16384)
        : page_size_(page_size) {}

    /**
     * @brief Detect DDL change chain from an IBD file
     *
     * Scans data pages and detects variations in column count that
     * indicate multiple schema versions (e.g., INSTANT ADD COLUMN).
     */
    DDLAnalysisResult detect(const std::string& ibd_path) {
        DDLAnalysisResult result;

        std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
        if (!file) {
            result.summary = "Cannot open file";
            return result;
        }

        uint64_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
        std::vector<uint8_t> page_buf(page_size_);

        result.total_pages_scanned = total_pages;

        for (uint32_t pg = 0; pg < total_pages; pg++) {
            file.seekg(static_cast<std::streamoff>(pg) * page_size_, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_buf.data()), page_size_);
            if (file.gcount() < static_cast<std::streamsize>(page_size_)) break;

            uint16_t page_type = read_le<uint16_t>(page_buf.data(), 24);
            if (page_type != FIL_PAGE_INDEX) continue;

            // Skip internal (non-leaf) pages — only analyze leaf pages
            uint16_t page_level = read_le<uint16_t>(page_buf.data(), 38 + 2 + 10 + 2 + 2);
            if (page_level != 0) continue;

            auto column_count = detect_column_count_from_page(page_buf.data(), page_size_);
            if (!column_count.has_value()) continue;

            result.total_pages_with_records++;
            uint32_t col_cnt = column_count.value();

            bool found = false;
            for (auto& ver : result.versions) {
                if (ver.column_count == col_cnt) {
                    ver.page_count++;
                    ver.page_numbers.push_back(pg);
                    found = true;
                    break;
                }
            }
            if (!found) {
                SchemaVersion ver;
                ver.column_count = col_cnt;
                ver.page_count = 1;
                ver.page_numbers.push_back(pg);
                result.versions.push_back(ver);
            }
        }

        // Sort versions by column count (ascending — earlier versions have fewer cols)
        std::sort(result.versions.begin(), result.versions.end(),
            [](const SchemaVersion& a, const SchemaVersion& b) {
                return a.column_count < b.column_count;
            });

        // Generate descriptions
        for (auto& ver : result.versions) {
            std::ostringstream desc;
            desc << ver.column_count << " columns (" << ver.page_count << " pages)";
            ver.description = desc.str();
        }

        // Check for INSTANT DDL pattern
        if (result.versions.size() > 1) {
            result.has_instant_ddl = true;
            std::ostringstream summary;
            summary << "Detected " << result.versions.size()
                    << " schema versions (possible INSTANT DDL changes). ";
            for (size_t i = 0; i < result.versions.size(); i++) {
                if (i > 0) summary << ", ";
                summary << "v" << (i+1) << ": " << result.versions[i].description;
            }
            summary << ". Latest version has "
                    << result.versions.back().column_count << " columns.";
            result.summary = summary.str();
        } else if (result.versions.size() == 1) {
            result.summary = "Single schema version: "
                           + result.versions[0].description + ".";
        } else {
            result.summary = "No data pages with records found.";
        }

        return result;
    }

    /**
     * @brief Detect column count from a COMPACT/DYNAMIC format page
     *
     * Algorithm:
     * 1. Read PAGE_N_RECS from page header
     * 2. For each record (via PAGE_HEAP_TOP and record offsets):
     *    a. Read the offset table (2-byte entries, reverse)
     *    b. Column count = offset_table_entries - 3 (for TRX_ID, ROLL_PTR, ROW_ID)
     * 3. Return the most common column count
     */
    std::optional<uint32_t> detect_column_count_from_page(
        const uint8_t* page_data, size_t page_size) {

        // Page header starts at offset 38 (after FIL header)
        const size_t PAGE_HEADER_OFFSET = 38;
        if (page_size < PAGE_HEADER_OFFSET + 56) return std::nullopt;

        uint16_t page_n_recs = read_le<uint16_t>(page_data, PAGE_HEADER_OFFSET + 16);
        uint16_t page_level = read_le<uint16_t>(page_data, PAGE_HEADER_OFFSET + 26);
        uint16_t heap_top = read_le<uint16_t>(page_data, PAGE_HEADER_OFFSET + 20);

        if (page_n_recs == 0 || page_level != 0) return std::nullopt;
        if (heap_top < PAGE_HEADER_OFFSET + 56 || heap_top > page_size) return std::nullopt;

        // Find INFIMUM and SUPREMUM to locate the record heap
        uint16_t infimum_offset = PAGE_HEADER_OFFSET + 56 + 10; // +10 for INFIMUM header
        uint16_t supremum_offset = find_supremum_offset(page_data, PAGE_HEADER_OFFSET);

        std::map<uint32_t, uint32_t> col_count_votes;

        for (int retry = 0; retry < 2; retry++) {
            uint16_t scan_start = infimum_offset;
            uint16_t scan_end = heap_top;

            for (uint16_t pos = scan_start; pos + 5 < scan_end && pos < page_size;) {
                // Read offset table entry — first entry is record start offset
                uint16_t first_offset = read_le<uint16_t>(page_data, pos);
                if (first_offset <= pos || first_offset > page_size) {
                    pos += (retry == 0) ? 1 : 2;
                    continue;
                }

                // Count offset table entries by reading sequential decreasing offsets
                uint16_t prev_offset = first_offset;
                uint16_t entry_count = 1;
                for (uint16_t i = 2; pos + i + 2 <= page_size && entry_count < 200; i += 2) {
                    uint16_t next_off = read_le<uint16_t>(page_data, pos + i);
                    if (next_off == 0 || next_off >= prev_offset) break;
                    prev_offset = next_off;
                    entry_count++;
                }

                // Column count = offset entries - 2 (minus TRX_ID + ROLL_PTR)
                uint32_t col_cnt = (entry_count >= 3) ? entry_count - 2 : entry_count;
                if (col_cnt >= 1 && col_cnt <= 200) {
                    col_count_votes[col_cnt]++;
                }

                // Advance: the record ends where first_offset points
                pos = first_offset;
                if (pos <= scan_start) {
                    pos += (retry == 0) ? 5 : 2;
                }
            }
        }

        if (col_count_votes.empty()) return std::nullopt;

        // Return the most common column count
        return std::max_element(col_count_votes.begin(), col_count_votes.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    }

private:
    size_t page_size_;

    uint16_t find_supremum_offset(const uint8_t* page_data, uint16_t scan_start) {
        // SUPREMUM record: [2B offset_to_next=0] [1B info_flags] [8B "supremum"]
        for (uint16_t pos = scan_start + 20; pos + 13 <= page_size_; pos++) {
            if (page_data[pos] == 0 && page_data[pos + 1] == 0 &&
                page_data[pos + 2] == 0 && page_data[pos + 5] == 's' &&
                page_data[pos + 6] == 'u' && page_data[pos + 7] == 'p') {
                return pos;
            }
        }
        return page_size_ - 8; // fallback guess
    }
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_DDL_CHANGE_CHAIN_DETECTOR_HPP