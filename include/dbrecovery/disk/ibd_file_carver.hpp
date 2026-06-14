/**
 * @file ibd_file_carver.hpp
 * @brief IBD File Carver - PhotoRec-style file recovery
 *
 * Carves deleted .ibd files from raw disk images using InnoDB page signatures.
 * Based on TestDisk/PhotoRec file carving approach.
 *
 * Key features:
 * - Signature-based IBD file detection
 * - Multi-page aggregation for complete file recovery
 * - Checksum validation for data integrity
 * - Space ID tracking for file boundary detection
 */

#ifndef DBRECOVERY_DISK_IBD_FILE_CARVER_HPP
#define DBRECOVERY_DISK_IBD_FILE_CARVER_HPP

#include "dbrecovery/engine/raw_disk_engine.hpp"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>
#include <cstdint>

namespace dbrecovery {
namespace disk {

// ==================== IBD Page Signatures ====================

/**
 * @brief InnoDB page signatures for file carving
 */
namespace signatures {
    // Page types (offset 24-25, big-endian)
    constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED = 0x0000;  // Freshly allocated
    constexpr uint16_t FIL_PAGE_TYPE_UNDO_LOG  = 0x0002;  // Undo log page
    constexpr uint16_t FIL_PAGE_TYPE_INODE     = 0x0003;  // Index node
    constexpr uint16_t FIL_PAGE_TYPE_IBUF_BITMAP = 0x0005;  // Insert buffer bitmap
    constexpr uint16_t FIL_PAGE_TYPE_SYS       = 0x0006;  // System page
    constexpr uint16_t FIL_PAGE_TYPE_TRX_SYS   = 0x0007;  // Transaction system data
    constexpr uint16_t FIL_PAGE_TYPE_FSP_HDR   = 0x0008;  // File space header
    constexpr uint16_t FIL_PAGE_TYPE_XDES      = 0x0009;  // Extent descriptor
    constexpr uint16_t FIL_PAGE_TYPE_BLOB      = 0x000A;  // Uncompressed BLOB page
    constexpr uint16_t FIL_PAGE_TYPE_ZBLOB     = 0x000B;  // Compressed BLOB page
    constexpr uint16_t FIL_PAGE_TYPE_ZBLOB2    = 0x000C;  // Compressed BLOB page (new)
    constexpr uint16_t FIL_PAGE_TYPE_SDI       = 0x45BD;  // SDI page (MySQL 8.0+)
    constexpr uint16_t FIL_PAGE_TYPE_INDEX     = 0x45BF;  // INDEX page (17855)

    // Common patterns in IBD files
    // "infimum" marker at offset 0xC063 (page 4, record header area)
    constexpr const char* INFIMUM_MARKER = "infimum";
    constexpr const char* SUPREMUM_MARKER = "supremum";
    constexpr size_t INFIMUM_OFFSET = 0xC063;
    constexpr size_t SUPREMUM_OFFSET = 0xC070;

    // FIL header structure
    constexpr size_t FIL_PAGE_SPACE_OR_CHKSUM = 0;   // 4 bytes
    constexpr size_t FIL_PAGE_OFFSET = 4;             // 4 bytes - page number
    constexpr size_t FIL_PAGE_PREV = 8;               // 4 bytes
    constexpr size_t FIL_PAGE_NEXT = 12;              // 4 bytes
    constexpr size_t FIL_PAGE_LSN = 16;               // 8 bytes
    constexpr size_t FIL_PAGE_TYPE = 24;              // 2 bytes
    constexpr size_t FIL_PAGE_FILE_FLUSH_LSN = 26;    // 8 bytes
    constexpr size_t FIL_PAGE_SPACE_ID = 34;          // 4 bytes
}

// ==================== Found IBD Page ====================

/**
 * @brief Represents a found IBD page on disk
 */
struct FoundIbdPage {
    uint64_t disk_offset;        // Offset on disk
    uint32_t page_number;        // Page number within tablespace
    uint32_t space_id;           // Tablespace ID
    uint16_t page_type;          // Page type
    uint64_t lsn;                // Log sequence number
    uint32_t prev_page;          // Previous page in chain
    uint32_t next_page;          // Next page in chain
    uint32_t page_size;          // Page size
    bool checksum_valid;         // Is checksum valid
    bool is_compressed;          // Is page compressed

