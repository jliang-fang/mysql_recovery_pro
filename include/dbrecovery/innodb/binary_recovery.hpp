#ifndef DBRECOVERY_INNODB_BINARY_RECOVERY_HPP
#define DBRECOVERY_INNODB_BINARY_RECOVERY_HPP

/**
 * @file binary_recovery.hpp
 * @brief Low-level binary recovery module for MySQL 5.6/5.7 InnoDB
 * 
 * This module implements:
 * - Binary signature scanning to identify InnoDB Pages
 * - Zombie tuple extraction (ignoring delete-mark bit)
 * - B+Tree chain reconstruction
 * - Heuristic schema inference
 * 
 * Technical Requirements:
 * - MySQL 5.6 Antelope/Barracuda page formats
 * - mmap-based high-performance scanning
 * - Zero-copy parsing using pointer casting
 * - Strict bounds checking for memory safety
 */

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <optional>
#include <map>
#include <set>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_parser_v2.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Constants and Type Definitions ====================

/// @brief Maximum file size for scanning (8TB for extreme scenarios)
constexpr uint64_t MAX_SCAN_SIZE = 8ULL * 1024 * 1024 * 1024 * 1024;

/// @brief Minimum page count for a valid .ibd file
constexpr uint32_t MIN_VALID_PAGES = 2;  //至少要有系统页和索引页

/// @brief CRC32 checksum offset in InnoDB page header
constexpr uint32_t FIL_PAGE_SPACE_ID_OR_CHKSUM = 0;  // CRC32 or space ID at offset 0

/// @brief InnoDB LSN (Log Sequence Number) structure
struct LSN {
    uint64_t value;
    
    static LSN from_bytes(const uint8_t* data, size_t offset) {
        LSN lsn;
        lsn.value = read_le<uint64_t>(data, offset);
        return lsn;
    }
    
    bool is_valid() const {
        // Valid LSN should be within reasonable range
        // LSN increases monotonically, modern MySQL can have very large LSNs
        return value > 0 && value < 1000000000000000000ULL;  // < 10^18
    }
};

/// @brief Recovered Record (including "zombie" deleted records)
struct RecoveredRecord {
    uint32_t page_no;           // Page number containing this record
    uint16_t record_offset;     // Offset within page
    uint16_t heap_no;           // Heap number
    std::vector<std::vector<uint8_t>> fields;  // Field data
    bool is_deleted;            // Whether record is marked deleted
    bool is_zombie;             // "Zombie" - deleted but bytes still present
    uint64_t lsn;               // LSN when record was last modified
    uint16_t record_length;     // Total record length
    
    std::string to_hex() const {
        std::string hex;
        for (const auto& field : fields) {
            for (uint8_t b : field) {
                char buf[3];
                snprintf(buf, sizeof(buf), "%02x", b);
                hex += buf;
            }
            hex += "|";
        }
        if (!hex.empty()) hex.pop_back();
        return hex;
    }
};

/// @brief B+Tree Page Metadata (simplified for reconstruction)
struct BPlusTreePage {
    uint32_t page_no;           // This page's number
    uint32_t prev_page;         // Previous leaf page (for bidirectional chain)
    uint32_t next_page;         // Next leaf page (for bidirectional chain)
    uint16_t page_level;        // Level in B+Tree (0 = leaf)
    uint16_t n_records;         // Number of user records
    uint32_t heap_top;          // Heap top position
    uint32_t free_space;        // Available free space
    uint16_t n_dir_slots;       // Number of directory slots
    
    // B+Tree properties
    bool is_leaf;               // Leaf node (page_level == 0)
    bool is_root;               // Root node (page_level == max_level)
    
    BPlusTreePage() : page_no(0), prev_page(0xFFFFFFFF), next_page(0xFFFFFFFF),
                      page_level(0), n_records(0), heap_top(0), free_space(0),
                      n_dir_slots(0), is_leaf(false), is_root(false) {}
};

