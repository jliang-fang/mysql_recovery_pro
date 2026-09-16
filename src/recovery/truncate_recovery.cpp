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
 * 4. Heuristic record detection in zeroed pages
 */

#include "dbrecovery/recovery/truncate_recovery.hpp"
#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include "dbrecovery/innodb/heuristic_parser.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/catalog/schema_type_conversion.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <set>

namespace dbrecovery {
namespace recovery {

// ==================== Constants ====================

// InnoDB page types
constexpr uint16_t FIL_PAGE_INDEX = 0x45BF;
constexpr uint16_t FIL_PAGE_UNDO_LOG = 0x0002;
constexpr uint16_t FIL_PAGE_TYPE_BLOB = 0x000A;
constexpr uint16_t FIL_PAGE_SDI_BLOB = 0x45BD;
constexpr uint16_t FIL_PAGE_ALLOCATED = 0x0000;
constexpr uint16_t FIL_PAGE_FSP_HDR = 0x0008;

// Undo log types for TRUNCATE detection
constexpr uint8_t TRX_UNDO_DEL_REC = 13;
constexpr uint8_t TRX_UNDO_UPD_DEL_REC = 12;

// Record header signatures for heuristic detection
constexpr uint8_t REC_STATUS_INFIMUM = 0x01;
constexpr uint8_t REC_STATUS_SUPREMUM = 0x02;
constexpr uint8_t REC_STATUS_NORMAL = 0x00;

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
    uint32_t blob_pages = 0;
    uint32_t sdi_pages = 0;
    uint32_t pages_checked = 0;

    // Check all pages for accurate detection
    // For large files, sample up to 1000 pages distributed across the file
    uint32_t pages_to_check;
    if (total_pages <= 1000) {
        pages_to_check = total_pages;
    } else {
        // Sample 1000 pages spread across the file
        pages_to_check = 1000;
    }

    uint32_t page_step = (total_pages > 1000) ? (total_pages / 1000) : 1;