    // For INDEX pages
    uint64_t index_id;           // Index ID (if INDEX page)
    uint16_t level;              // B-tree level
    uint16_t n_recs;             // Number of records

    // Raw data
    std::vector<uint8_t> data;
};

// ==================== Carved IBD File ====================

/**
 * @brief Represents a carved (recovered) IBD file
 */
struct CarvedIbdFile {
    uint64_t disk_offset;        // Starting offset on disk
    uint64_t file_size;          // Estimated file size
    uint32_t space_id;           // Tablespace ID
    uint32_t page_size;          // Page size
    uint32_t num_pages;          // Number of pages recovered

    // Page statistics
    uint32_t index_pages;
    uint32_t blob_pages;
    uint32_t sdi_pages;
    uint32_t fsp_pages;

    // Root page info
    uint32_t root_page_number;
    uint64_t root_index_id;

    // Recovery confidence
    double confidence;           // 0.0 - 1.0
    bool is_complete;            // All pages found
    bool is_fragmented;          // Pages not contiguous

    // File data
    std::vector<uint8_t> data;

    // Page map: page_number -> disk_offset
    std::map<uint32_t, uint64_t> page_map;

    // Errors/warnings
    std::vector<std::string> warnings;
};

// ==================== Carving Configuration ====================

/**
 * @brief Configuration for IBD file carving
 */
struct CarverConfig {
    // Scan parameters
    uint64_t start_offset = 0;
    uint64_t end_offset = 0;          // 0 = scan entire device

    // Page sizes to scan
    std::vector<uint32_t> page_sizes = {4096, 8192, 16384, 32768, 65536};

    // Minimum file requirements
    uint32_t min_pages = 2;          // Minimum pages to consider a file
    uint64_t min_file_size = 16384;  // Minimum file size

    // Validation options
    bool validate_checksums = true;
    bool require_fsp_header = false;  // Require FSP_HDR page
    bool require_index_pages = true;  // Require INDEX pages

    // Recovery options
    bool recover_data = true;        // Extract records from pages
    bool aggregate_pages = true;     // Try to aggregate pages into files
    bool follow_page_chain = true;   // Follow prev/next page links

    // Performance options
    uint32_t buffer_size = 1024 * 1024;  // 1MB buffer
    uint32_t num_threads = 4;
    bool use_direct_io = true;
};

// ==================== Carving Statistics ====================

/**
 * @brief Statistics from carving operation
 */
struct CarverStats {
    uint64_t total_bytes_scanned;
    uint64_t pages_examined;
    uint64_t valid_pages_found;
    uint64_t index_pages_found;
    uint64_t blob_pages_found;
    uint64_t fsp_pages_found;

    uint32_t files_found;
    uint32_t complete_files;
    uint32_t fragmented_files;

    uint64_t total_bytes_recovered;
    double scan_time_seconds;
    double throughput_mbps;
};

// ==================== IBD File Carver ====================

/**
 * @brief PhotoRec-style IBD file carver
 *
 * Scans raw disk for IBD file signatures and reconstructs files.
 */
class IbdFileCarver {
public:
    /**
     * @brief Construct carver
     * @param engine Raw disk engine for scanning
     */
    explicit IbdFileCarver(engine::RawDiskIOEngine& engine);

    /**
     * @brief Carve IBD files from disk
     * @param config Carving configuration
     * @param progress Progress callback (0.0 - 1.0)
     * @return List of carved files
     */
    std::vector<CarvedIbdFile> carve(
        const CarverConfig& config,
        std::function<void(double, const std::string&)> progress = nullptr);

    /**
     * @brief Scan for IBD pages only (no file aggregation)
     * @param config Configuration
     * @param callback Called for each found page
     * @return Statistics
     */
    CarverStats scan_pages(
        const CarverConfig& config,
        std::function<void(const FoundIbdPage&)> callback);

    /**
     * @brief Find files by space_id
     * @param space_id Tablespace ID to search for
     * @param config Configuration
     * @return Found files
     */
    std::vector<CarvedIbdFile> find_by_space_id(
        uint32_t space_id, const CarverConfig& config);

