#ifndef DBRECOVERY_INNODB_PAGE_HEADER_HPP
#define DBRECOVERY_INNODB_PAGE_HEADER_HPP

#include <cstdint>
#include <array>

namespace dbrecovery {
namespace innodb {

// InnoDB page header structure (fixed part, 38 bytes for FIL header + 10 bytes for page type-specific header)
// Total page size is typically 16KB (16384 bytes), but header is only the first ~50 bytes.
// This struct represents the logical layout of the InnoDB page header as defined in MySQL/InnoDB source.

constexpr uint32_t FIL_PAGE_OFFSET = 4;      // 4-byte file page offset
constexpr uint32_t FIL_PAGE_LSN = 8;         // 8-byte log sequence number (LSN)
constexpr uint32_t FIL_PAGE_TYPE = 24;       // 2-byte page type
constexpr uint32_t FIL_PAGE_DATA_START = 38; // Start of data area after header

// Page types (FIL_PAGE_TYPE values)
enum class PageType : uint16_t {
    FSP_HDR = 0x45BF,   // File space header page
    IBUF_BITMAP = 0x45BD, // Insert buffer bitmap
    INODE = 0x45BB,     // Index node (inode) page
    ALLOCATED = 0x0000, // Fresh allocated page
    FSEG_DIR = 0x45BA,  // File segment directory
    BLOB_PTR = 0x45B7,  // Blob pointer page
    BLOB = 0x45B6,      // Blob fragment page
    INDEX = 0x45BF & ~0x1, // Index page (B-tree node) — actual value is 0x45BF but type check uses mask
};

// FIL header (20 bytes)
struct FilHeader {
    uint32_t offset;      // FIL_PAGE_OFFSET: 4 bytes
    uint32_t prev_page;   // FIL_PAGE_PREV: 4 bytes (only for index pages)
    uint32_t next_page;   // FIL_PAGE_NEXT: 4 bytes (only for index pages)
    uint64_t lsn;         // FIL_PAGE_LSN: 8 bytes
};
static_assert(sizeof(FilHeader) == 20, "FilHeader must be 20 bytes");

// Page header for INDEX pages (10 bytes after FIL header, i.e., at offset 38)
struct IndexPageHeader {
    uint16_t n_recs;          // Number of heap records (excluding infimum/supremum) — 2 bytes
    uint16_t max_trx_id;      // Max transaction ID modifying this page (offset into TRX_ID field in record) — 2 bytes
    uint16_t level;           // B-tree level (0 = leaf) — 2 bytes
    uint16_t index_id;        // Index ID (only first 4 bytes used, stored as 2-byte offset + padding?) — actually 8 bytes in newer versions
    uint32_t space_id;        // Space ID of the page (redundant but included for verification) — 4 bytes
};
// Note: In MySQL 5.7+, index_id is 8 bytes, and max_trx_id may be replaced by TRX_ID header in clustered indexes.
// For simplicity, we define a minimal compatible layout.
static_assert(sizeof(IndexPageHeader) == 10, "IndexPageHeader must be 10 bytes");

// Full InnoDB page header (FIL header + index page header)
struct PageHeader {
    FilHeader fil_header;
    IndexPageHeader index_page_header;
};
static_assert(sizeof(PageHeader) == 30, "PageHeader must be 30 bytes for minimal layout");

// Utility functions to extract fields from raw byte buffer (little-endian assumed)
inline uint32_t get_fil_page_offset(const std::array<uint8_t, 16384>& page) {
    return *reinterpret_cast<const uint32_t*>(page.data() + FIL_PAGE_OFFSET);
}

inline uint64_t get_fil_page_lsn(const std::array<uint8_t, 16384>& page) {
    return *reinterpret_cast<const uint64_t*>(page.data() + FIL_PAGE_LSN);
}

inline uint16_t get_fil_page_type(const std::array<uint8_t, 16384>& page) {
    return *reinterpret_cast<const uint16_t*>(page.data() + FIL_PAGE_TYPE);
}

inline PageType get_page_type(const std::array<uint8_t, 16384>& page) {
    return static_cast<PageType>(get_fil_page_type(page));
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_HEADER_HPP