/// @brief Page Chain for B+Tree traversal
struct PageChain {
    uint32_t first_page;        // First page in chain (leftmost leaf)
    uint32_t last_page;         // Last page in chain (rightmost leaf)
    std::vector<uint32_t> pages; // Pages in order
    
    bool is_valid_chain() const {
        return !pages.empty() && 
               pages.front() == first_page && 
               pages.back() == last_page;
    }
};

/// @brief Heuristic Field Type (inferred from binary data)
/// PhotoRec-style: detect field type based on data patterns and byte layout
enum class HeuristicFieldType : uint8_t {
    UNKNOWN = 0,
    TINY_INT = 1,           // 1 byte (INT1/TINYINT)
    SMALL_INT = 2,          // 2 bytes (INT2/SMALLINT)
    MEDIUM_INT = 3,         // 3 bytes (INT3/MEDIUMINT)
    INT = 4,                // 4 bytes (INT32)
    BIGINT = 8,             // 8 bytes (INT64/BIGINT)
    FLOAT = 4,              // 4 bytes (FLOAT)
    DOUBLE = 8,             // 8 bytes (DOUBLE)
    VARCHAR_VARLEN = 9,     // 1-2 byte length prefix + data
    VARCHAR_FIXLEN = 10,    // Fixed length (CHAR)
    BLOB_PTR = 20,          // 20-byte BLOB pointer
    NULL_VALUE = 0xFF,      // NULL marker
    DATETIME = 11,          // 8 bytes datetime (MySQL 5.6+)
    TIMESTAMP = 12,         // 4 bytes timestamp
    DATE = 13,              // 3 bytes date
    TIME = 14,              // 3 bytes time
};

/// @brief PhotoRec-style signature for InnoDB page identification
/// based on MySQL 5.6/5.7 InnoDB file format
struct InnoDBSignature {
    uint16_t page_type;     // FIL_PAGE_TYPE (offset 24)
    uint32_t page_offset;   // FIL_PAGE_OFFSET (offset 4) 
    uint64_t lsn_low;       // LSN low 4 bytes (offset 16-19)
    uint64_t lsn_high;      // LSN high 4 bytes (offset 20-23)
    
    // PhotoRec-style validation scoring
    int validate_score() const {
        int score = 0;
        
        // Page type validation (PhotoRec: check magic bytes)
        if (page_type == 0x45BC || page_type == 0x45BF || 
            page_type == 0x26F1 || page_type == 0x0006) {
            score += 50;  // Valid index page
        } else if (page_type == 0x0000) {
            score += 30;  // Allocated page
        } else if (page_type == 0x0002 || page_type == 0x26F2) {
            score += 20;  // Undo log
        }
        
        // LSN should be increasing ( PhotoRec: temporal consistency)
        if (lsn_low > 0 || lsn_high > 0) {
            score += 15;
        }
        
        // Page offset should be reasonable (PhotoRec: within file bounds)
        if (page_offset < 100000000) {
            score += 15;
        }
        
        return score;
    }
    
    bool is_valid_index_page() const {
        return page_type == 0x45BC || page_type == 0x45BF || 
               page_type == 0x26F1 || page_type == 0x0006;
    }
};

/// @brief Heuristic Field
struct HeuristicField {
    HeuristicFieldType type;
    std::vector<uint8_t> data;
    bool is_null;
    int32_t numeric_value;    // Cached numeric value
    bool is_valid;
    
    HeuristicField() : type(HeuristicFieldType::UNKNOWN), is_null(false), 
                       is_valid(false), numeric_value(0) {}
    
    std::string to_string() const {
        if (!is_valid || is_null) return "";
        if (type == HeuristicFieldType::TINY_INT || 
            type == HeuristicFieldType::SMALL_INT ||
            type == HeuristicFieldType::MEDIUM_INT ||
            type == HeuristicFieldType::INT ||
            type == HeuristicFieldType::BIGINT) {
            return std::to_string(numeric_value);
        }
        // Try ASCII
        std::string result;
        for (uint8_t b : data) {
            if (b >= 32 && b < 127) {
                result += static_cast<char>(b);
            }
        }
        return result;
    }
};

