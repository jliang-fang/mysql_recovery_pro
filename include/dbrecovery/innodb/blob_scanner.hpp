/**
 * @file blob_scanner.hpp
 * @brief BLOB/TEXT page scanner for recovering large object data
 */

#ifndef DBRECOVERY_INNODB_BLOB_SCANNER_HPP
#define DBRECOVERY_INNODB_BLOB_SCANNER_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include "dbrecovery/innodb/heuristic_parser.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"  // For OFF_PAGE_FLAG_* constants

namespace dbrecovery {
namespace innodb {

// ==================== BLOB Structures ====================

/**
 * @brief BLOB page type enumeration
 */
enum class BlobPageType : uint16_t {
    BLOB = 0x0003,       // Uncompressed BLOB
    ZBLOB = 0x0005,      // Compressed BLOB (zlib)
    ZBLOB2 = 0x0006,     // Compressed BLOB v2
};

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
    bool is_compressed() const {
        return page_type == static_cast<uint16_t>(BlobPageType::ZBLOB) ||
               page_type == static_cast<uint16_t>(BlobPageType::ZBLOB2);
    }
};

/**
 * @brief BLOB field reference - pointer to external BLOB storage
 *
 * This structure represents the 20-byte BTR_EXTERN field reference
 * that appears at the end of externally stored (off-page) fields.
 *
 * Layout (20 bytes, at end of field data):
 *   bytes 0-3:  space_id   (4 bytes)
 *   bytes 4-7:  page_no    (4 bytes)
 *   bytes 8-11: offset     (4 bytes)
 *   bytes 12-19: length    (4 bytes) + flags (4 bytes)
 */
struct BLOBField {
    uint16_t field_index;     // Index in record fields
    uint32_t space_id;        // Space ID (BTR_EXTERN_SPACE_ID)
    uint32_t page_no;         // First page number (BTR_EXTERN_PAGE_NO)
    uint32_t offset;          // Offset within first page (BTR_EXTERN_OFFSET)
    uint32_t total_len;       // Total BLOB length (BTR_EXTERN_LEN, lower 24 bits)
    bool is_partial;          // Partial external storage
    bool is_owner;            // This field owns the BLOB (BTR_EXTERN_OWNER_FLAG)
    bool is_inherited;        // Inherited from parent (BTR_EXTERN_INHERITED_FLAG)

    /**
     * @brief Parse BLOB field reference from 20-byte pointer
     * @param data Pointer to BTR_EXTERN field ref (20 bytes at end of field)
     * @return Parsed BLOBField structure
     */
    static BLOBField parse(const uint8_t* data);
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
     * @brief Read ZBLOB (compressed) chain with streaming decompression
     * @param file_path Path to .ibd file
     * @param first_page First page number
     * @return Decompressed BLOB data
     *
     * ZBLOB uses zlib streaming decompression across page chains
     */
    std::vector<uint8_t> read_zblob_chain(
        const std::string& file_path,
        uint32_t first_page);

    /**
     * @brief Read FIRST_BLOB with entry chain traversal (MySQL 5.7.44+)
     * @param page_reader Function to read a page by number
     * @param first_page First page number
     * @param mysql_version_id MySQL version ID (e.g., 50744 for 5.7.44)
     * @return Complete BLOB data
     *
     * Entry header at offset 96 (60 bytes), data at offset 696 for first page
     * Entry chain: next_entry_pageno/next_entry_offset from entry[6:12]
     * Data: pageno/datalen/lobversion from entry[-12:]
     */
    std::vector<uint8_t> read_first_blob_chain(
        const std::function<std::vector<uint8_t>(uint32_t)>& page_reader,
        uint32_t first_page,
        uint32_t mysql_version_id = 80000);

    /**
     * @brief Check if size value indicates off-page storage
     * @param vsize Variable size value from record
     * @param row_format Row format (0=REDUNDANT, 1=COMPACT, 2=DYNAMIC, 3=COMPRESSED)
     * @param mysql_version MySQL version ID
     * @return true if field is stored externally
     */
    static bool is_off_page(uint16_t vsize, uint8_t row_format, uint32_t mysql_version);

    /**
     * @brief Read FIRST_BLOB with entry chain traversal
     * @param file_path Path to .ibd file
     * @param first_page First page number
     * @param total_length Expected total length
     * @return Complete BLOB data
     *
     * First page has entry header at offset 96, data at offset 696
     * Entry chain: next_entry_pageno, next_entry_offset from entry[6:12]
     */
    std::vector<uint8_t> read_first_blob_chain(
        const std::string& file_path,
        uint32_t first_page,
        uint32_t total_length);

