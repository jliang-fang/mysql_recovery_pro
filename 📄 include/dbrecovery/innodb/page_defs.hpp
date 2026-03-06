// /app/mysql8_recovery/include/dbrecovery/innodb/page_defs.hpp (FIXED)
#ifndef DBRECOVERY_INNODB_PAGE_DEFS_HPP
#define DBRECOVERY_INNODB_PAGE_DEFS_HPP

#include <cstdint>
#include <string>

namespace dbrecovery {
namespace innodb {

// ==================== InnoDB File Header Constants ====================
// From MySQL 8.0.36: include/fil0fil.h & inlude/page0page.h

constexpr uint16_t FIL_PAGE_TYPE        = 0;    // offset in FIL_HEADER (bytes 0-3)
constexpr uint16_t FIL_PAGE_OFFSET      = 4;    // offset of page number (bytes 4-7)
constexpr uint16_t FIL_PAGE_LSN         = 8;    // LSN of last change (bytes 8-15, 8 bytes)
constexpr uint16_t FIL_PAGE_SPACE_ID    = 16;   // tablespace ID (bytes 16-19)
constexpr uint16_t FIL_PAGE_PAGE_SIZE   = 28;   // page size (bytes 28-29), low 14 bits = log2(page_size) - 9
constexpr uint16_t FIL_PAGE_CHECKSUM      = 0;  // legacy checksum at offset 0 (overwritten)
constexpr uint16_t FIL_PAGE_END_LOG_SIZE    = 8; // bytes 8-15 in old format, now replaced by LSN

// Page types (FIL_PAGE_TYPE value)
constexpr uint32_t FIL_PAGE_INDEX       = 9969;  // 0x2041 — B+Tree index page
constexpr uint32_t FIL_PAGE_UNDO_LOG    = 9970;  // 0x2042 — Undo log page
constexpr uint32_t FIL_PAGE_TYPE_ALLOCATED = 0;  // Freshly allocated page
constexpr uint32_t FIL_PAGE_SDI         = 9971;  // SDI page (Space Descriptor Index)
constexpr uint32_t FIL_PAGE_TYPE_BLOB   = 9972;  // BLOB page

// ==================== CRC32 Constants ====================
constexpr uint32_t DBRECOVERY_CRC32_INITIAL = 0xFFFFFFFF;

// ==================== Record Info Flags ====================
// From MySQL 8.0.36: inlude/page0page.h
constexpr uint8_t REC_INFO_BITS         = 0x07;   // info bits mask (lowest 3 bits)
constexpr uint8_t REC_INFO_DELETED_FLAG = 0x01;   // deleted record flag
constexpr uint8_t REC_INFO_NEW_CHUNK_FLAG = 0x08; // new chunk flag in n_heap

// ==================== Page Header Offsets (relative to offset 38) ====================
constexpr uint16_t PAGE_N_DIR_SLOTS     = 0;  // number of slots in page directory
constexpr uint16_t PAGE_HEAP_TOP        = 2;  // heap top offset
constexpr uint16_t PAGE_N_HEAP          = 4;  // number of records in heap (15-bit)
constexpr uint16_t PAGE_FREE            = 6;  // start of free list
constexpr uint16_t PAGE_GARBAGE_END     = 8;  // end of garbage
constexpr uint16_t PAGE_LAST_INSERT     = 10; // last insert offset
constexpr uint16_t PAGE_DIRECTION       = 12; // direction of inserts
constexpr uint16_t PAGE_N_DIRECT_PAGES  = 14; // number of direct pages (non-leaf)

// ==================== Record Structure (COMPACT format header) ====================
struct RecordHeader {
    uint32_t next_record_offset : 15; // offset to next record (relative)
    uint32_t deleted_flag       : 1;  // REC_INFO_DELETED_FLAG
    uint32_t min_rec_flag       : 1;  // minimum record flag (leaf page only)
    uint32_t heap_no            : 13; // heap number in page heap

    std::string to_string() const {
        return "RecordHeader{next=" + std::to_string(next_record_offset) +
               ", deleted=" + std::to_string(deleted_flag) +
               ", heap_no=" + std::to_string(heap_no) + "}";
    }
};

// ==================== Page Header Structure (for reference) ====================
struct PageHeader {
    uint16_t n_dir_slots;   // PAGE_N_DIR_SLOTS
    uint16_t heap_top;      // PAGE_HEAP_TOP
    uint16_t n_heap;        // PAGE_N_HEAP & 0x7FFF
    uint16_t free_offset;   // PAGE_FREE

    std::string to_string() const {
        return "PageHeader{n_dir_slots=" + std::to_string(n_dir_slots) +
               ", heap_top=" + std::to_string(heap_top) +
               ", n_heap=" + std::to_string(n_heap) +
               ", free_offset=" + std::to_string(free_offset) + "}";
    }
};

// ==================== Record Info (parsed from record header) ====================
struct Record {
    uint16_t offset;          // absolute offset in page
    uint16_t next_offset;     // next record's absolute offset
    bool is_deleted;          // REC_INFO_DELETED_FLAG
    uint16_t heap_no;         // heap number for ordering

    std::string to_string() const {
        return "Record{offset=" + std::to_string(offset) +
               ", next=" + std::to_string(next_offset) +
               ", deleted=" + std::to_string(is_deleted) +
               ", heap_no=" + std::to_string(heap_no) + "}";
    }
};

// ==================== Utility Functions ====================
inline uint16_t decode_page_size(const uint8_t* page) noexcept {
    // Page size is stored at offset 28-29 as (log2(page_size) - 9)
    // e.g., 16KB → log2(16384)=14 → 14-9=5
    uint16_t encoded = *reinterpret_cast<const uint16_t*>(page + FIL_PAGE_PAGE_SIZE);
    return static_cast<uint16_t>(1 << (encoded & 0x3FFF));
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_DEFS_HPP
