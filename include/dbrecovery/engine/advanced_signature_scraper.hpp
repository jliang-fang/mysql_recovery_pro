/**
 * @file advanced_signature_scraper.hpp
 * @brief Advanced Signature-Based Page Scraper for MySQL 5.6 extreme recovery
 *
 * Implements PhotoRec-style file carving with two-level signature indexing:
 * - Level 1: Index by offset (page type at offset 24:26)
 * - Level 2: Index by first byte value (256 buckets)
 *
 * Key features:
 * - InnoDB page signature library for MySQL 5.6/5.7/8.0
 * - Sliding window scanning for misaligned pages
 * - Dual checksum validation (CRC32C + INNODB)
 * - Page aggregation into files
 *
 * @version 2.2.0
 */

#ifndef DBRECOVERY_ENGINE_ADVANCED_SIGNATURE_SCRAPER_HPP
#define DBRECOVERY_ENGINE_ADVANCED_SIGNATURE_SCRAPER_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <unordered_map>
#include <map>
#include <set>

#include "dbrecovery/engine/enhanced_raw_disk_engine.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace engine {

// ==================== MySQL Version Detection ====================

/**
 * @brief MySQL version for signature matching
 */
enum class MySQLVersion {
    UNKNOWN = 0,
    MYSQL_51 = 51,      // MySQL 5.1
    MYSQL_55 = 55,      // MySQL 5.5
    MYSQL_56 = 56,      // MySQL 5.6
    MYSQL_57 = 57,      // MySQL 5.7
    MYSQL_80 = 80,      // MySQL 8.0
    MYSQL_84 = 84,      // MySQL 8.4
    MYSQL_90 = 90       // MySQL 9.0
};

/**
 * @brief Convert MySQLVersion to string
 */
inline std::string mysql_version_to_string(MySQLVersion v) {
    switch (v) {
        case MySQLVersion::MYSQL_51: return "5.1";
        case MySQLVersion::MYSQL_55: return "5.5";
        case MySQLVersion::MYSQL_56: return "5.6";
        case MySQLVersion::MYSQL_57: return "5.7";
        case MySQLVersion::MYSQL_80: return "8.0";
        case MySQLVersion::MYSQL_84: return "8.4";
        case MySQLVersion::MYSQL_90: return "9.0";
        default: return "UNKNOWN";
    }
}

// ==================== Checksum Algorithm ====================

/**
 * @brief InnoDB checksum algorithm type
 */
enum class ChecksumAlgo {
    NONE = 0,           // No checksum
    INNODB = 1,         // Legacy InnoDB checksum (ut_fold_binary)
    CRC32C = 2,         // CRC32C (MySQL 5.7+ default)
    STRICT_CRC32C = 3,  // Strict CRC32C
    STRICT_INNODB = 4,  // Strict InnoDB
    UNKNOWN = 5         // Unknown algorithm
};

/**
 * @brief Convert ChecksumAlgo to string
 */
inline std::string checksum_algo_to_string(ChecksumAlgo algo) {
    switch (algo) {
        case ChecksumAlgo::NONE: return "NONE";
        case ChecksumAlgo::INNODB: return "INNODB";
        case ChecksumAlgo::CRC32C: return "CRC32C";
        case ChecksumAlgo::STRICT_CRC32C: return "STRICT_CRC32C";
        case ChecksumAlgo::STRICT_INNODB: return "STRICT_INNODB";
        default: return "UNKNOWN";
    }
}

// ==================== Page Signature Definitions ====================

/**
 * @brief Page signature definition for pattern matching
 */
struct PageSignatureDef {
    std::string name;                   ///< Signature name
    uint16_t page_type;                 ///< FIL_PAGE_TYPE value
    uint32_t signature_offset;          ///< Offset where signature appears
    std::vector<uint8_t> signature;     ///< Signature bytes
    std::vector<uint8_t> mask;          ///< Mask for matching (optional)
    double confidence_weight;           ///< Weight for confidence calculation
    MySQLVersion min_version;           ///< Minimum MySQL version
    MySQLVersion max_version;           ///< Maximum MySQL version