    /**
     * @brief Parse BLOB page header from page data
     * @param page_data Raw page data (16KB)
     * @return BLOBPage with parsed header fields
     */
    BLOBPage parse_blob_page_header(const std::vector<uint8_t>& page_data);

    /**
     * @brief Check if page is a valid BLOB page
     * @param page_data Raw page data
     * @return true if page is valid BLOB/ZBLOB page
     */
    bool is_valid_blob_page(const std::vector<uint8_t>& page_data);

    /**
     * @brief Extract BLOB field reference from record field
     * @param field_data Field data with BTR_EXTERN pointer at end
     * @param field_length Total field length (including 20-byte pointer)
     * @return BLOBField with parsed external storage info
     */
    BLOBField extract_blob_pointer(const uint8_t* field_data, uint32_t field_length);

    /**
     * @brief Decompress ZBLOB page data using zlib
     * @param compressed_data Compressed data (starts at offset 38)
     * @param decompressor_state Optional: continue streaming decompression
     * @return Decompressed data chunk
     */
    std::vector<uint8_t> decompress_zblob_page(
        const std::vector<uint8_t>& compressed_data,
        void* decompressor_state = nullptr);

    /**
     * @brief Read BLOB with automatic compression detection
     * @param file_path Path to .ibd file
     * @param first_page First page number
     * @param total_length Expected total length (0 for ZBLOB)
     * @param page_type Page type (auto-detected if 0)
     * @return BLOB data (decompressed if needed)
     */
    std::vector<uint8_t> read_blob_auto(
        const std::string& file_path,
        uint32_t first_page,
        uint32_t total_length = 0,
        uint16_t page_type = 0);

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

// ==================== Inline Implementations ====================

inline BLOBField BLOBField::parse(const uint8_t* data) {
    BLOBField field{};
    field.field_index = 0;
    field.space_id = (static_cast<uint32_t>(data[0]) << 24) |
                     (static_cast<uint32_t>(data[1]) << 16) |
                     (static_cast<uint32_t>(data[2]) << 8) |
                     static_cast<uint32_t>(data[3]);
    field.page_no = (static_cast<uint32_t>(data[4]) << 24) |
                    (static_cast<uint32_t>(data[5]) << 16) |
                    (static_cast<uint32_t>(data[6]) << 8) |
                    static_cast<uint32_t>(data[7]);
    field.offset = (static_cast<uint32_t>(data[8]) << 24) |
                   (static_cast<uint32_t>(data[9]) << 16) |
                   (static_cast<uint32_t>(data[10]) << 8) |
                   static_cast<uint32_t>(data[11]);
    // Length is 8 bytes: lower 4 bytes are length, upper 4 bytes are flags
    uint32_t len_low = (static_cast<uint32_t>(data[12]) << 24) |
                       (static_cast<uint32_t>(data[13]) << 16) |
                       (static_cast<uint32_t>(data[14]) << 8) |
                       static_cast<uint32_t>(data[15]);
    uint32_t len_high = (static_cast<uint32_t>(data[16]) << 24) |
                        (static_cast<uint32_t>(data[17]) << 16) |
                        (static_cast<uint32_t>(data[18]) << 8) |
                        static_cast<uint32_t>(data[19]);
    field.total_len = len_low;
    field.is_owner = (len_high & BTR_EXTERN_OWNER_FLAG) != 0;
    field.is_inherited = (len_high & BTR_EXTERN_INHERITED_FLAG) != 0;
    field.is_partial = false;
    return field;
}

inline BLOBPage BlobScanner::parse_blob_page_header(const std::vector<uint8_t>& page_data) {
    BLOBPage page{};
    page.data = page_data;

    if (page_data.size() < FIL_HEADER_SIZE + BTR_BLOB_HDR_SIZE) {
        return page;
    }

    // Parse FIL header
    page.page_no = (static_cast<uint32_t>(page_data[4]) << 24) |
                   (static_cast<uint32_t>(page_data[5]) << 16) |
                   (static_cast<uint32_t>(page_data[6]) << 8) |
                   static_cast<uint32_t>(page_data[7]);

    page.page_type = (static_cast<uint16_t>(page_data[24]) << 8) |
                     static_cast<uint16_t>(page_data[25]);

    // Parse BLOB header at FIL_PAGE_DATA (offset 38)
    const uint8_t* blob_hdr = page_data.data() + FIL_HEADER_SIZE;
    page.total_len = (static_cast<uint32_t>(blob_hdr[BTR_BLOB_HDR_PART_LEN]) << 24) |
                     (static_cast<uint32_t>(blob_hdr[BTR_BLOB_HDR_PART_LEN + 1]) << 16) |
                     (static_cast<uint32_t>(blob_hdr[BTR_BLOB_HDR_PART_LEN + 2]) << 8) |
                     static_cast<uint32_t>(blob_hdr[BTR_BLOB_HDR_PART_LEN + 3]);

    page.next_page = (static_cast<uint32_t>(blob_hdr[BTR_BLOB_HDR_NEXT_PAGE_NO]) << 24) |
                     (static_cast<uint32_t>(blob_hdr[BTR_BLOB_HDR_NEXT_PAGE_NO + 1]) << 16) |
                     (static_cast<uint32_t>(blob_hdr[BTR_BLOB_HDR_NEXT_PAGE_NO + 2]) << 8) |
                     static_cast<uint32_t>(blob_hdr[BTR_BLOB_HDR_NEXT_PAGE_NO + 3]);

    page.flag = 0;
    page.prev_page = 0;

    return page;
}

inline bool BlobScanner::is_valid_blob_page(const std::vector<uint8_t>& page_data) {
    if (page_data.size() < FIL_HEADER_SIZE) {
        return false;
    }

    uint16_t page_type = (static_cast<uint16_t>(page_data[24]) << 8) |
                         static_cast<uint16_t>(page_data[25]);

    // Check for BLOB page types
    return page_type == static_cast<uint16_t>(BlobPageType::BLOB) ||
           page_type == static_cast<uint16_t>(BlobPageType::ZBLOB) ||
           page_type == static_cast<uint16_t>(BlobPageType::ZBLOB2);
}

inline BLOBField BlobScanner::extract_blob_pointer(const uint8_t* field_data, uint32_t field_length) {
    if (field_length < BTR_EXTERN_FIELD_REF_SIZE) {
        return BLOBField{};
    }

    // BTR_EXTERN field ref is at the END of the field data
    const uint8_t* extern_ref = field_data + field_length - BTR_EXTERN_FIELD_REF_SIZE;
    return BLOBField::parse(extern_ref);
}

// ==================== FIRST_BLOB Entry Chain Traversal ====================

inline bool BlobScanner::is_off_page(uint16_t vsize, uint8_t row_format, uint32_t mysql_version) {
    // DYNAMIC and COMPRESSED row formats use 16404 as off-page flag
    if (row_format == row_formats::DYNAMIC || row_format == row_formats::COMPRESSED) {
        return vsize == OFF_PAGE_FLAG_DYNAMIC;
    }
    // COMPACT/REDUNDANT with POST_ANTELOPE=0 (MySQL < 8.0) uses 17172
    if (mysql_version < 80000) {
        return vsize == OFF_PAGE_FLAG_COMPACT;
    }
    return vsize == OFF_PAGE_FLAG_DYNAMIC;
}

inline std::vector<uint8_t> BlobScanner::read_first_blob_chain(
    const std::function<std::vector<uint8_t>(uint32_t)>& page_reader,
    uint32_t first_page,
    uint32_t mysql_version_id) {

    std::vector<uint8_t> result;

    // MySQL 5.7.44+ uses entry chain traversal
    if (mysql_version_id > 50744) {
        uint32_t current_page = first_page;
        std::vector<uint8_t> first_page_data = page_reader(current_page);

        if (first_page_data.size() < BLOB_ENTRY_HEADER_OFFSET + BLOB_ENTRY_HEADER_SIZE) {
            return result;
        }

        // Entry header at offset 96 (60 bytes)
        std::vector<uint8_t> entry(first_page_data.begin() + BLOB_ENTRY_HEADER_OFFSET,
                                   first_page_data.begin() + BLOB_ENTRY_HEADER_OFFSET + BLOB_ENTRY_HEADER_SIZE);

        size_t iteration_count = 0;
        constexpr size_t MAX_ITERATIONS = 10000;  // Safety limit

        while (!entry.empty() && iteration_count < MAX_ITERATIONS) {
            iteration_count++;

            // Check minimum entry size
            if (entry.size() < BLOB_ENTRY_TRAILER_SIZE) {
                break;
            }

            // Parse trailer: pageno, datalen, lobversion at entry[-12:]
            // Format: struct.unpack('>3L', entry[-12:])
            size_t trailer_offset = entry.size() - BLOB_ENTRY_TRAILER_SIZE;
            uint32_t entry_pageno = (static_cast<uint32_t>(entry[trailer_offset]) << 24) |
                                    (static_cast<uint32_t>(entry[trailer_offset + 1]) << 16) |
                                    (static_cast<uint32_t>(entry[trailer_offset + 2]) << 8) |
                                    static_cast<uint32_t>(entry[trailer_offset + 3]);
            uint32_t entry_datalen_raw = (static_cast<uint32_t>(entry[trailer_offset + 4]) << 24) |
                                          (static_cast<uint32_t>(entry[trailer_offset + 5]) << 16) |
                                          (static_cast<uint32_t>(entry[trailer_offset + 6]) << 8) |
                                          static_cast<uint32_t>(entry[trailer_offset + 7]);
            // datalen is upper 16 bits
            uint32_t datalen = entry_datalen_raw >> BLOB_ENTRY_DATALEN_SHIFT;

            if (entry_pageno == 0) {
                break;
            }

            // Read data from this entry's page
            if (entry_pageno == first_page) {
                // First page: data at offset 696
                if (first_page_data.size() >= BLOB_FIRST_PAGE_DATA_OFFSET + datalen) {
                    result.insert(result.end(),
                                  first_page_data.begin() + BLOB_FIRST_PAGE_DATA_OFFSET,
                                  first_page_data.begin() + BLOB_FIRST_PAGE_DATA_OFFSET + datalen);
                }
            } else {
                // Subsequent pages: data at offset 49
                std::vector<uint8_t> page_data = page_reader(entry_pageno);
                if (page_data.size() >= BLOB_SUBSEQUENT_PAGE_DATA_OFFSET + datalen) {
                    result.insert(result.end(),
                                  page_data.begin() + BLOB_SUBSEQUENT_PAGE_DATA_OFFSET,
                                  page_data.begin() + BLOB_SUBSEQUENT_PAGE_DATA_OFFSET + datalen);
                }
            }

            // Get next entry in chain: next_entry_pageno, next_entry_offset from entry[6:12]
            if (entry.size() < BLOB_ENTRY_NEXT_OFFSET_OFFSET + 2) {
                break;
            }

            uint32_t next_entry_pageno = (static_cast<uint32_t>(entry[BLOB_ENTRY_NEXT_PAGE_OFFSET]) << 24) |
                                          (static_cast<uint32_t>(entry[BLOB_ENTRY_NEXT_PAGE_OFFSET + 1]) << 16) |
                                          (static_cast<uint32_t>(entry[BLOB_ENTRY_NEXT_PAGE_OFFSET + 2]) << 8) |
                                          static_cast<uint32_t>(entry[BLOB_ENTRY_NEXT_PAGE_OFFSET + 3]);
            uint16_t next_entry_offset = (static_cast<uint16_t>(entry[BLOB_ENTRY_NEXT_OFFSET_OFFSET]) << 8) |
                                          static_cast<uint16_t>(entry[BLOB_ENTRY_NEXT_OFFSET_OFFSET + 1]);

            // Check for end of chain (FIL_NULL = 0xFFFFFFFF)
            if (next_entry_pageno == 0 || next_entry_pageno == FIL_NULL) {
                break;
            }

            // Read next entry
            std::vector<uint8_t> next_page_data = page_reader(next_entry_pageno);
            if (next_page_data.size() < next_entry_offset + BLOB_ENTRY_HEADER_SIZE) {
                break;
            }

            entry.assign(next_page_data.begin() + next_entry_offset,
                        next_page_data.begin() + next_entry_offset + BLOB_ENTRY_HEADER_SIZE);
        }
    } else {
        // Legacy format for MySQL 5.6 and early 5.7
        // Simple page chain at offset 38-46
        uint32_t current_page = first_page;

        while (current_page != 0 && current_page != FIL_NULL) {
            std::vector<uint8_t> page_data = page_reader(current_page);
            if (page_data.size() < FIL_HEADER_SIZE + 8) {
                break;
            }

            // Legacy format: REAL_SIZE at offset 38-41, PAGENO at offset 42-45
            uint32_t real_size = (static_cast<uint32_t>(page_data[38]) << 24) |
                                  (static_cast<uint32_t>(page_data[39]) << 16) |
                                  (static_cast<uint32_t>(page_data[40]) << 8) |
                                  static_cast<uint32_t>(page_data[41]);
            uint32_t next_page = (static_cast<uint32_t>(page_data[42]) << 24) |
                                  (static_cast<uint32_t>(page_data[43]) << 16) |
                                  (static_cast<uint32_t>(page_data[44]) << 8) |
                                  static_cast<uint32_t>(page_data[45]);

            // Data starts at offset 46
            if (page_data.size() >= 46 + real_size) {
                result.insert(result.end(),
                              page_data.begin() + 46,
                              page_data.begin() + 46 + real_size);
            }

            current_page = next_page;
        }
    }

    return result;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BLOB_SCANNER_HPP