/// @brief Recovered Record with Heuristic Schema
struct HeuristicRecord {
    uint32_t page_no;
    uint16_t record_offset;
    std::vector<HeuristicField> fields;
    bool is_deleted;
    bool is_system_record;    // INFIMUM/SUPREMUM
    
    HeuristicRecord() : page_no(0), record_offset(0), is_deleted(false), 
                        is_system_record(false) {}
    
    std::string to_csv() const {
        if (is_system_record) return "";
        std::string result;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) result += ",";
            result += fields[i].to_string();
        }
        return result;
    }
};

/// @brief Schema Inference Result
struct HeuristicSchema {
    std::vector<std::pair<std::string, HeuristicFieldType>> columns;
    uint32_t min_columns;
    uint32_t max_columns;
    uint32_t columns_detected;
    bool has_auto_increment;
    std::string guessed_engine;
    uint32_t int_columns;
    uint32_t varchar_columns;
    uint32_t blob_columns;
    
    HeuristicSchema() : min_columns(0), max_columns(0), columns_detected(0), 
                        has_auto_increment(false), int_columns(0), varchar_columns(0), blob_columns(0) {}
};

// ==================== Binary Signature Scanner ====================

/**
 * @brief BinaryScanner: Scan raw disk/file data to find InnoDB pages
 * 
 * Implements:
 * - Binary signature scanning
 * - Page header validation (checksum, page type, LSN)
 * - Zero-copy parsing via mmap
 */
class BinaryScanner {
public:
    explicit BinaryScanner(uint32_t page_size = 16384);
    ~BinaryScanner();

    // Disable copy
    BinaryScanner(const BinaryScanner&) = delete;
    BinaryScanner& operator=(const BinaryScanner&) = delete;

    /**
     * @brief Scan a file using mmap for high-performance access
     * @param file_path Path to .ibd file or raw device
     * @param callback Callback for each discovered page
     * @return Number of pages found
     */
    uint64_t scan_file(const std::string& file_path,
                       std::function<bool(uint32_t page_no, const uint8_t* page_data, size_t size)> callback);

    /**
     * @brief Scan raw memory buffer
     * @param data Buffer pointer
     * @param size Buffer size
     * @param start_offset Starting offset (default 0)
     * @param callback Callback for each discovered page
     * @return Number of pages found
     */
    uint64_t scan_buffer(const uint8_t* data, size_t size, uint64_t start_offset,
                        std::function<bool(uint32_t page_no, const uint8_t* page_data, size_t size)> callback);

    /**
     * @brief Validate page header
     * @param data Page data
     * @param size Page size
     * @return true if page header is valid
     */
    bool validate_page_header(const uint8_t* data, size_t size);

    /**
     * @brief Read page type
     * @param data Page data
     * @return Page type code
     */
    uint16_t get_page_type(const uint8_t* data);

    /**
     * @brief Read page offset (page number)
     * @param data Page data
     * @return Page number
     */
    uint32_t get_page_offset(const uint8_t* data);

    /**
     * @brief Read LSN from page
     * @param data Page data
     * @return LSN value
     */
    uint64_t get_page_lsn(const uint8_t* data);

    /**
     * @brief Get scanner statistics
     */
    struct ScanStats {
        uint64_t pages_scanned;
        uint64_t valid_pages;
        uint64_t index_pages;
        uint64_t sdi_pages;
        uint64_t undo_pages;
        uint64_t checksum_failed;
        uint64_t type_failed;
        uint64_t pages_with_records;
        
        void reset() {
            pages_scanned = 0;
            valid_pages = 0;
            index_pages = 0;
            sdi_pages = 0;
            undo_pages = 0;
            checksum_failed = 0;
            type_failed = 0;
            pages_with_records = 0;
        }
    };
    
    const ScanStats& get_stats() const { return stats_; }

private:
    uint32_t page_size_;
    ScanStats stats_;
    
    // Checksum validation (simplified)
    uint32_t compute_crc32(const uint8_t* data, size_t size);
    bool validate_checksum(const uint8_t* data, size_t size);
    
