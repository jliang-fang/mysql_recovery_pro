/**
 * @file blob_scanner.hpp
 * @brief BLOB/TEXT page scanner for recovering large object data
 */

#ifndef DBRECOVERY_INNODB_BLOB_SCANNER_HPP
#define DBRECOVERY_INNODB_BLOB_SCANNER_HPP

#include <cstdint>
#include <vector>
#include <string>
#include "dbrecovery/innodb/heuristic_parser.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== BLOB Structures ====================

/**
 * @brief BLOB page structure - represents a single BLOB storage page
 */
struct BLOBPage {
    uint32_t page_no;
    uint16_t page_type;
    uint32_t next_page;   // Next page in BLOB chain
    uint32_t prev_page;   // Previous page in BLOB chain
    uint32_t total_len;   // Total BLOB length (for first page)
    uint8_t flag;         // BLOB flag
    std::vector<uint8_t> data;  // Page payload data

    bool is_first_page() const { return (flag & 0x01) != 0; }
    bool is_last_page() const { return next_page == 0; }
};

/**
 * @brief BLOB field reference - pointer to external BLOB storage
 */
struct BLOBField {
    uint16_t field_index;     // Index in record fields
    uint32_t space_id;        // Space ID
    uint32_t page_no;         // First page number
    uint32_t offset;          // Offset within first page
    uint32_t total_len;       // Total BLOB length
    bool is_partial;          // Partial external storage
};

/**
 * @brief Recovered BLOB - fully recovered BLOB data
 */
struct RecoveredBLOB {
    uint32_t page_no;         // First page number (or source if pattern match)
    uint32_t total_length;    // Total length in bytes
    std::vector<uint8_t> data;  // BLOB data
    bool is_text;             // Is this text data?
    size_t source_offset;     // Offset in original file (for pattern matches)
};

/**
 * @brief Field type definition
 */
struct FieldType {
    uint8_t type_code;
    bool is_nullable;
    bool is_extern;       // Is stored externally (BLOB)?
    uint16_t max_length;  // Maximum length
    uint8_t type;         // Type code
    
    static FieldType create_blob(uint8_t type) {
        return {type, false, true, 0};
    }
    
    static FieldType create_inline(uint8_t type, uint16_t len) {
        return {type, false, false, len};
    }
};

/**
 * @brief BlobScanner - Scanner for InnoDB BLOB/TEXT pages
 * 
 * This class scans InnoDB IBD files to find and recover BLOB/TEXT data
 * that may be stored in separate BLOB pages.
 * 
 * Key features:
 * - Scan for FIL_PAGE_TYPE_BLOB pages
 * - Parse BLOB page headers (next/prev chain)
 * - Reconstruct BLOB data from page chains
 * - Extract BLOB pointers from row records
 * - Pattern-based BLOB detection for extreme corruption
 */
class BlobScanner {
public:
    explicit BlobScanner(uint32_t page_size = 16384)
        : page_size_(page_size) {}
    
    ~BlobScanner() = default;

    /**
     * @brief Scan IBD file for BLOB pages
     * @param file_path Path to .ibd file
     * @return Vector of BLOB pages found
     */
    std::vector<BLOBPage> scan_ibd_file(const std::string& file_path);

    /**
     * @brief Extract BLOB fields from row data
     * @param row_data Parsed row data
     * @param field_definitions Field type definitions
     * @return BLOB fields found in the row
     */
    std::vector<BLOBField> extract_blob_fields(
        const std::vector<uint8_t>& row_data,
        const std::vector<FieldType>& field_definitions);

    /**
     * @brief Read BLOB data from specific page/offset
     * @param file_path Path to .ibd file
     * @param space_id Space ID
     * @param page_no First page number
     * @param offset Offset within page
     * @param length Length to read
     * @return BLOB data bytes
     */
    std::vector<uint8_t> read_blob_from_pages(
        const std::string& file_path,
        uint32_t space_id,
        uint32_t page_no,
        uint32_t offset,
        uint32_t length);

    /**
     * @brief Read complete BLOB chain from first page
     * @param file_path Path to .ibd file
     * @param first_page First page number
     * @param total_length Expected total length
     * @return Complete BLOB data
     */
    std::vector<uint8_t> read_blob_chain(
        const std::string& file_path,
        uint32_t first_page,
        uint32_t total_length);

    /**
     * @brief Recover all BLOBs from file
     * @param file_path Path to .ibd file
     * @return All recovered BLOBs
     */
    std::vector<RecoveredBLOB> recover_all_blobs(const std::string& file_path);

    /**
     * @brief Recover records with BLOB data from secondary index
     * @param file_path Path to .ibd file
     * @param index_page_no Secondary index page number
     * @return Records with full BLOB data
     */
    std::vector<HeuristicRecord> recover_from_secondary_index(
        const std::string& file_path,
        uint32_t index_page_no);

    /**
     * @brief Scan for BLOBs using pattern matching (corruption recovery)
     * @param file_path Path to .ibd file
     * @return BLOBs found by pattern
     */
    std::vector<RecoveredBLOB> scan_for_blobs_by_pattern(const std::string& file_path);

    /**
     * @brief BLOB scanner statistics
     */
    struct ScanStats {
        uint32_t pages_scanned;
        uint32_t blob_pages_found;
        uint32_t blobs_recovered;
        uint32_t total_blob_bytes;
    };

    const ScanStats& get_stats() const { return stats_; }

private:
    uint32_t page_size_;
    ScanStats stats_{0, 0, 0, 0};

    size_t get_field_size(FieldType type, 
                         const std::vector<uint8_t>& data, 
                         size_t offset);
    bool is_ascii_or_utf8(const std::vector<uint8_t>& data);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BLOB_SCANNER_HPP