    /**
     * @brief Check if signature matches at given offset
     */
    bool matches(const uint8_t* data, size_t size) const;
};

// ==================== InnoDB Signature Library ====================

/**
 * @brief Library of InnoDB page signatures
 *
 * Contains signatures for all InnoDB page types across MySQL versions.
 */
class InnoDBSignatureLibrary {
public:
    // ==================== Page Type Constants ====================

    /// @brief FIL_PAGE_TYPE values (from ibd2sql)
    static constexpr uint16_t FIL_PAGE_INDEX = 0x45BF;       // 17855
    static constexpr uint16_t FIL_PAGE_UNDO_LOG = 0x45B2;    // 17842
    static constexpr uint16_t FIL_PAGE_INODE = 0x45B3;       // 17843
    static constexpr uint16_t FIL_PAGE_IBUF_FREE_LIST = 0x45B4; // 17844
    static constexpr uint16_t FIL_PAGE_IBUF_BITMAP = 0x45B5; // 17845
    static constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED = 0x0000; // Freshly allocated
    static constexpr uint16_t FIL_PAGE_SDI = 0x45BE;         // 17854 (MySQL 8.0+)
    static constexpr uint16_t FIL_PAGE_BLOB = 0x45BD;        // 17853
    static constexpr uint16_t FIL_PAGE_COMPRESSED = 0x000E;  // Compressed page
    static constexpr uint16_t FIL_PAGE_ENCRYPTED = 0x000F;   // Encrypted page

    // ==================== Signature Offset Constants ====================

    /// @brief FIL header offsets
    static constexpr uint32_t FIL_HEADER_OFFSET = 0;
    static constexpr uint32_t FIL_PAGE_TYPE_OFFSET = 24;     // Page type at bytes 24:26
    static constexpr uint32_t FIL_SPACE_ID_OFFSET = 34;      // Space ID at bytes 34:38
    static constexpr uint32_t FIL_PAGE_NUMBER_OFFSET = 4;    // Page number at bytes 4:8

    /// @brief PAGE header offsets (after FIL header at 38)
    static constexpr uint32_t PAGE_HEADER_OFFSET = 38;
    static constexpr uint32_t PAGE_N_DIR_SLOTS_OFFSET = 0;   // Directory slots count
    static constexpr uint32_t PAGE_HEAP_TOP_OFFSET = 2;      // Heap top position
    static constexpr uint32_t PAGE_N_HEAP_OFFSET = 4;        // Heap size
    static constexpr uint32_t PAGE_FREE_OFFSET = 6;          // Free list head
    static constexpr uint32_t PAGE_LEVEL_OFFSET = 12;        // B-tree level
    static constexpr uint32_t PAGE_INDEX_ID_OFFSET = 14;     // Index ID (8 bytes)

    /// @brief Record offsets
    static constexpr uint32_t INFIMUM_OFFSET = 99;           // Infimum record at offset 99
    static constexpr uint32_t SUPREMUM_OFFSET = 112;         // Supremum record at offset 112

    // ==================== Signature Retrieval ====================

    /**
     * @brief Get all signatures for MySQL 5.6
     */
    static std::vector<PageSignatureDef> get_mysql56_signatures();

    /**
     * @brief Get all signatures for MySQL 5.7
     */
    static std::vector<PageSignatureDef> get_mysql57_signatures();

    /**
     * @brief Get all signatures for MySQL 8.0
     */
    static std::vector<PageSignatureDef> get_mysql80_signatures();

    /**
     * @brief Get all signatures for specified version
     */
    static std::vector<PageSignatureDef> get_signatures(MySQLVersion version);

    /**
     * @brief Get signature for specific page type
     */
    static std::optional<PageSignatureDef> get_signature(uint16_t page_type, MySQLVersion version);

    // ==================== Signature Matching ====================

    /**
     * @brief Match signature against data
     * @param data Data buffer
     * @param size Buffer size
     * @param sig Signature to match
     * @return Confidence score (0.0-1.0)
     */
    static double match_signature(const uint8_t* data, size_t size, const PageSignatureDef& sig);