    // Page type validation
    bool is_valid_page_type(uint16_t type);
};

// ==================== Zombie Tuple Extractor ====================

/**
 * @brief ZombieTupleExtractor: Extract deleted tuples by ignoring delete-mark bit
 * 
 * Key features:
 * - Parse record headers and extract "zombie" tuples (deleted but bytes present)
 * - Ignore REC_INFO_DELETED_FLAG to extract all record data
 * - Handle both COMPACT and REDUNDANT row formats
 */
class ZombieTupleExtractor {
public:
    explicit ZombieTupleExtractor(uint32_t page_size = 16384);
    ~ZombieTupleExtractor();

    // Disable copy
    ZombieTupleExtractor(const ZombieTupleExtractor&) = delete;
    ZombieTupleExtractor& operator=(const ZombieTupleExtractor&) = delete;

    /**
     * @brief Extract all records from a page (including "zombie" deleted records)
     * @param data Page data
     * @param size Page size
     * @param page_no Page number
     * @return Vector of recovered records
     */
    std::vector<RecoveredRecord> extract_all_tuples(const uint8_t* data, size_t size, uint32_t page_no);

    /**
     * @brief Extract single record at given offset
     * @param data Page data
     * @param size Page size
     * @param offset Record offset
     * @param page_no Page number
     * @return Recovered record or nullopt
     */
    std::optional<RecoveredRecord> extract_record(const uint8_t* data, size_t size,
                                                   size_t offset, uint32_t page_no);

    /**
     * @brief Extract record header from page
     * @param data Page data
     * @param size Page size
     * @param offset Record offset
     * @return Parsed record header
     */
    RecordHeader parse_record_header(const uint8_t* data, size_t size, size_t offset);

    /**
     * @brief Extract record data (fields)
     * @param data Page data
     * @param size Page size
     * @param offset Record offset
     * @param header Parsed record header
     * @param n_columns Number of columns (for schema inference)
     * @return Vector of field data
     */
    std::vector<std::vector<uint8_t>> extract_record_data(const uint8_t* data, size_t size,
                                                            size_t offset, const RecordHeader& header,
                                                            uint16_t n_columns = 0);

    /**
     * @brief Get statistics
     */
    struct ExtractStats {
        uint64_t total_records;
        uint64_t zombie_records;    // Deleted but bytes present
        uint64_t valid_records;     // Not deleted
        uint64_t corrupted_records;
        
        void reset() {
            total_records = 0;
            zombie_records = 0;
            valid_records = 0;
            corrupted_records = 0;
        }
    };
    
    const ExtractStats& get_stats() const { return stats_; }

private:
    uint32_t page_size_;
    ExtractStats stats_;
    
    // Row format detection
    bool is_compact_format(const uint8_t* data, size_t size);
    uint16_t get_record_data_size(const uint8_t* data, size_t size, size_t offset);
};

// ==================== B+Tree Chain Reconstructor ====================

/**
 * @brief BPlusTreeReconstructor: Reconstruct B+Tree chain using sibling pointers
 * 
 * Key features:
 * - Rebuild logical sequence using prev_page_no/next_page_no
 * - Handle missing .frm by using page metadata
 * - Support both leaf and interior pages
 */
class BPlusTreeReconstructor {
public:
    explicit BPlusTreeReconstructor(uint32_t page_size = 16384);
    ~BPlusTreeReconstructor();

    // Disable copy
    BPlusTreeReconstructor(const BPlusTreeReconstructor&) = delete;
    BPlusTreeReconstructor& operator=(const BPlusTreeReconstructor&) = delete;

    /**
     * @brief Add a page to the reconstructor
     * @param page_no Page number
     * @param data Page data
     * @param size Page size
     * @return true if page was valid
     */
    bool add_page(uint32_t page_no, const uint8_t* data, size_t size);

    /**
     * @brief Reconstruct B+Tree chain from all added pages
     * @param chain Output: reconstructed chain
     * @return true if chain was successfully reconstructed
     */
    bool reconstruct_chain(PageChain& chain);

