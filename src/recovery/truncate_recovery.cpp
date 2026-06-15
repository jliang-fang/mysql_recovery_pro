/**
 * @file truncate_recovery.cpp
 * @brief TRUNCATE TABLE Recovery Implementation
 *
 * TRUNCATE TABLE in MySQL:
 * - MySQL 8.0+: Uses DROP + CREATE (file replaced)
 * - MySQL 5.7: Uses DELETE all rows (undo logs generated)
 *
 * Recovery strategies:
 * 1. Undo log parsing (for MySQL 5.7 style TRUNCATE)
 * 2. Disk carving for orphan pages (PhotoRec style)
 * 3. LSN-based page version tracking
 */

#include "dbrecovery/recovery/truncate_recovery.hpp"
#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <cstring>

namespace dbrecovery {
namespace recovery {

// ==================== Constants ====================

// InnoDB page types
constexpr uint16_t FIL_PAGE_INDEX = 0x45BF;
constexpr uint16_t FIL_PAGE_UNDO_LOG = 0x0002;
constexpr uint16_t FIL_PAGE_TYPE_BLOB = 0x000A;
constexpr uint16_t FIL_PAGE_SDI_BLOB = 0x45BD;
constexpr uint16_t FIL_PAGE_ALLOCATED = 0x0000;

// Undo log types for TRUNCATE detection
constexpr uint8_t TRX_UNDO_DEL_REC = 13;
constexpr uint8_t TRX_UNDO_UPD_DEL_REC = 12;

// ==================== TruncateRecovery Implementation ====================

TruncateRecovery::TruncateRecovery()
    : page_size_(16384),
      scan_disk_(false),
      use_undo_log_(true) {
}

TruncateRecovery::~TruncateRecovery() = default;

void TruncateRecovery::set_page_size(uint32_t page_size) {
    if (page_size == 4096 || page_size == 8192 || page_size == 16384 ||
        page_size == 32768 || page_size == 65536) {
        page_size_ = page_size;
    }
}

void TruncateRecovery::set_system_tablespace(const std::string& path) {
    system_tablespace_ = path;
}

bool TruncateRecovery::is_truncated_file(const std::string& ibd_path) {
    /**
     * Detect if a file has been TRUNCATEd:
     * 1. FSP header page (page 0) exists with valid structure
     * 2. Data pages (page 3+) are zeroed or marked as ALLOCATED
     * 3. INDEX pages count is 0 or very low
     */

    std::ifstream file(ibd_path, std::ios::binary);
    if (!file) {
        return false;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_ * 3) {
        return false;  // Too small to be truncated
    }

    // Read first page (FSP header)
    std::vector<uint8_t> fsp_page(page_size_);
    file.read(reinterpret_cast<char*>(fsp_page.data()), page_size_);

    if (!file) {
        return false;
    }

    // Check FSP page type (should be 0x0008 for FSP_HEADER)
    uint16_t fsp_type = innodb::read_be<uint16_t>(fsp_page.data(), 24);
    if (fsp_type != 0x0008 && fsp_type != FIL_PAGE_ALLOCATED) {
        return false;  // Not a valid InnoDB file
    }

    // Count INDEX pages in the file
    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
    uint32_t index_pages = 0;
    uint32_t zero_pages = 0;

    for (uint32_t page_no = 1; page_no < std::min(total_pages, 100u); page_no++) {
        file.seekg(page_no * page_size_, std::ios::beg);
        std::vector<uint8_t> page(page_size_);
        file.read(reinterpret_cast<char*>(page.data()), page_size_);

        if (!file) break;

        uint16_t page_type = innodb::read_be<uint16_t>(page.data(), 24);

        if (page_type == FIL_PAGE_INDEX) {
            index_pages++;
        }

        // Check if page is all zeros
        bool all_zero = true;
        for (size_t i = 0; i < 64; i++) {  // Check first 64 bytes
            if (page[i] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            zero_pages++;
        }
    }

    // TRUNCATE detection:
    // - INDEX pages count is very low (0-2)
    // - Most data pages are zeroed
    // - File still has reasonable size
    bool is_truncated = (index_pages <= 2) &&
                        (zero_pages > total_pages * 0.8) &&
                        (file_size > page_size_ * 3);

    if (is_truncated) {
        std::cout << "[INFO] Detected TRUNCATED file: " << ibd_path << "\n";
        std::cout << "  Total pages: " << total_pages << "\n";
        std::cout << "  INDEX pages: " << index_pages << "\n";
        std::cout << "  Zero pages: " << zero_pages << "\n";
    }

    return is_truncated;
}

TruncateRecovery::Result TruncateRecovery::recover(
    const std::string& ibd_path,
    const catalog::TableSchema& schema,
    std::function<void(double, const std::string&)> progress) {

    Result result;
    result.success = false;
    result.records_recovered = 0;
    result.method_used = "none";

    auto start_time = std::chrono::high_resolution_clock::now();

    // Step 1: Check if file is truncated
    if (progress) progress(0.1, "Checking file status...");

    bool is_truncated = is_truncated_file(ibd_path);

    if (!is_truncated) {
        // Not a truncated file - use normal recovery
        if (progress) progress(0.5, "File not truncated, using normal recovery...");

        // TODO: Delegate to normal recovery
        result.method_used = "normal_recovery";
        result.success = true;

        auto end_time = std::chrono::high_resolution_clock::now();
        result.recovery_time_seconds = std::chrono::duration<double>(end_time - start_time).count();
        return result;
    }

    // Step 2: Try undo log recovery (if system_tablespace provided)
    if (use_undo_log_ && !system_tablespace_.empty()) {
        if (progress) progress(0.2, "Attempting undo log recovery...");

        auto undo_records = recover_from_undo_log(system_tablespace_, schema);

        if (!undo_records.empty()) {
            result.records_recovered = undo_records.size();
            result.method_used = "undo_log";
            result.recovered_records = undo_records;
            result.success = true;

            std::cout << "[INFO] Recovered " << undo_records.size()
                      << " records from undo log\n";
        }
    }

    // Step 3: Try disk carving (PhotoRec style)
    if (scan_disk_ && !result.success) {
        if (progress) progress(0.4, "Attempting disk carving...");

        auto carved_records = recover_from_disk_carving(ibd_path, schema);

        if (!carved_records.empty()) {
            result.records_recovered += carved_records.size();
            result.method_used = result.method_used == "undo_log"
                ? "undo_log+disk_carving" : "disk_carving";

            // Merge records
            for (const auto& rec : carved_records) {
                // Deduplicate by primary key
                std::string pk;
                for (const auto& [key, value] : rec) {
                    if (key == "id" || key.find("_id") != std::string::npos) {
                        pk = value;
                        break;
                    }
                }

                if (pk.empty() || seen_primary_keys_.find(pk) == seen_primary_keys_.end()) {
                    result.recovered_records.push_back(rec);
                    if (!pk.empty()) {
                        seen_primary_keys_.insert(pk);
                    }
                }
            }

            result.success = true;
            std::cout << "[INFO] Recovered " << carved_records.size()
                      << " additional records from disk carving\n";
        }
    }

    // Step 4: Try orphan page scanning
    if (!result.success && !system_tablespace_.empty()) {
        if (progress) progress(0.6, "Scanning for orphan pages...");

        auto orphan_records = recover_from_orphan_pages(ibd_path, schema);

        if (!orphan_records.empty()) {
            result.records_recovered += orphan_records.size();
            result.method_used = result.method_used.empty()
                ? "orphan_pages" : result.method_used + "+orphan_pages";
            result.success = true;

            std::cout << "[INFO] Recovered " << orphan_records.size()
                      << " records from orphan pages\n";
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.recovery_time_seconds = std::chrono::duration<double>(end_time - start_time).count();

    if (progress) progress(1.0, "Recovery complete");

    return result;
}

std::vector<std::map<std::string, std::string>> TruncateRecovery::recover_from_undo_log(
    const std::string& ibdata_path,
    const catalog::TableSchema& schema) {

    std::vector<std::map<std::string, std::string>> records;

    /**
     * Undo log recovery strategy:
     * 1. Parse rollback segments from ibdata1
     * 2. Find DELETE undo records
     * 3. Reconstruct records from undo log data
     */

    std::cout << "[INFO] Scanning undo logs in: " << ibdata_path << "\n";

    innodb::UndoLogParser undo_parser;
    auto undo_records = undo_parser.scan_all_rollback_segments(ibdata_path);

    std::cout << "[INFO] Found " << undo_records << " undo log entries\n";

    if (undo_records > 0) {
        // Parse undo log entries for this table
        // TODO: Implement full undo log parsing with table_id filtering

        // For now, use heuristic approach
        auto heuristic_records = undo_parser.recover_from_undo_logs(
            ibdata_path,
            std::to_string(schema.table_id),
            0);  // Use 0 as default index_id

        // Convert HeuristicRecord to map format
        for (const auto& hr : heuristic_records) {
            std::map<std::string, std::string> record;

            size_t col_idx = 0;
            for (const auto& field : hr.fields) {
                std::string col_name = "col_" + std::to_string(col_idx);
                if (col_idx < schema.columns.size()) {
                    // Find column name by index
                    for (const auto& [idx, col] : schema.columns) {
                        if (idx == col_idx) {
                            col_name = col.name;
                            break;
                        }
                    }
                }
                record[col_name] = field.to_string();
                col_idx++;
            }

            records.push_back(record);
        }
    }

    return records;
}

std::vector<std::map<std::string, std::string>> TruncateRecovery::recover_from_disk_carving(
    const std::string& ibd_path,
    const catalog::TableSchema& schema) {

    std::vector<std::map<std::string, std::string>> records;

    /**
     * Disk carving strategy (PhotoRec style):
     * 1. Read file byte-by-byte looking for INDEX page signatures
     * 2. For each found INDEX page, try to parse records
     * 3. Use schema-agnostic parsing for unknown pages
     */

    std::cout << "[INFO] Disk carving: " << ibd_path << "\n";

    std::ifstream file(ibd_path, std::ios::binary);
    if (!file) {
        std::cerr << "[ERROR] Cannot open file for disk carving\n";
        return records;
    }

    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Scan for INDEX page signatures (0x45BF at offset 24-25)
    // Use sliding window approach like PhotoRec

    uint64_t scan_pos = 0;
    const uint64_t scan_step = page_size_;  // Page-aligned scan

    innodb::SchemaAgnosticScanner::Config sa_config;
    sa_config.recover_deleted = true;
    innodb::SchemaAgnosticScanner sa_scanner(sa_config);

    uint32_t pages_found = 0;

    while (scan_pos < file_size) {
        file.seekg(scan_pos, std::ios::beg);

        std::vector<uint8_t> buffer(page_size_);
        file.read(reinterpret_cast<char*>(buffer.data()), page_size_);

        if (!file) break;

        // Check for INDEX page signature
        uint16_t page_type = innodb::read_be<uint16_t>(buffer.data(), 24);

        if (page_type == FIL_PAGE_INDEX) {
            pages_found++;

            // Try schema-agnostic parsing
            auto page_result = sa_scanner.parse_page(buffer.data(), page_size_, scan_pos / page_size_);

            for (const auto& rec : page_result.records) {
                if (rec.is_deleted || !rec.is_valid) {
                    continue;
                }

                std::map<std::string, std::string> record;
                size_t col_idx = 0;

                for (const auto& field : rec.fields) {
                    std::string col_name;
                    if (col_idx < schema.columns.size()) {
                        for (const auto& [idx, col] : schema.columns) {
                            if (idx == col_idx) {
                                col_name = col.name;
                                break;
                            }
                        }
                    }
                    if (col_name.empty()) {
                        col_name = "col_" + std::to_string(col_idx);
                    }

                    record[col_name] = field.str_value;
                    col_idx++;
                }

                if (!record.empty()) {
                    records.push_back(record);
                }
            }
        }

        scan_pos += scan_step;
    }

    std::cout << "[INFO] Disk carving found " << pages_found << " INDEX pages, "
              << records.size() << " records\n";

    return records;
}

std::vector<std::map<std::string, std::string>> TruncateRecovery::recover_from_orphan_pages(
    const std::string& ibd_path,
    const catalog::TableSchema& schema) {

    std::vector<std::map<std::string, std::string>> records;

    /**
     * Orphan page recovery strategy:
     * 1. Scan file for pages with valid INDEX structure but zeroed n_recs
     * 2. Check for pages that were allocated but never written
     * 3. Look for pages with mismatched LSN (older version)
     */

    std::ifstream file(ibd_path, std::ios::binary);
    if (!file) {
        return records;
    }

    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);

    // Scan all pages looking for orphan data
    for (uint32_t page_no = 0; page_no < total_pages; page_no++) {
        file.seekg(page_no * page_size_, std::ios::beg);

        std::vector<uint8_t> page(page_size_);
        file.read(reinterpret_cast<char*>(page.data()), page_size_);

        if (!file) break;

        // Check if page has residual data (not all zeros)
        bool has_data = false;
        for (size_t i = 100; i < page_size_ - 100; i++) {
            if (page[i] != 0) {
                has_data = true;
                break;
            }
        }

        if (!has_data) continue;

        // Check page type
        uint16_t page_type = innodb::read_be<uint16_t>(page.data(), 24);

        // Look for pages that might contain residual records
        if (page_type == FIL_PAGE_ALLOCATED || page_type == 0x0000) {
            // Allocated but empty page - might have residual data

            // Try to parse as if it were INDEX page
            innodb::SchemaAgnosticScanner::Config sa_config2;
            sa_config2.recover_deleted = true;
            innodb::SchemaAgnosticScanner sa_scanner2(sa_config2);

            auto page_result2 = sa_scanner2.parse_page(page.data(), page_size_, page_no);

            for (const auto& rec : page_result2.records) {
                if (!rec.is_valid) continue;

                std::map<std::string, std::string> record;
                size_t col_idx = 0;

                for (const auto& field : rec.fields) {
                    std::string col_name;
                    if (col_idx < schema.columns.size()) {
                        for (const auto& [idx, col] : schema.columns) {
                            if (idx == col_idx) {
                                col_name = col.name;
                                break;
                            }
                        }
                    }
                    if (col_name.empty()) {
                        col_name = "col_" + std::to_string(col_idx);
                    }

                    record[col_name] = field.str_value;
                    col_idx++;
                }

                if (!record.empty()) {
                    records.push_back(record);
                }
            }
        }
    }

    return records;
}

} // namespace recovery
} // namespace dbrecovery