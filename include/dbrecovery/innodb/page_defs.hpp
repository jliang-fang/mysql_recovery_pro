#ifndef DBRECOVERY_INNODB_PAGE_DEFS_HPP
#define DBRECOVERY_INNODB_PAGE_DEFS_HPP

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <optional>
#include <array>
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== InnoDB File Header Constants ====================
// From MySQL 8.0.36: include/fil0fil.h & inlude/page0page.h
// FIL_HEADER_SIZE = 38 bytes

/// @brief Size of the FIL header (fixed at 38 bytes)
constexpr uint16_t FIL_HEADER_SIZE      = 38;

/// @brief Offset of page type within FIL Header (relative to start of FIL header)
constexpr uint16_t FIL_PAGE_TYPE_OFFSET = 12;
/// @brief Absolute offset of page type within a page (FIL_HEADER_SIZE + FIL_PAGE_TYPE_OFFSET = 50)
constexpr uint16_t FIL_PAGE_TYPE_ABS    = FIL_HEADER_SIZE + FIL_PAGE_TYPE_OFFSET; // 50
/// @brief Alias for FIL_PAGE_TYPE_ABS, more explicit
constexpr uint16_t TYPE_OFFSET          = FIL_PAGE_TYPE_ABS;

/// @brief Absolute offset of INFIMUM record within Page Data (after FIL header + BTR header + 8 bytes for system fields)
/// In practice, it's at absolute offset 54, which is FIL_HEADER_SIZE(38) + PAGE_N_DIR_SLOTS(2) + PAGE_HEAP_TOP(2) + ... = 54
constexpr uint16_t PAGE_INFIMUM_REC_OFFSET_ABS = 54;

/// @brief Offset of page number within FIL Header (relative to start of FIL header)
constexpr uint16_t FIL_PAGE_OFFSET      = 4;
/// @brief Absolute offset of LSN within a page (FIL_HEADER_SIZE + 8 = 46)
constexpr uint16_t FIL_PAGE_LSN_ABS     = FIL_HEADER_SIZE + 8; // 46

/// @brief Offset of space ID within FIL Header (relative to start of FIL header)
constexpr uint16_t FIL_PAGE_SPACE_ID    = 0;

// ✅ 新增：页类型值定义（MySQL 8.0 官方定义，参考 include/fil0fil.h）
/// @brief B+Tree index page type
constexpr uint16_t FIL_PAGE_INDEX       = 9969;  // 0x2041 — B+Tree index page
/// @brief Undo log page type
constexpr uint16_t FIL_PAGE_UNDO_LOG    = 9970;  // 0x2042 — Undo log page
/// @brief Freshly allocated page type
constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED = 0;  // Freshly allocated page
/// @brief SDI (Space Descriptor Index) page type
constexpr uint16_t FIL_PAGE_SDI         = 9971;  // 0x2043 — SDI metadata page
/// @brief BLOB page type
constexpr uint16_t FIL_PAGE_TYPE_BLOB   = 9972;  // 0x2044 — BLOB page
/// @brief Compressed page type
constexpr uint16_t FIL_PAGE_COMPRESSED  = 9973;  // 0x2045 — Compressed page

// ✅ 新增：页类型读取辅助函数（使用绝对偏移，更直观）
inline uint16_t get_page_type(const uint8_t* data, size_t offset = 0) {
    return dbrecovery::innodb::read_le<uint16_t>(data, offset + FIL_PAGE_TYPE_ABS);
}

// ✅ 新增：页类型字符串化辅助函数（用于调试输出）
inline std::string page_type_to_string(uint16_t type) {
    switch (type) {
        case FIL_PAGE_INDEX:       return "FIL_PAGE_INDEX";
        case FIL_PAGE_UNDO_LOG:    return "FIL_PAGE_UNDO_LOG";
        case FIL_PAGE_TYPE_ALLOCATED: return "FIL_PAGE_TYPE_ALLOCATED";
        case FIL_PAGE_SDI:         return "FIL_PAGE_SDI";
        case FIL_PAGE_TYPE_BLOB:   return "FIL_PAGE_TYPE_BLOB";
        case FIL_PAGE_COMPRESSED:  return "FIL_PAGE_COMPRESSED";
        default:                   return "UNKNOWN";
    }
}