    /**
     * @brief Get all pages in traversal order
     * @param pages Output: pages in order
     * @return true if successful
     */
    bool get_pages_in_order(std::vector<uint32_t>& pages);

    /**
     * @brief Get page count
     */
    size_t page_count() const { return pages_.size(); }

    /**
     * @brief Find the root page
     * @return Root page number, or 0 if not found
     */
    uint32_t find_root() const;

    /**
     * @brief Find leaf pages
     * @return Vector of leaf page numbers
     */
    std::vector<uint32_t> find_leaf_pages() const;

private:
    uint32_t page_size_;
    std::map<uint32_t, BPlusTreePage> pages_;
};

// ==================== Heuristic Schema Inference ====================

/**
 * @brief HeuristicSchemaInferer: Infer table schema from binary data patterns
 * 
 * Key features:
 * - Analyze binary layout of records
 * - Detect INT/VARCHAR/BLOB patterns
 * - Estimate column types and counts
 * - Handle variable-length fields
 */
class HeuristicSchemaInferer {
public:
    explicit HeuristicSchemaInferer();
    ~HeuristicSchemaInferer();

    // Disable copy
    HeuristicSchemaInferer(const HeuristicSchemaInferer&) = delete;
    HeuristicSchemaInferer& operator=(const HeuristicSchemaInferer&) = delete;

    /**
     * @brief Analyze a single record for schema inference
     * @param record Parsed record data
     */
    void analyze_record(const std::vector<std::vector<uint8_t>>& record);

    /**
     * @brief Analyze a heuristic record
     * @param record Heuristic record
     */
    void analyze_heuristic_record(const std::vector<HeuristicField>& fields);

    /**
     * @brief Infer schema from all analyzed records
     * @return Inferred schema
     */
    HeuristicSchema infer_schema() const;

    /**
     * @brief Detect field type from binary data
     * @param data Field data
     * @return Inferred field type
     */
    static HeuristicFieldType detect_field_type(const std::vector<uint8_t>& data);

    /**
     * @brief Parse numeric value from binary data
     * @param data Field data
     * @param field_type Inferred type
     * @return Numeric value
     */
    static int64_t parse_numeric_value(const std::vector<uint8_t>& data, HeuristicFieldType field_type);

    /**
     * @brief Get statistics
     */
    struct InferStats {
        uint32_t records_analyzed;
        uint32_t columns_detected;
        uint32_t int_columns;
        uint32_t varchar_columns;
        uint32_t blob_columns;
        
        void reset() {
            records_analyzed = 0;
            columns_detected = 0;
            int_columns = 0;
            varchar_columns = 0;
            blob_columns = 0;
        }
    };
    
    const InferStats& get_stats() const { return stats_; }

private:
    InferStats stats_;
    std::vector<std::vector<std::vector<uint8_t>>> analyzed_records_;
    
    // Column type counters per position
    struct ColumnStats {
        uint32_t int_patterns;
        uint32_t varchar_patterns;
        uint32_t null_count;
        uint32_t blob_patterns;
        
        ColumnStats() : int_patterns(0), varchar_patterns(0), null_count(0), blob_patterns(0) {}
    };
    std::map<size_t, ColumnStats> column_statistics_;
};

// ==================== Record Chain Reconstructor ====================

/**
 * @brief RecordChainReconstructor: Reconstruct record chain within a page
 * 
 * Key features:
 * - Use next_record_offset to traverse records
 * - Handle circular references and corruption
 * - Extract records in logical order
 */
class RecordChainReconstructor {
public:
    explicit RecordChainReconstructor(uint32_t page_size = 16384);
    ~RecordChainReconstructor();

    // Disable copy
    RecordChainReconstructor(const RecordChainReconstructor&) = delete;
    RecordChainReconstructor& operator=(const RecordChainReconstructor&) = delete;

    /**
     * @brief Add a record offset
     * @param offset Record offset
     * @param heap_no Heap number
     * @param is_deleted Whether marked deleted
     */
    void add_record_offset(uint16_t offset, uint16_t heap_no, bool is_deleted);

