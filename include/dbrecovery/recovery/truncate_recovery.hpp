/**
 * @file truncate_recovery.hpp
 * @brief TRUNCATE TABLE Recovery Interface
 *
 * Provides recovery capabilities for tables that have been TRUNCATEd.
 * Uses multiple recovery strategies:
 * 1. Undo log parsing (MySQL 5.7 style TRUNCATE)
 * 2. Disk carving (PhotoRec style)
 * 3. Orphan page scanning
 */

#ifndef DBRECOVERY_RECOVERY_TRUNCATE_RECOVERY_HPP
#define DBRECOVERY_RECOVERY_TRUNCATE_RECOVERY_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <chrono>
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace recovery {

/**
 * @brief TRUNCATE recovery result
 */
struct TruncateRecoveryResult {
    bool success;
    uint64_t records_recovered;
    std::string method_used;  // "undo_log", "disk_carving", "orphan_pages", or combination
    double recovery_time_seconds;
    std::vector<std::map<std::string, std::string>> recovered_records;
    std::vector<std::string> errors;
};

/**
 * @brief TruncateRecovery - TRUNCATE TABLE disaster recovery
 *
 * Implements multi-strategy recovery for TRUNCATEd tables:
 *
 * Strategy 1: Undo Log Recovery
 * - MySQL 5.7 TRUNCATE generates undo logs
 * - Parse rollback segments from ibdata1
 * - Reconstruct records from undo log entries
 *
 * Strategy 2: Disk Carving (PhotoRec style)
 * - Scan file for INDEX page signatures
 * - Extract records from found pages
 * - Works even when file structure is damaged
 *
 * Strategy 3: Orphan Page Scanning
 * - Find pages allocated but never written
 * - Look for residual data in zeroed pages
 * - Check LSN mismatches for older page versions
 */
class TruncateRecovery {
public:
    /**
     * @brief Recovery result structure
     */
    using Result = TruncateRecoveryResult;

    /**
     * @brief Constructor
     */
    TruncateRecovery();

    /**
     * @brief Destructor
     */
    ~TruncateRecovery();

    // Disable copy
    TruncateRecovery(const TruncateRecovery&) = delete;
    TruncateRecovery& operator=(const TruncateRecovery&) = delete;

    /**
     * @brief Set page size
     * @param page_size Page size (4096/8192/16384/32768/65536)
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief Set system tablespace path (for undo log recovery)
     * @param path Path to ibdata1 or mysql.ibd
     */
    void set_system_tablespace(const std::string& path);

    /**
     * @brief Enable/disable disk scanning
     * @param enable true to enable
     */
    void enable_disk_scan(bool enable) { scan_disk_ = enable; }

    /**
     * @brief Enable/disable undo log parsing
     * @param enable true to enable
     */
    void enable_undo_log(bool enable) { use_undo_log_ = enable; }

    /**
     * @brief Check if a file appears to be truncated
     * @param ibd_path Path to .ibd file
     * @return true if file appears truncated
     */
    bool is_truncated_file(const std::string& ibd_path);

    /**
     * @brief Perform recovery
     *
     * @param ibd_path Path to truncated .ibd file
     * @param schema Table schema (if known)
     * @param progress Progress callback (0.0-1.0, message)
     * @return Recovery result
     */
    Result recover(
        const std::string& ibd_path,
        const catalog::TableSchema& schema,
        std::function<void(double, const std::string&)> progress = nullptr);

private:
    /**
     * @brief Recover records from undo log
     */
    std::vector<std::map<std::string, std::string>> recover_from_undo_log(
        const std::string& ibdata_path,
        const catalog::TableSchema& schema);

    /**
     * @brief Recover records using disk carving (PhotoRec style)
     */
    std::vector<std::map<std::string, std::string>> recover_from_disk_carving(
        const std::string& ibd_path,
        const catalog::TableSchema& schema);

    /**
     * @brief Recover records from orphan pages
     */
    std::vector<std::map<std::string, std::string>> recover_from_orphan_pages(
        const std::string& ibd_path,
        const catalog::TableSchema& schema);

    uint32_t page_size_;
    std::string system_tablespace_;
    bool scan_disk_;
    bool use_undo_log_;
    std::set<std::string> seen_primary_keys_;  // For deduplication
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_TRUNCATE_RECOVERY_HPP