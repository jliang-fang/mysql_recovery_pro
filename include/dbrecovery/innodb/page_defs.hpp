#ifndef DBRECOVERY_INNODB_PAGE_DEFS_HPP
#define DBRECOVERY_INNODB_PAGE_DEFS_HPP

#include <cstdint>
#include <string>

namespace dbrecovery {
namespace innodb {

// ==================== InnoDB File Header Constants ====================
// From MySQL 8.0.36: include/fil0fil.h & inlude/page0page.h

constexpr uint16_t FIL_PAGE_TYPE        = 12;   // offset in FIL_HEADER
constexpr uint16_t TYPE_OFFSET          = 12;   // alias for FIL_PAGE_TYPE
constexpr uint16_t FIL_PAGE_OFFSET      = 4;    // offset of page number (4 bytes at offset 4)
constexpr uint16_t FIL_PAGE_LSN         = 8;    // LSN of last change (8 bytes at offset 8)
constexpr uint16_t FIL_PAGE_SPACE_ID    = 0;    // tablespace ID (4 bytes at offset 0, but not standard)

// Page types (FIL_PAGE_TYPE value)
constexpr uint16_t FIL_PAGE_INDEX       = 9969;  // 0x2041 — B+Tree index page
constexpr uint16_t FIL_PAGE_UNDO_LOG    = 9970;  // 0x2042 — Undo log page
constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED = 0;  // Freshly allocated page

// InnoDB 常量
constexpr size_t UNIV_PAGE_SIZE         = 16384; // 默认页大小 16KB
constexpr size_t UNIV_PAGE_SIZE_MIN     = 4096;  // 最小页大小 (4KB)
constexpr size_t UNIV_PAGE_SIZE_MAX     = 65536; // 最大页大小 (64KB)

// ==================== B+Tree Header Constants ====================
// From MySQL 8.0.36: include/page0page.h
constexpr uint16_t BTR_N_LEVELS         = 38;   // number of levels in the tree
constexpr uint16_t BTR_IND_PAGE_OFFSET  = 42;   // offset of the index id
constexpr uint16_t BTR_MAX_SIZE         = 50;   // maximum size of a record

// ==================== Record Header Constants (COMPACT format) ====================
// From MySQL 8.0.36: include/page0page.h
// REC_N_NEW_BYTES = 5 for COMPACT/REDUNDANT, extra_bytes = 5 + n_nullable * 1

constexpr uint8_t REC_INFO_BITS_MASK    = 0x07;   // info bits mask (lowest 3 bits)
constexpr uint8_t REC_INFO_DELETED_FLAG = 0x08;   // deleted flag (bit 3 of info_bits) - MySQL 8.0 中是 bit 3
constexpr uint8_t REC_INFO_MIN_REC_FLAG = 0x02;   // min record flag (bit 1 of info_bits)
constexpr uint8_t REC_INFO_EXTERN_FLAG  = 0x10;   // external pointer flag (bit 4 of info_bits) - BLOB指针标志

constexpr uint8_t REC_STATUS_ORDINARY   = 0x00;   // ordinary record
constexpr uint8_t REC_STATUS_NODE_PTR   = 0x01;   // node pointer record
constexpr uint8_t REC_STATUS_INFIMUM    = 0x02;   // infimum system record
constexpr uint8_t REC_STATUS_SUPREMUM   = 0x03;   // supremum system record

// ==================== Page Header Offsets (relative to offset 38) ====================
constexpr uint16_t PAGE_N_DIR_SLOTS     = 0;  // number of slots in page directory
constexpr uint16_t PAGE_HEAP_TOP        = 2;  // heap top offset
constexpr uint16_t PAGE_N_HEAP          = 4;  // number of records in heap (15-bit)
constexpr uint16_t PAGE_FREE            = 6;  // start of free list
constexpr uint16_t PAGE_GARBAGE_END     = 8;  // end of garbage
constexpr uint16_t PAGE_LAST_INSERT     = 10; // last insert offset
constexpr uint16_t PAGE_DIRECTION       = 12; // direction of inserts
constexpr uint16_t PAGE_N_DIRECT_PAGES  = 14; // number of direct pages (non-leaf)

// ==================== Record Header Structure (COMPACT format) ====================
// Layout: [1 byte info_bits] + [4 bytes next_record_offset]
// Note: Cannot use C++ bitfields directly due to implementation-defined layout.
// Use explicit parsing functions instead.

struct alignas(1) RecordHeader {
    uint8_t info_bits;
    uint8_t next_rec_off_0;  // bits 0-7 of offset
    uint8_t next_rec_off_1;  // bits 8-15 of offset
    uint8_t heap_no_0;       // bits 0-7 of heap_no
    uint8_t heap_no_1;       // bits 8-13 of heap_no (only 6 bits used)