    /**
     * @brief Reconstruct record chain
     * @param chain Output: chain of record offsets
     * @return true if chain is valid
     */
    bool reconstruct_chain(std::vector<uint16_t>& chain);

    /**
     * @brief Get record count
     */
    size_t record_count() const { return offsets_.size(); }

    /**
     * @brief Clear all records
     */
    void clear() { offsets_.clear(); }

private:
    std::map<uint16_t, std::pair<uint16_t, bool>> offsets_;
};

// ==================== High-Level Recovery Interface ====================

/**
 * @brief BinaryRecovery: High-level binary recovery interface
 * 
 * Combines all recovery components for end-to-end recovery:
 * - BinaryScanner: Find pages
 * - ZombieTupleExtractor: Extract deleted records
 * - BPlusTreeReconstructor: Reconstruct B+Tree
 * - HeuristicSchemaInferer: Infer schema
 */
class BinaryRecovery {
public:
    explicit BinaryRecovery(uint32_t page_size = 16384);
    ~BinaryRecovery();

    // Disable copy
    BinaryRecovery(const BinaryRecovery&) = delete;
    BinaryRecovery& operator=(const BinaryRecovery&) = delete;

    /**
     * @brief Recover records from a file
     * @param file_path Path to .ibd file
     * @param recovered_records Output: recovered records
     * @return true if any records recovered
     */
    bool recover_file(const std::string& file_path,
                      std::vector<RecoveredRecord>& recovered_records);

    /**
     * @brief Recover records with heuristic schema inference
     * @param file_path Path to .ibd file
     * @param recovered_records Output: recovered records
     * @param schema Output: inferred schema
     * @return true if any records recovered
     */
    bool recover_with_schema(const std::string& file_path,
                             std::vector<RecoveredRecord>& recovered_records,
                             HeuristicSchema& schema);

    /**
     * @brief Get recovery statistics
     */
    struct RecoveryStats {
        uint64_t pages_scanned;
        uint64_t valid_pages;
        uint64_t records_extracted;
        uint64_t zombie_records;
        uint64_t checksum_failures;
        uint64_t schema_columns;
        
        double recovery_rate() const {
            return valid_pages > 0 ? 
                static_cast<double>(records_extracted) / valid_pages : 0.0;
        }
        
        void reset() {
            pages_scanned = 0;
            valid_pages = 0;
            records_extracted = 0;
            zombie_records = 0;
            checksum_failures = 0;
            schema_columns = 0;
        }
    };
    
    const RecoveryStats& get_stats() const { return stats_; }

    // Get individual components
    BinaryScanner& get_scanner() { return scanner_; }
    ZombieTupleExtractor& get_extractor() { return extractor_; }
    BPlusTreeReconstructor& get_btree_reconstructor() { return btree_reconstructor_; }
    HeuristicSchemaInferer& get_schema_inferer() { return schema_inferer_; }

private:
    uint32_t page_size_;
    RecoveryStats stats_;
    
    BinaryScanner scanner_;
    ZombieTupleExtractor extractor_;
    BPlusTreeReconstructor btree_reconstructor_;
    HeuristicSchemaInferer schema_inferer_;
    
    // Scan callback for recover_file
    bool on_page_found(uint32_t page_no, const uint8_t* page_data, size_t size,
                       std::vector<RecoveredRecord>& records);
};

// ==================== TRUNCATE and rm -rf Scenario Recovery ====================

/**
 * @brief TruncateRecovery: Specialized recovery for TRUNCATE scenarios
 * 
 * TRUNCATE behavior:
 * - B+Tree is emptied but .ibd file size remains
 * - All records marked as deleted
 * - Page structure remains but heap_top may be reset
 */
class TruncateRecovery {
public:
    explicit TruncateRecovery(uint32_t page_size = 16384);
    ~TruncateRecovery();

    /**
     * @brief Detect if file was TRUNCATED
     * @param file_path Path to .ibd file
     * @return true if file appears to be TRUNCATED
     */
    bool detect_truncate(const std::string& file_path);

