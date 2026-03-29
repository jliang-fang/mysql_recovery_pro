#ifndef DBRECOVERY_INNODB_PAGE_HEADER_HPP
#define DBRECOVERY_INNODB_PAGE_HEADER_HPP

#include <cstdint>
#include <array>
#include <string>
#include <sstream>
#include <iomanip>
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ✅ P1-2 完善：InnoDB 页头结构定义（参考 MySQL 8.0.36 源码）
// InnoDB page header structure (fixed part, 38 bytes for FIL header + 10 bytes for page type-specific header)
// Total page size is typically 16KB (16384 bytes), but header is only the first ~50 bytes.
// This struct represents the logical layout of the InnoDB page header as defined in MySQL/InnoDB source.

// ✅ 新增：FIL header 常量（绝对偏移）
constexpr uint32_t FIL_PAGE_OFFSET = 4;      // 4-byte file page offset
constexpr uint32_t FIL_PAGE_LSN = 8;         // 8-byte log sequence number (LSN)
constexpr uint32_t FIL_PAGE_TYPE = 16;       // 2-byte page type (absolute offset)
constexpr uint32_t FIL_PAGE_DATA_START = 38; // Start of data area after header

// ✅ 新增：页类型值定义（参考 page_defs.hpp）
constexpr uint16_t FIL_PAGE_INDEX = 9969;   // B+Tree index page
constexpr uint16_t FIL_PAGE_SDI = 9971;     // SDI metadata page
constexpr uint16_t FIL_PAGE_UNDO_LOG = 9970; // Undo log page
constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED = 0; // Fresh allocated page
constexpr uint16_t FIL_PAGE_TYPE_BLOB = 9972; // BLOB page
constexpr uint16_t FIL_PAGE_COMPRESSED = 9973; // Compressed page

// ✅ 新增：页类型字符串化函数
inline std::string page_type_to_string(uint16_t type) {
    switch (type) {
        case FIL_PAGE_INDEX:       return "FIL_PAGE_INDEX";
        case FIL_PAGE_SDI:         return "FIL_PAGE_SDI";
        case FIL_PAGE_UNDO_LOG:    return "FIL_PAGE_UNDO_LOG";
        case FIL_PAGE_TYPE_ALLOCATED: return "FIL_PAGE_TYPE_ALLOCATED";
        case FIL_PAGE_TYPE_BLOB:   return "FIL_PAGE_TYPE_BLOB";
        case FIL_PAGE_COMPRESSED:  return "FIL_PAGE_COMPRESSED";
        default:                   return "UNKNOWN";
    }
}

// Page types enum (for compatibility)
enum class PageType : uint16_t {
    FSP_HDR = 0x45BF,   // File space header page
    IBUF_BITMAP = 0x45BD, // Insert buffer bitmap
    INODE = 0x45BB,     // Index node (inode) page
    ALLOCATED = 0x0000, // Fresh allocated page
    FSEG_DIR = 0x45BA,  // File segment directory
    BLOB_PTR = 0x45B7,  // Blob pointer page
    BLOB = 0x45B6,      // Blob fragment page
    INDEX = 9969,       // Index page (B-tree node)
    SDI = 9971,         // SDI metadata page
    UNDO_LOG = 9970,    // Undo log page
};

// FIL header (20 bytes)
struct FilHeader {
    uint32_t offset;      // FIL_PAGE_OFFSET: 4 bytes
    uint32_t prev_page;   // FIL_PAGE_PREV: 4 bytes (only for index pages)
    uint32_t next_page;   // FIL_PAGE_NEXT: 4 bytes (only for index pages)
    uint64_t lsn;         // FIL_PAGE_LSN: 8 bytes
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "FilHeader{offset=" << offset
            << ", prev=" << prev_page
            << ", next=" << next_page
            << ", lsn=" << lsn << "}";
        return oss.str();
    }
};
static_assert(sizeof(FilHeader) == 20, "FilHeader must be 20 bytes");

// Page header for INDEX pages (10 bytes after FIL header, i.e., at offset 38)
struct IndexPageHeader {
    uint16_t n_recs;          // Number of heap records (excluding infimum/supremum) — 2 bytes
    uint16_t max_trx_id;      // Max transaction ID modifying this page (offset into TRX_ID field in record) — 2 bytes
    uint16_t level;           // B-tree level (0 = leaf) — 2 bytes
    uint16_t index_id_low;    // Index ID low part — 2 bytes (actual index_id is 8 bytes)
    uint32_t space_id;        // Space ID of the page (redundant but included for verification) — 4 bytes
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "IndexPageHeader{n_recs=" << n_recs
            << ", level=" << level
            << ", space_id=" << space_id << "}";
        return oss.str();
    }
};
// Note: In MySQL 5.7+, index_id is 8 bytes, and max_trx_id may be replaced by TRX_ID header in clustered indexes.
// For simplicity, we define a minimal compatible layout.
static_assert(sizeof(IndexPageHeader) == 10, "IndexPageHeader must be 10 bytes");

// Full InnoDB page header (FIL header + index page header)
struct PageHeader {
    FilHeader fil_header;
    IndexPageHeader index_page_header;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "PageHeader{" << fil_header.to_string()
            << ", " << index_page_header.to_string() << "}";
        return oss.str();
    }
};
static_assert(sizeof(PageHeader) == 30, "PageHeader must be 30 bytes for minimal layout");

// ✅ 新增：从原始字节缓冲区提取字段的工具函数（小端序假设）
inline uint32_t get_fil_page_offset(const uint8_t* page, size_t page_size = 16384) {
    if (page_size < 8) return 0;
    return dbrecovery::innodb::read_le<uint32_t>(page, 4);
}

inline uint64_t get_fil_page_lsn(const uint8_t* page, size_t page_size = 16384) {
    if (page_size < 16) return 0;
    return dbrecovery::innodb::read_le<uint64_t>(page, 8);
}

inline uint16_t get_fil_page_type(const uint8_t* page, size_t page_size = 16384) {
    if (page_size < 18) return 0;
    return dbrecovery::innodb::read_le<uint16_t>(page, 16);
}

inline PageType get_page_type_enum(const uint8_t* page, size_t page_size = 16384) {
    return static_cast<PageType>(get_fil_page_type(page, page_size));
}

inline std::string get_page_type_string(const uint8_t* page, size_t page_size = 16384) {
    return page_type_to_string(get_fil_page_type(page, page_size));
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_HEADER_HPP