    for (uint32_t i = 0; i < pages_to_check && i < total_pages; i++) {
        uint32_t page_no = (total_pages > 1000) ? (i * page_step) : i;
        if (page_no == 0) continue;  // Skip FSP header

        file.seekg(static_cast<uint64_t>(page_no) * page_size_, std::ios::beg);
        std::vector<uint8_t> page(page_size_);
        file.read(reinterpret_cast<char*>(page.data()), page_size_);

        if (!file) break;
        pages_checked++;

        uint16_t page_type = innodb::read_be<uint16_t>(page.data(), 24);

        if (page_type == FIL_PAGE_INDEX) {
            index_pages++;
        } else if (page_type == FIL_PAGE_TYPE_BLOB) {
            blob_pages++;
        } else if (page_type == FIL_PAGE_SDI_BLOB) {
            sdi_pages++;
        }

        // Check if page is all zeros (check more thoroughly)
        bool all_zero = true;
        for (size_t j = 0; j < page_size_; j++) {
            if (page[j] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            zero_pages++;
        }
    }

    // TRUNCATE detection:
    // - File has valid structure but NO INDEX pages
    // - No BLOB/SDI pages with data (these indicate active table)
    // - Most data pages are zeroed (>90% of checked pages)
    // - File has reasonable size (at least 4 pages)
    //
    // Important: A file with even 1 INDEX page and/or BLOB pages is NOT truncated
    // A truly truncated file will have:
    // - 0 INDEX pages (or just empty root page)
    // - 0 BLOB/SDI pages
    // - Almost all pages zeroed
    bool is_truncated = (index_pages == 0) &&
                        (blob_pages == 0) &&
                        (sdi_pages == 0) &&
                        (pages_checked > 0 && zero_pages >= pages_checked * 0.9) &&
                        (total_pages >= 4);

    if (is_truncated) {
        std::cout << "[INFO] Detected TRUNCATED file: " << ibd_path << "\n";
        std::cout << "  Total pages: " << total_pages << "\n";
        std::cout << "  Pages checked: " << pages_checked << "\n";
        std::cout << "  INDEX pages: " << index_pages << "\n";
        std::cout << "  BLOB pages: " << blob_pages << "\n";
        std::cout << "  SDI pages: " << sdi_pages << "\n";
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

    // Step 1: Check if file is truncated or empty
    if (progress) progress(0.1, "Checking file status...");

    bool is_truncated = is_truncated_file(ibd_path);

    // For MySQL 5.7 style TRUNCATE (DELETE all rows), file is NOT empty
    // but records are marked as deleted. We should still try undo log recovery.
    // For MySQL 8.0+ style TRUNCATE (DROP + CREATE), file IS empty.

    // Always try undo log recovery if system_tablespace is provided,
    // regardless of whether file appears truncated
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

    // If file appears truncated and no undo log records found, continue with other methods
    if (!is_truncated && result.success) {
        // Found records from undo log, file is NOT physically truncated
        // This is MySQL 5.7 style TRUNCATE (DELETE)
        if (progress) progress(0.8, "Recovery from undo log completed...");

        auto end_time = std::chrono::high_resolution_clock::now();
        result.recovery_time_seconds = std::chrono::duration<double>(end_time - start_time).count();
        return result;
    }

    // If file is NOT truncated and no undo log records found,
    // this might be a normal file with no deleted records
    if (!is_truncated) {
        if (progress) progress(0.5, "File not truncated, no undo log records found...");

        // Still try disk carving and heuristic detection as fallback
        // This handles cases where TRUNCATE detection missed the file state
    }

    // Check if the file has any residual data at all
    std::ifstream file_check(ibd_path, std::ios::binary);
    if (file_check) {
        file_check.seekg(0, std::ios::end);
        uint64_t file_size = file_check.tellg();
        file_check.seekg(0, std::ios::beg);

        // Skip first page (FSP header), check for any non-zero bytes in rest of file
        bool has_residual_data = false;
        const size_t check_chunk_size = 64 * 1024;  // 64KB chunks
        std::vector<uint8_t> check_chunk(check_chunk_size);

        file_check.seekg(page_size_, std::ios::beg);  // Skip FSP header

        while (file_check && file_check.tellg() < static_cast<std::streampos>(file_size)) {
            size_t to_read = std::min(check_chunk_size,
                static_cast<size_t>(file_size - file_check.tellg()));
            file_check.read(reinterpret_cast<char*>(check_chunk.data()), to_read);

            for (size_t i = 0; i < to_read; i++) {
                if (check_chunk[i] != 0) {
                    has_residual_data = true;
                    break;
                }
            }
            if (has_residual_data) break;
        }

        if (!has_residual_data) {
            std::cout << "[WARN] File is completely zeroed after FSP header. "
                      << "No residual data to recover.\n";
            std::cout << "[INFO] TRUNCATE recovery options:\n";
            std::cout << "  1. Check undo logs in system tablespace (ibdata1/mysql.ibd) "
                      << "for MySQL 5.7 style TRUNCATE\n";
            std::cout << "  2. Use --system-tablespace option to scan undo logs\n";
            std::cout << "  3. Access raw disk device for disk carving (PhotoRec style)\n";
            std::cout << "  4. Restore from backup if available\n";

            result.method_used = "zeroed_file";
            result.success = false;
            result.errors.push_back("File is completely zeroed after FSP header");

            auto end_time = std::chrono::high_resolution_clock::now();
            result.recovery_time_seconds = std::chrono::duration<double>(end_time - start_time).count();
            return result;
        }
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

    // Step 4: Try heuristic residual data detection (PhotoRec-style signature scan)
    if (!result.success || result.records_recovered == 0) {
        if (progress) progress(0.6, "Scanning for residual data signatures...");

        auto residual_records = detect_residual_records(ibd_path, schema);

        if (!residual_records.empty()) {
            result.records_recovered += residual_records.size();
            result.method_used = result.method_used.empty()
                ? "heuristic_detection" : result.method_used + "+heuristic";

            // Merge records with deduplication
            for (const auto& rec : residual_records) {
                std::string hash;
                for (const auto& [key, value] : rec) {
                    hash += key + "=" + value + "|";
                }

                if (seen_primary_keys_.find(hash) == seen_primary_keys_.end()) {
                    result.recovered_records.push_back(rec);
                    seen_primary_keys_.insert(hash);
                }
            }

            result.success = true;
            std::cout << "[INFO] Recovered " << residual_records.size()
                      << " records from heuristic signature scan\n";
        }
    }

    // Step 5: Try orphan page scanning
    if (!result.success && !system_tablespace_.empty()) {
        if (progress) progress(0.8, "Scanning for orphan pages...");

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
        // Using heuristic approach with table_id filtering
        // The table_id helps identify which undo log records belong to our target table
        auto heuristic_records = undo_parser.recover_from_undo_logs(
            ibdata_path,
            std::to_string(schema.table_id),
            schema.table_id);  // Use table_id for filtering

        // Convert HeuristicRecord to map format with forensic provenance marker
        for (const auto& hr : heuristic_records) {
            std::map<std::string, std::string> record;

            // Forensic provenance: these records come from heuristic undo log
            // parsing, not from pointer-verified version chain recovery.
            // They must never be treated as FLASHBACK_INSERT or authoritative
            // row history by downstream consumers.
            record["_undo_action"] = "FORENSIC_LEGACY_UNDO";
            record["_provenance"] = "heuristic_undo";

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

// ==================== New: Heuristic Record Detection ====================

std::vector<std::map<std::string, std::string>> TruncateRecovery::detect_residual_records(
    const std::string& ibd_path,
    const catalog::TableSchema& schema) {

    std::vector<std::map<std::string, std::string>> records;

    // Collect known column names from schema
    std::vector<std::string> col_names;
    if (!schema.columns.empty()) {
        for (const auto& [idx, col] : schema.columns) {
            col_names.push_back(col.name);
        }
    }

    // If we have schema, try InnoDBIndexParser first for high-quality recovery
    if (!schema.columns.empty()) {
        std::vector<innodb::IndexColumnDef> col_defs;
        uint32_t n_pk = 0;
        for (const auto& [idx, col] : schema.columns) {
            // Schema carries InnoDB parser-domain type IDs; route through the
            // fail-closed conversion boundary instead of a raw narrowing cast.
            std::vector<std::string> type_diag;
            auto converted = catalog::build_index_column(
                col,
                /*ordinal_position=*/col.ordinal,
                /*physical_pos_override=*/0,
                catalog::TypeIdDomain::kInnodbParser,
                type_diag);
            if (!converted.has_value()) {
                for (const auto& d : type_diag) {
                    std::cerr << "[WARN] TRUNCATE schema column rejected: " << d << "\n";
                }
                continue;
            }
            innodb::IndexColumnDef def = std::move(*converted);
            // Preserve prior binary/mb_maxlen refinements.
            if (col.charset_id == 63) def.mb_maxlen = 1;
            else if (col.charset_id == 8 || col.charset_id == 83) def.mb_maxlen = 1;
            if (def.is_variable_length()) def.is_big = (def.col_length * def.mb_maxlen > 255);
            def.is_binary = def.is_binary && (def.col_type != 31);
            col_defs.push_back(std::move(def));
        }
        if (n_pk == 0 && !col_defs.empty()) n_pk = 1;

        innodb::InnoDBIndexParser parser;
        parser.set_columns(col_defs, false, n_pk);
        parser.set_recover_deleted(true);
        parser.set_bad_page_mode(2);  // Try mode for truncated pages

        auto index_records = parser.parse_ibd_force(ibd_path);
        if (!index_records.empty()) {
            for (const auto& rec : index_records) {
                std::map<std::string, std::string> record;
                for (size_t i = 0; i < rec.fields.size() && i < col_names.size(); i++) {
                    record[col_names[i]] = rec.fields[i].str_value;
                }
                if (record.size() >= 2) {
                    records.push_back(record);
                }
            }
            if (!records.empty()) {
                std::cout << "[INFO] Detected " << records.size()
                          << " records via InnoDBIndexParser (schema-aware)\n";
                return records;
            }
        }
    }

    /**
     * PhotoRec-style residual data detection:
     *
     * 1. Scan for record header signatures even in zeroed pages
     * 2. Look for valid field length prefixes
     * 3. Detect printable string patterns
     * 4. Validate record structure
     *
     * This approach works because:
     * - TRUNCATE zeroes pages but may miss some sectors
     * - SSD wear leveling may preserve old data
     * - File system may not overwrite all blocks
     */

    std::ifstream file(ibd_path, std::ios::binary);
    if (!file) {
        return records;
    }

    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::cout << "[INFO] Scanning for residual records (PhotoRec style)...\n";

    // Read entire file in chunks
    const size_t chunk_size = 1024 * 1024;  // 1MB chunks
    std::vector<uint8_t> chunk(chunk_size);

    uint64_t offset = 0;
    uint32_t potential_records = 0;

    // Known patterns for InnoDB records
    // Record headers typically: [info_bits][next_offset][heap_no][n_fields]
    // COMPACT: 5 bytes, REDUNDANT: 6 bytes

    std::set<std::string> seen_hashes;

    while (offset < file_size) {
        size_t read_size = std::min(chunk_size, static_cast<size_t>(file_size - offset));
        file.read(reinterpret_cast<char*>(chunk.data()), read_size);

        if (!file) break;

        // Scan for potential record starts
        for (size_t i = 0; i < read_size - 20; i++) {
            // Look for potential record header patterns
            uint8_t b0 = chunk[i];
            uint8_t b1 = chunk[i + 1];

            // COMPACT format check: info_bits should be 0x00, 0x01, 0x02
            // and next_offset should be reasonable (non-zero, not too large)
            if (b0 <= 0x02) {
                uint16_t next_off = (static_cast<uint16_t>(b1) << 8) | chunk[i + 2];

                // Valid next_offset range: 5-400 for typical records
                if (next_off >= 5 && next_off <= 400) {
                    // Check if there's potential field data following
                    size_t data_start = i + 5;  // Skip COMPACT header

                    // Look for length-encoded string or int data
                    if (data_start + 10 < read_size) {
                        std::vector<uint8_t> potential_data(
                            chunk.begin() + data_start,
                            chunk.begin() + std::min(data_start + next_off, read_size));

                        // Check if data looks valid (not all zeros, has printable chars)
                        int printable = 0;
                        int non_zero = 0;
                        for (uint8_t b : potential_data) {
                            if (b != 0) non_zero++;
                            if (b >= 32 && b <= 126) printable++;
                        }

                        // If at least 50% printable and non-zero, might be valid record
                        if (non_zero > potential_data.size() * 0.3 &&
                            printable > potential_data.size() * 0.2) {

                            // Extract potential record
                            std::map<std::string, std::string> record;

                            // Try to parse as length-encoded fields
                            size_t pos = 0;
                            size_t col_idx = 0;
                            bool valid_record = true;

                            while (pos < potential_data.size() && col_idx < 20) {
                                uint8_t len = potential_data[pos];

                                // Valid length: 1-255
                                if (len == 0 || len > potential_data.size() - pos - 1) {
                                    break;
                                }

                                // Check if content looks valid
                                std::string value;
                                for (size_t j = 0; j < len && pos + 1 + j < potential_data.size(); j++) {
                                    uint8_t c = potential_data[pos + 1 + j];
                                    if (c >= 32 && c <= 126) {
                                        value += static_cast<char>(c);
                                    } else if (c == 0) {
                                        // Null byte in string - might be binary data
                                        value += "\\x00";
                                    } else {
                                        value += "?";
                                    }
                                }

                                if (!value.empty()) {
                                    std::string col_name;
                                    if (col_idx < col_names.size()) {
                                        col_name = col_names[col_idx];
                                    } else {
                                        col_name = "col_" + std::to_string(col_idx);
                                    }
                                    record[col_name] = value;
                                }

                                pos += len + 1;
                                col_idx++;
                            }

                            // Add record if we found at least 2 fields
                            if (record.size() >= 2) {
                                // Create hash for deduplication
                                std::string hash;
                                for (const auto& [k, v] : record) {
                                    hash += k + "=" + v + "|";
                                }

                                if (seen_hashes.find(hash) == seen_hashes.end()) {
                                    seen_hashes.insert(hash);
                                    records.push_back(record);
                                    potential_records++;
                                }
                            }
                        }
                    }
                }
            }
        }

        offset += read_size;

        // Limit to prevent excessive memory usage
        if (records.size() >= 10000) {
            std::cout << "[INFO] Found 10000+ residual records, stopping scan\n";
            break;
        }
    }

    std::cout << "[INFO] Detected " << potential_records << " potential residual records\n";

    return records;
}

} // namespace recovery
} // namespace dbrecovery