// InnoDB 常量
constexpr size_t UNIV_PAGE_SIZE         = 16384; // 默认页大小 16KB
constexpr size_t UNIV_PAGE_SIZE_MIN     = 4096;  // 最小页大小 (4KB)
constexpr size_t UNIV_PAGE_SIZE_MAX     = 65536; // 最大页大小 (64KB)

// ==================== B+Tree Header Constants ====================
// From MySQL 8.0.36: include/page0page.h
// BTR_HEADER starts at offset FIL_HEADER_SIZE (after FIL_HEADER)
// ✅ 修复：使用相对偏移（相对于B+树头），而不是绝对偏移
constexpr uint16_t BTR_N_LEVELS         = 4;        // number of levels in the tree (relative to BTR header)
constexpr uint16_t BTR_IND_PAGE_OFFSET  = 6;        // offset of the index id (relative to BTR header)
constexpr uint16_t BTR_MAX_SIZE         = 50;       // maximum size of a record

// ✅ 新增：绝对偏移常量（用于直接读取页数据）
constexpr uint16_t BTR_N_LEVELS_ABS     = FIL_HEADER_SIZE + 4;   // absolute offset for page_level
constexpr uint16_t BTR_IND_PAGE_OFFSET_ABS = FIL_HEADER_SIZE + 6; // absolute offset for index id

// ==================== Page Header Constants (relative to FIL_HEADER_SIZE) ====================
// From MySQL 8.0.36: include/page0page.h
// PAGE_N_DIR_SLOTS, PAGE_HEAP_TOP, etc., are relative to the start of page data (after FIL header)

/// @brief Offset of PAGE_N_DIR_SLOTS within Page Header (relative to FIL_HEADER_SIZE)
constexpr uint16_t PAGE_N_DIR_SLOTS_OFFSET = 0;
/// @brief Offset of PAGE_HEAP_TOP within Page Header (relative to FIL_HEADER_SIZE)
constexpr uint16_t PAGE_HEAP_TOP_OFFSET    = 2;
/// @brief Offset of PAGE_N_HEAP within Page Header (relative to FIL_HEADER_SIZE)
constexpr uint16_t PAGE_N_HEAP_OFFSET      = 4;
/// @brief Offset of PAGE_FREE within Page Header (relative to FIL_HEADER_SIZE)
constexpr uint16_t PAGE_FREE_OFFSET        = 6;

// ==================== Record Constants ====================
/// @brief Offset of INFIMUM record within Page Data (after FIL header + BTR header + 8 bytes for system fields)
/// In practice, it's at absolute offset 54, which is FIL_HEADER_SIZE(38) + PAGE_N_DIR_SLOTS(2) + PAGE_HEAP_TOP(2) + ... = 54
/// For simplicity and clarity, we define the absolute offset directly.
constexpr uint16_t PAGE_INFIMUM_REC_OFFSET_ABS = 54;
/// @brief Offset of SUPREMUM record within Page Data (typically at heap_top - 5)
/// This is dynamic and should be read from page header, but for reference:
constexpr uint16_t PAGE_SUPREMUM_REC_OFFSET_ABS = 0xFFFF; // Placeholder

// ✅ 新增：页目录槽相关常量（用于解析）
constexpr uint16_t PAGE_N_DIR_SLOTS_SIZE   = 2;  // size of n_dir_slots field
constexpr uint16_t PAGE_HEAP_TOP_SIZE      = 2;  // size of heap_top field
constexpr uint16_t PAGE_N_HEAP_SIZE        = 2;  // size of n_heap field

// ==================== Record Header Constants (COMPACT format) ====================
// From MySQL 8.0.36: include/page0page.h
// REC_N_NEW_BYTES = 5 for COMPACT/REDUNDANT, extra_bytes = 5 + n_nullable * 1