    /**
     * @brief Detect page type from data
     * @param data Page data
     * @param size Data size
     * @return Page type, or 0xFFFF if unknown
     */
    static uint16_t detect_page_type(const uint8_t* data, size_t size);

    /**
     * @brief Detect MySQL version from page data
     * @param page_data Page data (must be at least 100 bytes)
     * @param page_size Page size
     * @return Detected version
     */
    static MySQLVersion detect_version(const uint8_t* page_data, size_t page_size);

    // ==================== Page Validation ====================

    /**
     * @brief Check if page type is valid
     */
    static bool is_valid_page_type(uint16_t type);

    /**
     * @brief Check if page type is INDEX
     */
    static bool is_index_page(uint16_t type) {
        return type == FIL_PAGE_INDEX;
    }

    /**
     * @brief Check if page type is BLOB
     */
    static bool is_blob_page(uint16_t type) {
        return type == FIL_PAGE_BLOB;
    }

    /**
     * @brief Check if page type is SDI
     */
    static bool is_sdi_page(uint16_t type) {
        return type == FIL_PAGE_SDI;
    }

    /**
     * @brief Check if page type is UNDO
     */
    static bool is_undo_page(uint16_t type) {
        return type == FIL_PAGE_UNDO_LOG;
    }

    /**
     * @brief Check if page is leaf page (level 0)
     */
    static bool is_leaf_page(const uint8_t* page_data, size_t page_size);

    /**
     * @brief Check if page is root page (level > 0 with specific markers)
     */
    static bool is_root_page(const uint8_t* page_data, size_t page_size);

    // ==================== Page Header Parsing ====================

    /**
     * @brief Parse FIL header from page data
     */
    struct FILHeader {
        uint32_t checksum1;         // Bytes 0:4
        uint32_t page_number;       // Bytes 4:8
        uint32_t prev_page;         // Bytes 8:12 (FIL_PAGE_PREV)
        uint32_t next_page;         // Bytes 12:16 (FIL_PAGE_NEXT)
        uint64_t lsn;               // Bytes 16:24 (FIL_PAGE_LSN)
        uint16_t page_type;         // Bytes 24:26 (FIL_PAGE_TYPE)
        uint32_t space_id;          // Bytes 34:38 (FIL_PAGE_SPACE_ID)
    };

    static std::optional<FILHeader> parse_fil_header(const uint8_t* data, size_t size);

    /**
     * @brief Parse PAGE header from page data
     */
    struct PAGEHeader {
        uint16_t n_dir_slots;       // Number of directory slots
        uint16_t heap_top;          // Heap top position
        uint16_t n_heap;            // Heap size (with flags)
        uint16_t free;              // Free list head offset
        uint16_t garbage;           // Garbage space
        uint16_t level;             // B-tree level
        uint64_t index_id;          // Index ID
    };

    static std::optional<PAGEHeader> parse_page_header(const uint8_t* data, size_t size);

private:
    // Signature cache
    static std::unordered_map<MySQLVersion, std::vector<PageSignatureDef>> signature_cache_;
};

// ==================== Carved Page Structure ====================

/**
 * @brief Carved page from disk scanning
 */
struct CarvedPage {
    uint64_t disk_offset;           ///< Offset in disk/image
    uint32_t page_size;             ///< Page size (4K/8K/16K/32K/64K)
    uint16_t page_type;             ///< FIL_PAGE_TYPE
    uint32_t page_number;           ///< Page number from header
    uint32_t space_id;              ///< Tablespace ID
    uint64_t lsn;                   ///< Log Sequence Number
    double confidence;              ///< Confidence score (0.0-1.0)
    std::vector<uint8_t> data;      ///< Full page data

    // Checksum validation results
    bool checksum_valid;            ///< Whether checksum passed
    ChecksumAlgo checksum_algorithm;///< Algorithm used for validation
    uint32_t checksum_stored;       ///< Stored checksum value
    uint32_t checksum_computed;     ///< Computed checksum value