    /**
     * @brief Find files by index_id
     * @param index_id Index ID to search for
     * @param config Configuration
     * @return Found files
     */
    std::vector<CarvedIbdFile> find_by_index_id(
        uint64_t index_id, const CarverConfig& config);

    /**
     * @brief Get carving statistics
     */
    const CarverStats& get_stats() const { return stats_; }

    /**
     * @brief Export carved file to disk
     * @param file Carved file
     * @param output_path Output path
     * @return True if successful
     */
    static bool export_file(const CarvedIbdFile& file, const std::string& output_path);

private:
    engine::RawDiskIOEngine& engine_;
    CarverStats stats_;

    /**
     * @brief Check if data looks like IBD page
     */
    static bool is_ibd_page(const uint8_t* data, uint32_t page_size);

    /**
     * @brief Parse page header
     */
    static FoundIbdPage parse_page_header(const uint8_t* data, uint32_t page_size, uint64_t offset);

    /**
     * @brief Validate page checksum
     */
    static bool validate_checksum(const uint8_t* data, uint32_t page_size);

    /**
     * @brief Aggregate pages into files by space_id
     */
    std::vector<CarvedIbdFile> aggregate_by_space_id(
        const std::vector<FoundIbdPage>& pages, const CarverConfig& config);

    /**
     * @brief Follow page chain to find related pages
     */
    std::vector<FoundIbdPage> follow_chain(
        const FoundIbdPage& start_page, const CarverConfig& config);

    /**
     * @brief Calculate recovery confidence
     */
    double calculate_confidence(const CarvedIbdFile& file);
};

// ==================== IBD Signature Detector ====================

/**
 * @brief Detects IBD file signatures in raw data
 *
 * Used for quick signature scanning without full parsing.
 */
class IbdSignatureDetector {
public:
    /**
     * @brief Detection result
     */
    struct Detection {
        uint64_t offset;
        uint32_t page_size;
        uint16_t page_type;
        uint32_t space_id;
        uint64_t lsn;
        bool is_valid;
    };

    /**
     * @brief Detect IBD signatures in buffer
     * @param data Buffer data
     * @param size Buffer size
     * @param page_size Page size to check
     * @return List of detections
     */
    static std::vector<Detection> detect(
        const uint8_t* data, size_t size, uint32_t page_size);

    /**
     * @brief Check for FSP_HDR signature
     */
    static bool is_fsp_header(const uint8_t* data, uint32_t page_size);

    /**
     * @brief Check for INDEX signature
     */
    static bool is_index_page(const uint8_t* data, uint32_t page_size);

    /**
     * @brief Check for infimum/supremum markers
     */
    static bool has_infimum_supremum(const uint8_t* data, uint32_t page_size);

    /**
     * @brief Extract space_id from page
     */
    static uint32_t extract_space_id(const uint8_t* data, uint32_t page_size);

    /**
     * @brief Extract page_type from page
     */
    static uint16_t extract_page_type(const uint8_t* data);
};

// ==================== File Boundary Detector ====================

/**
 * @brief Detects IBD file boundaries
 *
 * Uses heuristics to find where one IBD file ends and another begins.
 */
class FileBoundaryDetector {
public:
    /**
     * @brief Detected boundary
     */
    struct Boundary {
        uint64_t start_offset;
        uint64_t end_offset;
        uint32_t space_id;
        uint32_t page_size;
        uint32_t num_pages;
        bool is_valid;
    };

    /**
     * @brief Detect file boundaries from page list
     * @param pages Found pages
     * @param page_size Page size
     * @return Detected boundaries
     */
    static std::vector<Boundary> detect(
        const std::vector<FoundIbdPage>& pages, uint32_t page_size);

    /**
     * @brief Check if two pages belong to same file
     */
    static bool same_file(const FoundIbdPage& a, const FoundIbdPage& b);

    /**
     * @brief Estimate file size from pages
     */
    static uint64_t estimate_size(const std::vector<FoundIbdPage>& pages);
};

} // namespace disk
} // namespace dbrecovery

#endif // DBRECOVERY_DISK_IBD_FILE_CARVER_HPP