constexpr uint8_t REC_INFO_BITS_MASK    = 0x07;   // info bits mask (lowest 3 bits)
constexpr uint8_t REC_INFO_DELETED_FLAG = 0x08;   // deleted flag (bit 3 of info_bits) - MySQL 8.0 中是 bit 3
constexpr uint8_t REC_INFO_MIN_REC_FLAG = 0x10;   // min record flag (bit 4 of info_bits) - ✅ 修复：MySQL 8.0 中是 bit 4 (REC_MIN_MARK_FLAG)
constexpr uint8_t REC_INFO_EXTERN_FLAG  = 0x20;   // external pointer flag (bit 5 of info_bits) - ✅ 修复：BLOB指针标志是 bit 5

constexpr uint8_t REC_STATUS_ORDINARY   = 0x00;   // ordinary record
constexpr uint8_t REC_STATUS_NODE_PTR   = 0x01;   // node pointer record
constexpr uint8_t REC_STATUS_INFIMUM    = 0x02;   // infimum system record (record type bits)
constexpr uint8_t REC_STATUS_SUPREMUM   = 0x03;   // supremum system record (record type bits)

// ✅ 新增：REC_MIN_MARK_FLAG 是 REC_INFO_MIN_REC_FLAG 的别名，用于兼容不同命名习惯
// 在 MySQL 8.0 源码中，该位被命名为 REC_MIN_MARK_FLAG（位于 info_bits 的 bit 4）
constexpr uint8_t REC_MIN_MARK_FLAG     = 0x10;   // min mark flag (same as REC_INFO_MIN_REC_FLAG)

// ✅ 修复：明确记录头标志位的位掩码定义
// REC_INFO_STATUS_MASK 是最低3位（0x07），用于区分 INFIMUM/SUPREMUM/ORDINARY/Node Pointer
constexpr uint8_t REC_INFO_STATUS_MASK  = 0x07;

// ✅ 新增：BLOB 指针标志位常量
constexpr uint8_t REC_INFO_EXTERN_FLAG  = 0x20;   // external pointer flag (bit 5 of info_bits)

// ==================== Page Header Offsets (relative to page start, absolute offsets) ====================
// These are the offsets from the beginning of the page (FIL_HEADER_SIZE=38 is added).
// They match the InnoDB source code definitions in include/page0page.h
constexpr uint16_t FIL_PAGE_OFFSET_N_DIR_SLOTS   = 38 + 0;  // PAGE_N_DIR_SLOTS: number of slots in page directory
constexpr uint16_t FIL_PAGE_OFFSET_HEAP_TOP      = 38 + 2;  // PAGE_HEAP_TOP: heap top offset
constexpr uint16_t FIL_PAGE_OFFSET_N_HEAP        = 38 + 4;  // PAGE_N_HEAP: number of records in heap (15-bit)
constexpr uint16_t FIL_PAGE_OFFSET_FREE          = 38 + 6;  // PAGE_FREE: start of free list
constexpr uint16_t FIL_PAGE_OFFSET_GARBAGE_END   = 38 + 8;  // PAGE_GARBAGE_END: end of garbage
constexpr uint16_t FIL_PAGE_OFFSET_LAST_INSERT   = 38 + 10; // PAGE_LAST_INSERT: last insert offset
constexpr uint16_t FIL_PAGE_OFFSET_DIRECTION     = 38 + 12; // PAGE_DIRECTION: direction of inserts
constexpr uint16_t FIL_PAGE_OFFSET_N_DIRECT_PAGES= 38 + 14; // PAGE_N_DIRECT_PAGES: number of direct pages (non-leaf)