    // Additional metadata
    uint64_t index_id;              ///< Index ID (for INDEX pages)
    uint16_t page_level;            ///< B-tree level (for INDEX pages)
    bool is_leaf;                   ///< Is leaf page
    bool is_deleted;                ///< Has deleted records

    // Deleted record offsets (from free list)
    std::vector<uint16_t> deleted_record_offsets;

    /**
     * @brief Get page type name
     */
    std::string get_page_type_name() const;
};

// ==================== Carved File Structure ====================

/**
 * @brief Aggregated file from carved pages
 */
struct CarvedFile {
    uint64_t disk_offset;           ///< Starting offset in disk
    uint32_t page_size;             ///< Page size
    uint32_t space_id;              ///< Tablespace ID
    uint32_t num_pages;             ///< Number of pages found
    std::vector<uint32_t> page_numbers; ///< Page numbers found
    std::vector<uint8_t> data;      ///< Aggregated data
    double confidence;              ///< Overall confidence
    bool is_complete;               ///< All pages in sequence found

    // Index information
    uint64_t index_id;              ///< Primary index ID
    uint32_t root_page;             ///< Root page number (if found)
    uint32_t leaf_pages;            ///< Number of leaf pages
    uint32_t non_leaf_pages;        ///< Number of non-leaf pages

    // Recovery metadata
    std::string suggested_filename; ///< Suggested .ibd filename
    bool has_sdi;                   ///< Has SDI pages (MySQL 8.0+)
    MySQLVersion detected_version;  ///< Detected MySQL version
};

// ==================== Carving Configuration ====================

/**
 * @brief Configuration for page carving
 */
struct CarvingConfig {
    std::vector<uint32_t> page_sizes = {4096, 8192, 16384, 32768, 65536};
    bool validate_checksums = true;
    bool strict_checksum = false;       ///< Reject pages with invalid checksum
    bool extract_deleted_records = true;
    bool recover_fragments = true;      ///< Recover partial pages
    uint32_t min_pages_per_file = 2;    ///< Minimum pages to form a file
    double signature_threshold = 0.8;   ///< Minimum confidence threshold
    MySQLVersion target_version = MySQLVersion::MYSQL_56;
    bool use_two_level_index = true;    ///< Use PhotoRec-style indexing
    uint32_t max_pages = 0;             ///< Maximum pages to find (0 = unlimited)
};

// ==================== Two-Level Signature Index ====================

/**
 * @brief PhotoRec-style two-level signature index
 *
 * Level 1: Index by offset (page type at 24:26)
 * Level 2: Index by first byte value (256 buckets)
 */
class TwoLevelSignatureIndex {
public:
    /**
     * @brief Initialize index with signatures
     */
    explicit TwoLevelSignatureIndex(const std::vector<PageSignatureDef>& signatures);

    /**
     * @brief Lookup signatures by offset and first byte
     * @param offset Signature offset
     * @param first_byte First byte value at that offset
     * @return Matching signatures
     */
    std::vector<const PageSignatureDef*> lookup(uint32_t offset, uint8_t first_byte) const;

    /**
     * @brief Get all signatures for an offset
     */
    std::vector<const PageSignatureDef*> lookup_by_offset(uint32_t offset) const;

    /**
     * @brief Check if offset has signatures
     */
    bool has_signatures_at(uint32_t offset) const;

private:
    // Level 1: offset -> Level 2 index
    std::map<uint32_t, std::array<std::vector<const PageSignatureDef*>, 256>> index_;

    // All signatures
    std::vector<PageSignatureDef> signatures_;
};

// ==================== Advanced Page Carver ====================

/**
 * @brief Advanced page carver with PhotoRec-style scanning
 */
class AdvancedPageCarver {
public:
    /**
     * @brief Construct with disk engine
     */
    explicit AdvancedPageCarver(EnhancedRawDiskEngine& engine);
    ~AdvancedPageCarver();

    // ==================== Carving Operations ====================

    /**
     * @brief Carve all pages from disk with configuration
     *
     * @param config Carving configuration
     * @param callback Called for each found page
     * @return Total pages found
     */
    uint32_t carve_pages(
        const CarvingConfig& config,
        std::function<void(const CarvedPage&)> callback);