    /**
     * @brief Recover from TRUNCATED file
     * @param file_path Path to .ibd file
     * @param recovered_records Output: recovered records
     * @return Number of records recovered
     */
    uint64_t recover_from_truncate(const std::string& file_path,
                                    std::vector<RecoveredRecord>& recovered_records);

private:
    uint32_t page_size_;
    
    // Check if page is likely TRUNCATED
    bool is_truncated_page(const uint8_t* data, size_t size);
    
    // Extract records from "zombie" pages
    uint64_t extract_from_truncated_page(const uint8_t* data, size_t size,
                                         std::vector<RecoveredRecord>& records);
};

/**
 * @brief RmrfRecoveryCustom: Specialized recovery for rm -rf scenarios
 * 
 * rm -rf behavior:
 * - .ibd and .frm files are deleted
 * - Data may still exist on disk/backup
 * - Need to recover from raw data pointers
 */
class RmrfRecoveryCustom {
public:
    explicit RmrfRecoveryCustom(uint32_t page_size = 16384);
    ~RmrfRecoveryCustom();

    /**
     * @brief Scan directory for orphaned .ibd files
     * @param dir_path Directory to scan
     * @return Paths to found .ibd files
     */
    std::vector<std::string> scan_directory(const std::string& dir_path);

    /**
     * @brief Recover from orphaned .ibd files
     * @param file_paths .ibd file paths
     * @param recovered_records Output: recovered records
     * @return Number of records recovered
     */
    uint64_t recover_from_files(const std::vector<std::string>& file_paths,
                                 std::vector<RecoveredRecord>& recovered_records);

    /**
     * @brief Scan raw device for InnoDB data
     * @param device_path Raw device path
     * @param recovered_records Output: recovered records
     * @return Number of records recovered
     */
    uint64_t scan_raw_device(const std::string& device_path,
                              std::vector<RecoveredRecord>& recovered_records);

private:
    uint32_t page_size_;
    
    // Signature for InnoDB page type (FIL_PAGE_INDEX = 0x45BF for big-endian read)
    static constexpr uint16_t INNODB_PAGE_SIGNATURE = 0x45BF;  // FIL_PAGE_INDEX (big-endian read)
};

// ==================== Utility Functions ====================

/**
 * @brief Calculate offset to first record in page
 * 
 * Calculate the offset where user records start in an InnoDB page:
 * - After FIL header (38 bytes)
 * - After Page header fields
 * - After INFIMUM/SUPREMUM system records
 * 
 * @param data Page data
 * @param size Page size
 * @return Offset to first user record
 */
inline size_t calculate_first_record_offset(const uint8_t* data, size_t size) {
    // FIL header is always 38 bytes
    size_t offset = FIL_HEADER_SIZE;
    
    // Add page header size (at least 16 bytes for basic fields)
    offset += 16;
    
    // Add INFIMUM record (13 bytes header + minimal data)
    offset += 13;
    
    // Add next_record pointer from INFIMUM (2 bytes)
    offset += 2;
    
    return offset;
}

/**
 * @brief Extract ASCII string from binary data
 */
inline std::string extract_ascii_string(const uint8_t* data, size_t size, size_t max_len = 256) {
    std::string result;
    result.reserve(std::min(size, max_len));
    for (size_t i = 0; i < size && i < max_len; ++i) {
        uint8_t b = data[i];
        if (b >= 32 && b < 127) {
            result += static_cast<char>(b);
        }
    }
    return result;
}

/**
 * @brief Check if data looks like ASCII text
 */
inline bool looks_like_ascii(const uint8_t* data, size_t size, double threshold = 0.8) {
    if (size == 0) return false;
    size_t ascii_count = 0;
    for (size_t i = 0; i < size; ++i) {
        uint8_t b = data[i];
        if (b == 0 || (b >= 32 && b < 127) || b == '\n' || b == '\r' || b == '\t') {
            ascii_count++;
        }
    }
    return static_cast<double>(ascii_count) / size >= threshold;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BINARY_RECOVERY_HPP