// ==================== Page Header Offsets (relative to B+Tree header, for compatibility with InnoDB source) ====================
// These are the offsets from the start of the B+Tree header (which is at FIL_HEADER_SIZE=38).
// They match the InnoDB source code definitions in include/page0page.h (e.g., BTR_N_LEVELS = 4)
constexpr uint16_t BTR_PAGE_OFFSET_N_DIR_SLOTS   = 0;
constexpr uint16_t BTR_PAGE_OFFSET_HEAP_TOP      = 2;
constexpr uint16_t BTR_PAGE_OFFSET_N_HEAP        = 4;
constexpr uint16_t BTR_PAGE_OFFSET_FREE          = 6;
constexpr uint16_t BTR_PAGE_OFFSET_GARBAGE_END   = 8;
constexpr uint16_t BTR_PAGE_OFFSET_LAST_INSERT   = 10;
constexpr uint16_t BTR_PAGE_OFFSET_DIRECTION     = 12;
constexpr uint16_t BTR_PAGE_OFFSET_N_DIRECT_PAGES= 14;

// ==================== Record Offset Constants (absolute offsets from page start) ====================
/// @brief Absolute offset of INFIMUM record within Page Data (after FIL header + BTR header + 8 bytes for system fields)
/// In practice, it's at absolute offset 54, which is FIL_HEADER_SIZE(38) + PAGE_N_DIR_SLOTS(2) + PAGE_HEAP_TOP(2) + ... = 54
constexpr uint16_t PAGE_INFIMUM_REC_OFFSET_ABS = 54;
/// @brief Absolute offset of SUPREMUM record within Page Data (typically at heap_top - 5)
/// This is dynamic and should be read from page header, but for reference:
/// Note: In practice, this is calculated as `heap_top - 5` where heap_top is read from the page header.
constexpr uint16_t PAGE_SUPREMUM_REC_OFFSET_ABS = 0xFFFF; // Placeholder (dynamic)