    /**
     * @brief Carve pages from a specific range
     */
    uint32_t carve_range(
        uint64_t start_offset,
        uint64_t end_offset,
        const CarvingConfig& config,
        std::function<void(const CarvedPage&)> callback);

    /**
     * @brief Carve pages of specific type
     */
    uint32_t carve_by_type(
        uint16_t page_type,
        const CarvingConfig& config,
        std::function<void(const CarvedPage&)> callback);

    // ==================== Aggregation ====================

    /**
     * @brief Aggregate carved pages into files
     *
     * Groups pages by space_id and page_size, attempts to reconstruct
     * complete .ibd files.
     *
     * @param pages Carved pages
     * @param config Configuration
     * @return Aggregated files
     */
    std::vector<CarvedFile> aggregate_to_files(
        const std::vector<CarvedPage>& pages,
        const CarvingConfig& config);

    /**
     * @brief Aggregate pages by index ID
     *
     * Groups INDEX pages by their index_id for table reconstruction.
     */
    std::vector<CarvedFile> aggregate_by_index(
        const std::vector<CarvedPage>& pages);

    // ==================== Statistics ====================

    /**
     * @brief Get carving statistics
     */
    struct CarvingStats {
        uint64_t bytes_scanned;
        uint64_t pages_found;
        uint64_t pages_valid_checksum;
        uint64_t pages_invalid_checksum;
        uint64_t index_pages;
        uint64_t blob_pages;
        uint64_t sdi_pages;
        uint64_t undo_pages;
        uint64_t allocated_pages;
        double elapsed_seconds;
        double mb_per_second;
    };
    CarvingStats get_stats() const { return stats_; }

    // ==================== Checksum Validation ====================

    /**
     * @brief Validate page checksum
     *
     * Tries CRC32C first, falls back to INNODB checksum.
     *
     * @param page_data Page data
     * @param page_size Page size
     * @param[out] algo Algorithm that matched
     * @return true if checksum valid
     */
    static bool validate_checksum(
        const uint8_t* page_data,
        uint32_t page_size,
        ChecksumAlgo& algo);

    /**
     * @brief Compute CRC32C checksum for page
     */
    static uint32_t compute_crc32c_checksum(const uint8_t* page_data, uint32_t page_size);

    /**
     * @brief Compute INNODB checksum for page
     */
    static uint32_t compute_innodb_checksum(const uint8_t* page_data, uint32_t page_size);

private:
    EnhancedRawDiskEngine& engine_;
    CarvingStats stats_;
    std::unique_ptr<TwoLevelSignatureIndex> signature_index_;

    // ==================== Internal Helpers ====================

    /**
     * @brief Sliding window scan for pages
     */
    std::vector<CarvedPage> sliding_window_scan(
        uint64_t start,
        uint64_t end,
        uint32_t page_size,
        const CarvingConfig& config);

    /**
     * @brief Check if page at offset is valid
     */
    std::optional<CarvedPage> check_page_at_offset(
        uint64_t offset,
        uint32_t page_size,
        const CarvingConfig& config);

    /**
     * @brief Extract deleted record offsets from page
     */
    std::vector<uint16_t> extract_deleted_offsets(const uint8_t* page_data, uint32_t page_size);

    /**
     * @brief Calculate page confidence score
     */
    double calculate_confidence(const uint8_t* page_data, uint32_t page_size, uint16_t page_type);
};

// ==================== Page Scanner with Progress ====================

/**
 * @brief Page scanner with progress reporting
 */
class ProgressPageScanner {
public:
    explicit ProgressPageScanner(AdvancedPageCarver& carver);

    /**
     * @brief Scan with progress callback
     */
    void scan_with_progress(
        const CarvingConfig& config,
        std::function<void(const CarvedPage&)> page_callback,
        std::function<void(double, const AdvancedPageCarver::CarvingStats&)> progress_callback);

private:
    AdvancedPageCarver& carver_;
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_ADVANCED_SIGNATURE_SCRAPER_HPP