    static std::optional<RecordHeader> parse(const uint8_t* ptr, size_t max_size = 5) {
        if (!ptr || max_size < 5) {
            return std::nullopt;
        }
        
        RecordHeader header;
        header.info_bits = ptr[0];
        header.next_rec_off_0 = ptr[1];
        header.next_rec_off_1 = ptr[2];
        header.heap_no_0 = ptr[3];
        header.heap_no_1 = ptr[4];
        return header;
    }

    // ✅ 注意：COMPACT 记录头中，next_record 偏移是 little-endian 格式
    // [1] 是低字节 (bits 0-7), [2] 是高字节 (bits 8-15)
    uint16_t get_next_record_offset() const {
        return static_cast<uint16_t>((static_cast<uint16_t>(next_rec_off_1 & 0x3F) << 8) | next_rec_off_0);
    }

    bool is_deleted() const { 
        return (info_bits & REC_INFO_DELETED_FLAG) != 0; 
    }
    
    bool is_min_rec() const { 
        return (info_bits & REC_INFO_MIN_REC_FLAG) != 0; 
    }

    uint16_t get_heap_no() const {
        return static_cast<uint16_t>((((uint16_t)(heap_no_1 & 0x3F)) << 8) | heap_no_0);
    }

    std::string to_string() const {
        return "RecordHeader{next=" + std::to_string(get_next_record_offset()) +
               ", deleted=" + std::to_string(is_deleted()) +
               ", min_rec=" + std::to_string(is_min_rec()) +
               ", heap_no=" + std::to_string(get_heap_no()) + "}";
    }
};

// ==================== Page Header Structure (for reference) ====================
struct alignas(1) PageHeader {
    uint16_t n_dir_slots;   // PAGE_N_DIR_SLOTS
    uint16_t heap_top;      // PAGE_HEAP_TOP
    uint16_t n_heap;        // PAGE_N_HEAP & 0x7FFF
    uint16_t free_offset;   // PAGE_FREE
    uint16_t garbage_end;   // PAGE_GARBAGE_END (optional)
    uint16_t last_insert;   // PAGE_LAST_INSERT (optional)
    
    std::string to_string() const {
        return "PageHeader{n_dir_slots=" + std::to_string(n_dir_slots) +
               ", heap_top=" + std::to_string(heap_top) +
               ", n_heap=" + std::to_string(n_heap) +
               ", free_offset=" + std::to_string(free_offset) +
               ", garbage_end=" + std::to_string(garbage_end) +
               ", last_insert=" + std::to_string(last_insert) + "}";
    }
};

// ==================== Record Info (parsed from record header) ====================
struct alignas(1) Record {
    uint16_t offset;          // absolute offset in page
    uint16_t next_offset;     // next record's absolute offset
    bool is_deleted;          // REC_INFO_DELETED_FLAG
    uint16_t heap_no;         // heap number for ordering
    uint8_t info_bits;        // 原始 info_bits 用于进一步分析
    uint16_t n_fields;        // 字段数量（COMPACT格式）
    
    std::string to_string() const {
        return "Record{offset=" + std::to_string(offset) +
               ", next=" + std::to_string(next_offset) +
               ", deleted=" + std::to_string(is_deleted) +
               ", heap_no=" + std::to_string(heap_no) +
               ", info_bits=0x" + std::hex + std::to_string(info_bits) + std::dec +
               ", n_fields=" + std::to_string(n_fields) + "}";
    }
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_DEFS_HPP