// ==================== Alias for backward compatibility and clarity ====================
#define PAGE_N_DIR_SLOTS FIL_PAGE_OFFSET_N_DIR_SLOTS
#define PAGE_HEAP_TOP    FIL_PAGE_OFFSET_HEAP_TOP
#define PAGE_N_HEAP      FIL_PAGE_OFFSET_N_HEAP
#define PAGE_FREE        FIL_PAGE_OFFSET_FREE
#define PAGE_GARBAGE_END FIL_PAGE_OFFSET_GARBAGE_END
#define PAGE_LAST_INSERT FIL_PAGE_OFFSET_LAST_INSERT
#define PAGE_DIRECTION   FIL_PAGE_OFFSET_DIRECTION
#define PAGE_N_DIRECT_PAGES FIL_PAGE_OFFSET_N_DIRECT_PAGES

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

        RecordHeader header{};
        header.info_bits = ptr[0];
        header.next_rec_off_0 = ptr[1];
        header.next_rec_off_1 = ptr[2];
        header.heap_no_0 = ptr[3];
        header.heap_no_1 = ptr[4];

        return header;
    }

    uint16_t get_next_record_offset() const {
        // ✅ 修复：next_record_offset 是 15 位（bit 0-14），高1位是其他标志
        // bit 0-7 来自 next_rec_off_0, bit 8-14 来自 next_rec_off_1 的低7位
        return static_cast<uint16_t>((static_cast<uint16_t>(next_rec_off_1 & 0x7F) << 8) | next_rec_off_0);
    }

    bool is_deleted() const { 
        return (info_bits & REC_INFO_DELETED_FLAG) != 0; 
    }
    
    bool is_min_rec() const { 
        // ✅ 新增：REC_MIN_MARK_FLAG 是 REC_INFO_MIN_REC_FLAG 的别名
        return (info_bits & REC_INFO_MIN_REC_FLAG) != 0; 
    }

    uint16_t get_heap_no() const {
        // heap_no 是 15 位（bit 0-14），高1位是其他标志
        return static_cast<uint16_t>((static_cast<uint16_t>(heap_no_1 & 0x7F) << 8) | heap_no_0);
    }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "RecordHeader{next=" << get_next_record_offset()
            << ", deleted=" << is_deleted()
            << ", min_rec=" << is_min_rec()
            << ", heap_no=" << get_heap_no()
            << ", info_bits=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(info_bits) << std::dec;
        return oss.str();
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

    // ✅ 新增：PAGE_DIRECTION 和 PAGE_N_DIRECT_PAGES 也属于页头的一部分
    uint16_t direction;     // PAGE_DIRECTION
    uint16_t n_direct_pages;// PAGE_N_DIRECT_PAGES

    static std::optional<PageHeader> parse(const uint8_t* ptr, size_t max_size = 38) {
        // ✅ 修复：max_size 是相对于 ptr 的偏移，至少需要 16 字节才能读取到 PAGE_N_DIRECT_PAGES
        if (!ptr || max_size < 16) {
            return std::nullopt;
        }

        PageHeader header{};
        header.n_dir_slots = read_le<uint16_t>(ptr, 0);
        header.heap_top = read_le<uint16_t>(ptr, 2);
        header.n_heap = read_le<uint16_t>(ptr, 4) & 0x7FFF;
        header.free_offset = read_le<uint16_t>(ptr, 6);
        header.garbage_end = read_le<uint16_t>(ptr, 8);
        header.last_insert = read_le<uint16_t>(ptr, 10);
        header.direction = read_le<uint16_t>(ptr, 12);
        header.n_direct_pages = read_le<uint16_t>(ptr, 14);

        return header;
    }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "PageHeader{n_dir_slots=" << n_dir_slots
            << ", heap_top=" << heap_top
            << ", n_heap=" << n_heap
            << ", free_offset=" << free_offset
            << ", garbage_end=" << garbage_end
            << ", last_insert=" << last_insert
            << ", direction=" << direction
            << ", n_direct_pages=" << n_direct_pages << "}";
        return oss.str();
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
    
    static std::optional<Record> parse(const uint8_t* ptr, size_t offset, size_t max_size = 6) {
        if (!ptr || max_size < 5) {
            return std::nullopt;
        }

        auto header_opt = RecordHeader::parse(ptr, max_size);
        if (!header_opt.has_value()) {
            return std::nullopt;
        }

        Record record{};
        record.offset = static_cast<uint16_t>(offset);
        record.next_offset = header_opt->get_next_record_offset();
        record.is_deleted = header_opt->is_deleted();
        record.heap_no = header_opt->get_heap_no();
        record.info_bits = header_opt->info_bits;
        
        // n_fields 需要额外解析（位于第5字节，即偏移 5）
        if (max_size >= 6) {
            record.n_fields = ptr[5];
        } else {
            record.n_fields = 0; // 默认值
        }

        return record;
    }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "Record{offset=" << offset
            << ", next=" << next_offset
            << ", deleted=" << std::boolalpha << is_deleted << std::noboolalpha
            << ", heap_no=" << heap_no
            << ", info_bits=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(info_bits) << std::dec
            << ", n_fields=" << n_fields << "}";
        return oss.str();
    }
};

// ==================== MySQL 类型常量 ====================
namespace mysql_types {

constexpr uint8_t TINY        = 1;
constexpr uint8_t SHORT       = 2;
constexpr uint8_t LONG        = 3;
constexpr uint8_t FLOAT       = 4;
constexpr uint8_t DOUBLE      = 5;
constexpr uint8_t LONGLONG    = 8;
constexpr uint8_t INT24       = 9;
constexpr uint8_t DATE        = 10;
constexpr uint8_t TIME        = 11;
constexpr uint8_t DATETIME    = 12;
constexpr uint8_t YEAR        = 13;
constexpr uint8_t VARCHAR     = 15;
constexpr uint8_t VAR_STRING  = 15; // ✅ 添加：VAR_STRING 是 VARCHAR 的别名
constexpr uint8_t BIT         = 16;
constexpr uint8_t TIMESTAMP2  = 17;
constexpr uint8_t DATETIME2   = 18;
constexpr uint8_t TIME2       = 19;
constexpr uint8_t JSON        = 245;

// BLOB 类型
constexpr uint8_t TINY_BLOB   = 250;
constexpr uint8_t MEDIUM_BLOB = 251;
constexpr uint8_t LONG_BLOB   = 252;
constexpr uint8_t BLOB        = 253;

} // namespace mysql_types

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_DEFS_HPP
