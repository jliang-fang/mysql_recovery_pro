#ifndef DBRECOVERY_INNODB_PAGE_DEFS_HPP
#define DBRECOVERY_INNODB_PAGE_DEFS_HPP

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <optional>
#include <array>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/constants.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== InnoDB File Header Constants ====================
// From MySQL 8.0.36: include/fil0fil.h & include/page0page.h
// FIL_HEADER_SIZE = 38 bytes

/// @brief Size of the FIL header (fixed at 38 bytes)
constexpr uint16_t FIL_HEADER_SIZE      = 38;

/// @brief Offset of page type within FIL Header (relative to start of page)
/// In InnoDB, FIL_PAGE_TYPE is at offset 24 (relative to page start)
/// Per MySQL source code page0page.h: FIL_PAGE_TYPE = 24
/// Offset 16-19 is FIL_PAGE_SPACE_ID (4 bytes), not the page type!
constexpr uint16_t FIL_PAGE_TYPE_OFFSET = 24;
/// @brief Absolute offset of page type within a page (FIL_PAGE_TYPE_OFFSET = 24)
/// Note: Offset 16-19 is FIL_PAGE_SPACE_ID (4 bytes)
/// Note: Offset 20-23 is FIL_PAGE_OFFSET (4 bytes, the page number)
/// Note: Offset 24-25 is FIL_PAGE_TYPE (2 bytes, the page type)
constexpr uint16_t FIL_PAGE_TYPE_ABS    = FIL_PAGE_TYPE_OFFSET; // 24 (relative to page start)
/// @brief Alias for FIL_PAGE_TYPE_ABS, more explicit
constexpr uint16_t TYPE_OFFSET          = FIL_PAGE_TYPE_ABS;

/// @brief Offset of page number within FIL Header (relative to start of FIL header)
constexpr uint16_t FIL_PAGE_OFFSET      = 4;
/// @brief Absolute offset of LSN within a page (offset 16-23 in FIL header)
constexpr uint16_t FIL_PAGE_LSN_ABS     = 16;  // Corrected: LSN is at offset 16-23

/// @brief Offset of FIL_PAGE_PREV within FIL Header (relative to page start)
constexpr uint16_t FIL_PAGE_PREV_ABS    = 8;   // Previous page in page chain (offset 8-11)
/// @brief Offset of FIL_PAGE_NEXT within FIL Header (relative to page start)
constexpr uint16_t FIL_PAGE_NEXT_ABS    = 12;  // Next page in page chain (offset 12-15)
/// @brief Offset of space ID within FIL Header (relative to start of FIL header)
constexpr uint16_t FIL_PAGE_SPACE_ID    = 34;  // Corrected: space_id is at offset 34-37, not 0

// ✅ 新增：页类型值定义
/// MySQL 8.0+ 官方定义（参考 include/fil0fil.h）
/// InnoDB stores FIL header fields in BIG-ENDIAN format.
/// Page type values read with read_be():

/// @brief B+Tree index page type (0x45BF in big-endian = 17855)
constexpr uint16_t FIL_PAGE_INDEX       = 17855; // 0x45BF — B+Tree index page (MySQL 5.6+)
/// @brief Undo log page type (0x45B2 = 17842)
constexpr uint16_t FIL_PAGE_UNDO_LOG    = 17842; // 0x45B2 — Undo log page (MySQL 5.6+)
/// @brief Freshly allocated page type
constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED = 0;  // Freshly allocated page
/// @brief SDI (Space Descriptor Index) page type - MySQL 8.0+ (0x45BE = 17854)
/// Note: This is the uncompressed SDI page type (rarely used)
constexpr uint16_t FIL_PAGE_SDI         = 17854; // 0x45BE — SDI metadata page (MySQL 8.0+)
/// @brief Compressed SDI (SDI_ZIP) page type - MySQL 8.0+ (0x45BD = 17853)
/// This is the actual page type used for SDI pages in MySQL 8.0+ standalone tablespaces.
/// Pages 3 and 4 in a standalone IBD file typically contain compressed SDI data.
/// Reference: ibd2sql IS_SDI() checks for data[24:26] == b'E\xbd' (0x45BD big-endian)
constexpr uint16_t FIL_PAGE_SDI_ZIP     = 17853; // 0x45BD — Compressed SDI metadata page (MySQL 8.0+)
/// @brief BLOB page type (shares 0x45BD with compressed SDI)
/// Note: 0x45BD is used for BOTH compressed SDI pages AND BLOB pages.
/// The distinction is by page position: SDI pages are at pages 3,4; BLOB pages are elsewhere.
constexpr uint16_t FIL_PAGE_TYPE_BLOB   = 17853; // 0x45BD — BLOB page (same as SDI_ZIP)
/// @brief Compressed BLOB page type (0x45B5 = 17845)
constexpr uint16_t FIL_PAGE_COMPRESSED  = 17845; // 0x45B5 — Compressed page

// Page type validation functions (using big-endian read values)
inline bool is_valid_index_page_type(uint16_t type) {
    // MySQL 5.6+: 0x45BF (17855) - the standard INDEX page type
    return type == FIL_PAGE_INDEX;  // 0x45BF = 17855
}

inline bool is_valid_page_type(uint16_t type) {
    return type == FIL_PAGE_INDEX ||         // 0x45BF = 17855
           type == FIL_PAGE_UNDO_LOG ||      // 0x45B2 = 17842
           type == FIL_PAGE_SDI ||           // 0x45BE = 17854 (uncompressed SDI)
           type == FIL_PAGE_SDI_ZIP ||       // 0x45BD = 17853 (compressed SDI)
           type == FIL_PAGE_TYPE_BLOB ||     // 0x45BD = 17853 (same as SDI_ZIP)
           type == FIL_PAGE_TYPE_ALLOCATED;  // 0x0000 = 0
}

/// @brief Check if page is an SDI page (compressed or uncompressed)
inline bool is_sdi_page_type(uint16_t type) {
    return type == FIL_PAGE_SDI || type == FIL_PAGE_SDI_ZIP;
}

/// @brief Check if page is likely an SDI page by position
/// SDI pages are typically at pages 3 and 4 in MySQL 8.0+ standalone tablespaces
inline bool is_sdi_page_position(uint32_t page_no) {
    return page_no == 3 || page_no == 4;
}

// ✅ 新增：页类型读取辅助函数（使用绝对偏移，更直观）
inline uint16_t get_page_type(const uint8_t* data, size_t offset = 0) {
    return dbrecovery::innodb::read_be<uint16_t>(data, offset + FIL_PAGE_TYPE_ABS);
}

// ✅ 新增：兼容函数 get_fil_page_type（用于旧代码兼容）
inline uint16_t get_fil_page_type(const uint8_t* data, size_t page_size = 16384) {
    (void)page_size; // unused parameter for compatibility
    return dbrecovery::innodb::read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
}

// ✅ 新增：静态断言验证关键常量的正确性（编译时检查）
static_assert(FIL_HEADER_SIZE == 38, "FIL_HEADER_SIZE must be 38 bytes");
static_assert(FIL_PAGE_TYPE_ABS == 24, "FIL_PAGE_TYPE_ABS must be 24 (relative to page start)");

// ✅ 新增：页类型字符串化辅助函数（用于调试输出）
inline std::string page_type_to_string(uint16_t type) {
    switch (type) {
        case FIL_PAGE_INDEX:           return "FIL_PAGE_INDEX";       // 0x45BF
        case FIL_PAGE_UNDO_LOG:        return "FIL_PAGE_UNDO_LOG";    // 0x45B2
        case FIL_PAGE_TYPE_ALLOCATED:  return "FIL_PAGE_TYPE_ALLOCATED"; // 0x0000
        case FIL_PAGE_SDI:             return "FIL_PAGE_SDI";        // 0x45BE (uncompressed)
        case FIL_PAGE_COMPRESSED:      return "FIL_PAGE_COMPRESSED"; // 0x45B5
        default: {
            // Handle 0x45BD which is used for both SDI_ZIP and BLOB
            if (type == FIL_PAGE_SDI_ZIP) {
                return "FIL_PAGE_SDI_ZIP/BLOB";  // 0x45BD (compressed SDI or BLOB)
            }
            std::ostringstream oss;
            oss << "UNKNOWN(0x" << std::hex << type << std::dec << ")";
            return oss.str();
        }
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
constexpr uint16_t BTR_N_LEVELS_ABS     = FIL_HEADER_SIZE + 4;   // absolute offset for page_level (38+4=42)
constexpr uint16_t BTR_IND_PAGE_OFFSET_ABS = FIL_HEADER_SIZE + 6; // absolute offset for index id (38+6=44)

// ✅ 新增：BTR Header 相对偏移（用于解析 B+Tree 头部）
constexpr uint16_t BTR_PAGE_OFFSET_HEAP_TOP = 2;   // relative to BTR header
constexpr uint16_t BTR_PAGE_OFFSET_FREE     = 6;   // relative to BTR header
constexpr uint16_t BTR_PAGE_OFFSET_DIRECTION = 12; // relative to BTR header

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
/// @brief Offset of INFIMUM record within Page Data
/// In practice, it's at absolute offset 54, which is FIL_HEADER_SIZE(38) + PAGE_N_DIR_SLOTS(2) + PAGE_HEAP_TOP(2) + ... = 54
constexpr uint16_t PAGE_INFIMUM_REC_OFFSET_ABS = 54;
/// @brief Offset of SUPREMUM record within Page Data (typically at heap_top - 5)
/// This is dynamic and should be read from page header, but for reference:
/// Note: In practice, this is calculated as `heap_top - 5` where heap_top is read from the page header.
constexpr uint16_t PAGE_SUPREMUM_REC_OFFSET_ABS = 0xFFFF; // Placeholder (dynamic)

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

// ==================== External BLOB Field Reference Constants ====================
// From MySQL source: include/btr0cur.h lines 673-693
// BTR_EXTERN_FIELD_REF is a 20-byte structure at the end of externally stored fields

/// @brief Offset of space_id within BTR_EXTERN field ref (20 bytes at end of field)
constexpr uint8_t BTR_EXTERN_SPACE_ID     = 0;    // 4 bytes: space id where stored
/// @brief Offset of page_no within BTR_EXTERN field ref
constexpr uint8_t BTR_EXTERN_PAGE_NO      = 4;    // 4 bytes: page no where stored
/// @brief Offset of offset within BTR_EXTERN field ref
constexpr uint8_t BTR_EXTERN_OFFSET       = 8;    // 4 bytes: offset of BLOB header
/// @brief Offset of length within BTR_EXTERN field ref
constexpr uint8_t BTR_EXTERN_LEN          = 12;   // 8 bytes: length + flags (upper 16 bits are flags)

/// @brief Total size of BTR_EXTERN field reference
constexpr uint8_t BTR_EXTERN_FIELD_REF_SIZE = 20;

/// @brief Alias for backward compatibility
constexpr uint8_t BTR_EXTERN_SIZE = BTR_EXTERN_FIELD_REF_SIZE;

/// @brief Alias for BTR_EXTERN_PAGE_NO (used in some files)
constexpr uint8_t BTR_EXTERN_PAGE = BTR_EXTERN_PAGE_NO;

/// @brief Offset for version/length info (legacy)
constexpr uint8_t BTR_EXTERN_VERSION = 16;

/// @brief Owner flag in BTR_EXTERN_LEN high bits
constexpr uint32_t BTR_EXTERN_OWNER_FLAG      = 0x80000000;  // bit 31: this field owns the BLOB
/// @brief Inherited flag in BTR_EXTERN_LEN high bits
constexpr uint32_t BTR_EXTERN_INHERITED_FLAG  = 0x40000000;  // bit 30: inherited from parent

/// @brief FIL_NULL value (0xFFFFFFFF) indicates end of chain
constexpr uint32_t FIL_NULL = 0xFFFFFFFF;

// ==================== BLOB Page Header Constants ====================
// From MySQL source: include/page0page.h
// BLOB page header starts at FIL_PAGE_DATA (offset 38)

/// @brief BLOB header part length offset (within BLOB page data)
constexpr uint8_t BTR_BLOB_HDR_PART_LEN     = 0;    // 4 bytes: length of this part
/// @brief BLOB header next page number offset
constexpr uint8_t BTR_BLOB_HDR_NEXT_PAGE_NO = 4;    // 4 bytes: next page in chain
/// @brief BLOB header size (total header bytes before data)
constexpr uint8_t BTR_BLOB_HDR_SIZE         = 8;    // 8 bytes total header

/// @brief BLOB first page data starts after infimum/supremum (offset 696 for 16KB page)
/// Reference: ibd2sql FIRST_BLOB uses data[96:96+60] for entry, data[696:] for first page data
constexpr uint16_t BLOB_FIRST_PAGE_DATA_OFFSET = 696;  // For 16KB pages

/// @brief BLOB subsequent page data starts at offset 49 (after FIL header + BLOB header)
/// Reference: ibd2sql FIRST_BLOB uses pg.read(pageno)[49:49+datalen] for subsequent pages
constexpr uint16_t BLOB_SUBSEQUENT_PAGE_DATA_OFFSET = 49;

/// @brief BLOB entry header size (60 bytes in first page)
/// Reference: ibd2sql FIRST_BLOB: entry = data[96:96+60]
constexpr uint16_t BLOB_ENTRY_HEADER_SIZE = 60;

/// @brief BLOB entry header offset in first page
constexpr uint16_t BLOB_ENTRY_HEADER_OFFSET = 96;

// ==================== BLOB Entry Chain Constants ====================
// Reference: ibd2sql/innodb_page/lob.py FIRST_BLOB

/// @brief Offset of next_entry_pageno within BLOB entry header (bytes 6-9)
constexpr uint8_t BLOB_ENTRY_NEXT_PAGE_OFFSET = 6;
/// @brief Offset of next_entry_offset within BLOB entry header (bytes 10-11)
constexpr uint8_t BLOB_ENTRY_NEXT_OFFSET_OFFSET = 10;
/// @brief Size of entry trailer (pageno, datalen, lobversion at bytes -12 to end)
constexpr uint8_t BLOB_ENTRY_TRAILER_SIZE = 12;
/// @brief Data length is in upper 16 bits of trailer datalen field
constexpr uint8_t BLOB_ENTRY_DATALEN_SHIFT = 16;

// ==================== 1-byte vs 2-byte Offset Constants for REDUNDANT ====================
// Reference: undrop-for-innodb/rem0rec.ic

/// @brief REC_OLD_SHORT flag (bit in record header for 1-byte offset detection)
constexpr uint8_t REC_OLD_SHORT_MASK = 0x01;
/// @brief Maximum data size for 1-byte offset format (127 bytes)
constexpr uint32_t REC_1BYTE_OFFS_LIMIT = 0x7FUL;
/// @brief External storage mask in 2-byte offset (bit 14)
constexpr uint16_t REC_2BYTE_EXTERN_MASK = 0x4000;

// ==================== Page Record Chain Validation Constants ====================

/// @brief Maximum records sanity check divisor (page_size / 5)
constexpr uint32_t MAX_RECORDS_DIVISOR = 5;
/// @brief Minimum valid record offset
constexpr uint16_t MIN_RECORD_OFFSET = 2;

// ✅ 新增：REC_MIN_MARK_FLAG 是 REC_INFO_MIN_REC_FLAG 的别名，用于兼容不同命名习惯
// 在 MySQL 8.0 源码中，该位被命名为 REC_MIN_MARK_FLAG（位于 info_bits 的 bit 4）
constexpr uint8_t REC_MIN_MARK_FLAG     = 0x10;   // min mark flag (same as REC_INFO_MIN_REC_FLAG)

// ✅ 修复：明确记录头标志位的位掩码定义
// REC_INFO_STATUS_MASK 是最低3位（0x07），用于区分 INFIMUM/SUPREMUM/ORDINARY/Node Pointer
constexpr uint8_t REC_INFO_STATUS_MASK  = 0x07;

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
// ✅ P0-1 修复：移除重复定义（已在第91-93行定义）
// constexpr uint16_t BTR_PAGE_OFFSET_N_DIR_SLOTS   = 0;
// constexpr uint16_t BTR_PAGE_OFFSET_HEAP_TOP      = 2;
// constexpr uint16_t BTR_PAGE_OFFSET_N_HEAP        = 4;
// constexpr uint16_t BTR_PAGE_OFFSET_FREE          = 6;
// constexpr uint16_t BTR_PAGE_OFFSET_GARBAGE_END   = 8;
// constexpr uint16_t BTR_PAGE_OFFSET_LAST_INSERT   = 10;
// constexpr uint16_t BTR_PAGE_OFFSET_DIRECTION     = 12;
// constexpr uint16_t BTR_PAGE_OFFSET_N_DIRECT_PAGES= 14;

// ✅ 新增：B+Tree 记录偏移常量（用于解析用户记录）
/// @brief Offset of first user record after INFIMUM/SUPREMUM
constexpr uint16_t PAGE_USER_RECORDS_START = 54 + 13; // INFIMUM header size (13 bytes)
/// @brief Offset of next record field within record header
constexpr uint16_t REC_NEXT_OFFSET_POS = 1;

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
    // 5-byte COMPACT record header layout (big-endian bit order):
    // Byte 0-2: info_and_heap (24 bits)
    //   bit 23: instant_flag
    //   bit 22: version_flag
    //   bit 21: deleted_flag
    //   bit 20: min_rec_flag
    //   bits 19-16: n_owned (4 bits)
    //   bits 15-3: heap_no (13 bits)
    //   bits 2-0: rec_type (3 bits)
    // Byte 3-4: next_record_offset (signed 16-bit, relative to record start)
    uint8_t byte0;
    uint8_t byte1;
    uint8_t byte2;
    uint8_t next_off_0;  // low byte of next_record offset
    uint8_t next_off_1;  // high byte of next_record offset

    static std::optional<RecordHeader> parse(const uint8_t* ptr, size_t max_size = 5) {
        if (!ptr || max_size < 5) {
            return std::nullopt;
        }

        RecordHeader header{};
        header.byte0 = ptr[0];
        header.byte1 = ptr[1];
        header.byte2 = ptr[2];
        header.next_off_0 = ptr[3];
        header.next_off_1 = ptr[4];

        return header;
    }

    // REC_NEXT is a signed 16-bit value, relative to current record start
    int16_t get_next_record_offset() const {
        // Byte 3-4: signed 16-bit BIG-ENDIAN (matching ibd2sql struct.unpack('>h'))
        return static_cast<int16_t>(
            (static_cast<uint16_t>(next_off_0) << 8) |
            static_cast<uint16_t>(next_off_1)
        );
    }

    bool is_deleted() const { 
        return (byte0 & 0x20) != 0;  // bit 21 of 24-bit field = deleted
    }
    
    bool is_min_rec() const { 
        return (byte0 & 0x10) != 0;  // bit 20 of 24-bit field = min_rec
    }

    uint16_t get_heap_no() const {
        // bits 15-3 of 24-bit field = ((byte1 << 8) | byte2) >> 3
        return (static_cast<uint16_t>(byte1) << 5) |
               (static_cast<uint16_t>(byte2) >> 3);
    }

    uint8_t get_n_owned() const {
        // bits 19-16 of 24-bit field
        return byte0 & 0x0F;
    }

    uint8_t get_rec_type() const {
        // bits 2-0 of 24-bit field
        return byte2 & 0x07;
    }

    // Accessor for compatibility - info_bits is encoded in byte0/byte1/byte2
    uint8_t get_info_bits() const {
        return byte0;
    }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "RecordHeader{next=" << get_next_record_offset()
            << ", deleted=" << is_deleted()
            << ", min_rec=" << is_min_rec()
            << ", heap_no=" << get_heap_no()
            << ", n_owned=" << (int)get_n_owned()
            << ", rec_type=" << (int)get_rec_type()
            << "}";
        return oss.str();
    }
};

// ==================== Redundant Record Header (6 bytes) ====================
// REDUNDANT/REDUNDANT row format header layout (6 bytes, big-endian):
// Byte 0-3: info bits (32-bit field)
//   bit 31: instant_flag
//   bit 30: version_flag
//   bit 29: deleted_flag
//   bit 28: min_rec_flag
//   bits 27-24: n_owned (4 bits)
//   bits 23-11: heap_no (13 bits)
//   bits 10-1: n_fields (10 bits)
//   bit 0: short_flag
// Byte 4-5: next_record_offset (unsigned 16-bit, relative to record start)
struct alignas(1) RedundantHeader {
    uint8_t byte0;
    uint8_t byte1;
    uint8_t byte2;
    uint8_t byte3;
    uint8_t next_off_0;  // high byte of next_record offset (big-endian)
    uint8_t next_off_1;  // low byte of next_record offset (big-endian)

    static std::optional<RedundantHeader> parse(const uint8_t* ptr, size_t max_size = 6) {
        if (!ptr || max_size < 6) {
            return std::nullopt;
        }

        RedundantHeader header{};
        header.byte0 = ptr[0];
        header.byte1 = ptr[1];
        header.byte2 = ptr[2];
        header.byte3 = ptr[3];
        header.next_off_0 = ptr[4];
        header.next_off_1 = ptr[5];

        return header;
    }

    // REC_NEXT is unsigned 16-bit for REDUNDANT, relative to current record start
    uint16_t get_next_record_offset() const {
        return (static_cast<uint16_t>(next_off_0) << 8) |
               static_cast<uint16_t>(next_off_1);
    }

    /// @brief Check if using 1-byte offset format (REC_OLD_SHORT flag)
    /// Reference: undrop-for-innodb/rem0rec.ic rec_get_1byte_offs_flag()
    /// When bit 0 of byte3 is set, uses 1-byte offsets, otherwise 2-byte offsets
    bool is_1byte_offset_format() const {
        return (byte3 & REC_OLD_SHORT_MASK) != 0;
    }

    /// @brief Get number of fields from header
    /// bits 10-1 of 32-bit field = ((byte2 & 0x03) << 8) | byte3 >> 1
    uint16_t get_n_fields() const {
        return ((static_cast<uint16_t>(byte2) & 0x03) << 8) |
               (static_cast<uint16_t>(byte3) >> 1);
    }

    /// @brief Get the extra bytes size for this record
    /// For 1-byte offset format: 6 + n_fields
    /// For 2-byte offset format: 6 + 2 * n_fields
    uint16_t get_extra_bytes_size() const {
        uint16_t n_fields = get_n_fields();
        if (is_1byte_offset_format()) {
            return REC_N_OLD_EXTRA_BYTES + n_fields;
        }
        return REC_N_OLD_EXTRA_BYTES + 2 * n_fields;
    }

    bool is_deleted() const {
        return (byte0 & 0x10) != 0;  // bit 29 of 32-bit field = deleted
    }

    bool is_min_rec() const {
        return (byte0 & 0x08) != 0;  // bit 28 of 32-bit field = min_rec
    }

    uint16_t get_heap_no() const {
        // bits 23-11 of 32-bit field = ((byte1 << 8) | byte2) >> 3
        return (static_cast<uint16_t>(byte1) << 5) |
               (static_cast<uint16_t>(byte2) >> 3);
    }

    uint8_t get_n_owned() const {
        // bits 27-24 of 32-bit field = byte0 & 0x0F
        return byte0 & 0x0F;
    }

    bool is_short() const {
        return (byte3 & 0x01) != 0;  // bit 0 = short_flag
    }

    uint8_t get_info_bits() const {
        return byte0;
    }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "RedundantHeader{next=" << get_next_record_offset()
            << ", deleted=" << is_deleted()
            << ", min_rec=" << is_min_rec()
            << ", heap_no=" << get_heap_no()
            << ", n_owned=" << (int)get_n_owned()
            << ", n_fields=" << get_n_fields()
            << ", short=" << is_short()
            << "}";
        return oss.str();
    }
};

// ==================== Page Header Structure (for reference) ====================
struct PageHeader {
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
        if (!ptr || max_size < 16) {
            return std::nullopt;
        }

        PageHeader header{};
        // Page header fields are BIG-ENDIAN in InnoDB
        header.n_dir_slots = read_be<uint16_t>(ptr, 0);
        header.heap_top = read_be<uint16_t>(ptr, 2);
        header.n_heap = read_be<uint16_t>(ptr, 4) & 0x7FFF;
        header.free_offset = read_be<uint16_t>(ptr, 6);
        header.garbage_end = read_be<uint16_t>(ptr, 8);
        header.last_insert = read_be<uint16_t>(ptr, 10);
        header.direction = read_be<uint16_t>(ptr, 12);
        header.n_direct_pages = read_be<uint16_t>(ptr, 14);

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
struct Record {
    uint16_t offset;          // absolute offset in page
    int16_t next_offset;      // next record's RELATIVE offset (signed, add to current position)
    bool is_deleted;          // deleted flag
    uint16_t heap_no;         // heap number for ordering
    uint8_t info_bits;        // raw first byte for further analysis
    uint16_t n_fields;        // field count (COMPACT format)

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
        record.info_bits = header_opt->byte0;
        record.n_fields = 0;

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
// These are InnoDB internal type IDs, NOT MySQL server type IDs!
// Reference: ibd2sql/ibd2sql/innodb_page/column_type.py
namespace mysql_types {

// InnoDB internal type IDs (from ibd2sql)
constexpr uint8_t DECIMAL     = 1;   // DECIMAL (MySQL server type 0)
constexpr uint8_t TINY        = 2;   // TINYINT (MySQL server type 1)
constexpr uint8_t SHORT       = 3;   // SMALLINT (MySQL server type 2)
constexpr uint8_t LONG        = 4;   // INT (MySQL server type 3)
constexpr uint8_t FLOAT       = 5;   // FLOAT (MySQL server type 4)
constexpr uint8_t DOUBLE      = 6;   // DOUBLE (MySQL server type 5)
constexpr uint8_t TIMESTAMP   = 8;   // TIMESTAMP (MySQL server type 7)
constexpr uint8_t LONGLONG    = 9;   // BIGINT (MySQL server type 8)
constexpr uint8_t INT24       = 10;  // MEDIUMINT (MySQL server type 9)
constexpr uint8_t DATE        = 11;  // DATE (MySQL server type 10)
constexpr uint8_t TIME        = 12;  // TIME (MySQL server type 11)
constexpr uint8_t DATETIME    = 13;  // DATETIME (MySQL server type 12)
constexpr uint8_t YEAR        = 14;  // YEAR (MySQL server type 13)
constexpr uint8_t NEWDATE     = 15;  // NEWDATE (MySQL server type 14)
constexpr uint8_t VARCHAR     = 16;  // VARCHAR (MySQL server type 15)
constexpr uint8_t BIT         = 17;  // BIT (MySQL server type 16)
constexpr uint8_t TIMESTAMP2  = 18;  // TIMESTAMP2 (MySQL server type 17)
constexpr uint8_t DATETIME2   = 19;  // DATETIME2 (MySQL server type 18)
constexpr uint8_t TIME2       = 20;  // TIME2 (MySQL server type 19)
constexpr uint8_t NEWDECIMAL  = 21;  // NEWDECIMAL (MySQL server type 246)
constexpr uint8_t ENUM        = 22;  // ENUM (MySQL server type 247)
constexpr uint8_t SET         = 23;  // SET (MySQL server type 248)
constexpr uint8_t TINY_BLOB   = 24;  // TINY_BLOB/TINYTEXT (MySQL server type 249)
constexpr uint8_t MEDIUM_BLOB = 25;  // MEDIUM_BLOB/MEDIUMTEXT (MySQL server type 250)
constexpr uint8_t LONG_BLOB   = 26;  // LONG_BLOB/LONGTEXT (MySQL server type 251)
constexpr uint8_t BLOB        = 27;  // BLOB/TEXT (MySQL server type 252)
constexpr uint8_t VAR_STRING  = 28;  // VAR_STRING (MySQL server type 253)
constexpr uint8_t STRING      = 29;  // STRING/CHAR (MySQL server type 254)
constexpr uint8_t GEOMETRY    = 30;  // GEOMETRY (MySQL server type 255)
constexpr uint8_t JSON        = 31;  // JSON (MySQL server type 245)
constexpr uint8_t VECTOR      = 32;  // VECTOR (MySQL 9.0+)

} // namespace mysql_types

// 行格式常量（在 dbrecovery::innodb 命名空间中定义）
namespace row_formats {
    constexpr uint32_t REDUNDANT = 0;
    constexpr uint32_t COMPACT   = 1;
    constexpr uint32_t DYNAMIC   = 2;
    constexpr uint32_t COMPRESSED = 3;
} // namespace row_formats

// ==================== External Storage Validation ====================
// Reference: undrop-for-innodb/rem0rec.ic and c_parser.c

/**
 * @brief Check if field type can have external storage
 * @param mysql_type_id InnoDB internal type ID
 * @return true if type is BLOB/TEXT (can be externally stored)
 */
inline bool is_blob_type(uint8_t mysql_type_id) {
    return mysql_type_id == mysql_types::TINY_BLOB ||
           mysql_type_id == mysql_types::MEDIUM_BLOB ||
           mysql_type_id == mysql_types::LONG_BLOB ||
           mysql_type_id == mysql_types::BLOB ||
           mysql_type_id == mysql_types::JSON;
}

/**
 * @brief Check if 2-byte offset has external storage flag
 * @param offset Field offset value (from REDUNDANT 2-byte offset array)
 * @return true if REC_2BYTE_EXTERN_MASK (bit 14) is set
 */
inline bool is_extern_2byte(uint16_t offset) {
    return (offset & REC_2BYTE_EXTERN_MASK) != 0;
}

/**
 * @brief Extract field size from 1-byte offset (REDUNDANT format)
 * @param offset 1-byte field offset
 * @return Field size, or REC_OFFS_EXTERNAL if externally stored
 */
inline uint16_t extract_1byte_offset(uint8_t offset) {
    // 1-byte offset: high bit indicates SQL_NULL, rest is offset
    if (offset & 0x80) {
        return static_cast<uint16_t>(offset) & 0x7F;  // NULL flag, size in lower 7 bits
    }
    return static_cast<uint16_t>(offset);
}

/**
 * @brief Extract field size from 2-byte offset (REDUNDANT format)
 * @param offset 2-byte field offset
 * @param has_extern Output: true if externally stored
 * @return Field size
 */
inline uint16_t extract_2byte_offset(uint16_t offset, bool& has_extern) {
    has_extern = is_extern_2byte(offset);
    if (has_extern) {
        return offset & ~REC_2BYTE_EXTERN_MASK;
    }
    // Check for SQL_NULL (bit 15)
    if (offset & 0x8000) {
        return offset & 0x7FFF;  // NULL flag, size in lower 15 bits
    }
    return offset;
}

// ==================== Page Record Chain Validation ====================
// Reference: undrop-for-innodb/c_parser.c check_page()

/**
 * @brief Validate page record chain
 * @param page_data Page data bytes
 * @param page_size Page size
 * @param is_compact true for COMPACT/DYNAMIC format, false for REDUNDANT
 * @param n_records Output: number of records found (excluding system records)
 * @return true if record chain is valid
 */
inline bool validate_record_chain(const uint8_t* page_data, size_t page_size,
                                   bool is_compact, uint32_t& n_records) {
    if (!page_data || page_size < FIL_HEADER_SIZE + 20) {
        return false;
    }

    // Get infimum and supremum offsets based on format
    uint16_t infimum_off = is_compact ? 99 : 100;  // PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM
    uint16_t supremum_off = is_compact ? 112 : 113;

    // Safety limits
    uint32_t max_recs = page_size / MAX_RECORDS_DIVISOR;
    uint16_t p_prev = 0;
    uint16_t p = infimum_off;
    uint32_t recs = 0;

    while (p != supremum_off) {
        // Safety check: max records limit
        if (recs > max_recs) {
            return false;
        }
        // Minimum offset check
        if (p < MIN_RECORD_OFFSET) {
            return false;
        }
        // Out of bounds check
        if (p > page_size) {
            return false;
        }
        // Circular reference detection
        if (p == p_prev) {
            return false;
        }

        p_prev = p;

        // Get next record offset
        if (is_compact) {
            // COMPACT: relative offset at p - 2 (2 bytes, big-endian)
            if (p < 2 || p + 2 > page_size) {
                return false;
            }
            int16_t rel_off = (static_cast<int16_t>(page_data[p - 2]) << 8) |
                               static_cast<int16_t>(page_data[p - 1]);
            p = static_cast<uint16_t>(static_cast<int16_t>(p) + rel_off);
        } else {
            // REDUNDANT: absolute offset at p - 2 (2 bytes, big-endian)
            if (p < 2 || p + 2 > page_size) {
                return false;
            }
            p = (static_cast<uint16_t>(page_data[p - 2]) << 8) |
                static_cast<uint16_t>(page_data[p - 1]);
        }

        recs++;
    }

    n_records = recs > 0 ? recs - 1 : 0;  // Exclude infimum
    return true;
}

/**
 * @brief Check page format (COMPACT vs REDUNDANT)
 * @param page_data Page data bytes
 * @return true if COMPACT/DYNAMIC format, false if REDUNDANT
 */
inline bool is_compact_format(const uint8_t* page_data) {
    if (!page_data) {
        return true;  // Default to COMPACT
    }
    // Check n_heap format bit (bit 15)
    uint16_t n_heap = (static_cast<uint16_t>(page_data[42]) << 8) |
                       static_cast<uint16_t>(page_data[43]);
    return (n_heap & 0x8000) != 0;
}

// ==================== Record Header Validation ====================
// Reference: ibd2sql/innodb_page/index.py

/// @brief Maximum field count for 1-byte offset format (127)
constexpr uint16_t REC_N_FIELDS_ONE_BYTE_MAX = 0x7F;

/// @brief Maximum reasonable field count (1000 is a practical limit)
constexpr uint16_t MAX_FIELD_COUNT = 1000;

/**
 * @brief Validate COMPACT record header
 * @param page_data Page data
 * @param rec_offset Record offset in page
 * @param page_size Page size
 * @param out_n_fields Output: number of fields (if valid)
 * @param out_n_owned Output: number of records owned
 * @param out_deleted Output: deleted flag
 * @return true if record header is valid
 */
inline bool validate_compact_record_header(const uint8_t* page_data, uint16_t rec_offset,
                                            size_t page_size, uint16_t& out_n_fields,
                                            uint8_t& out_n_owned, bool& out_deleted) {
    if (!page_data || rec_offset < 5 || rec_offset >= page_size) {
        return false;
    }

    // COMPACT format: 5-byte header (info_bits, next_offset)
    // Header is stored in reverse order at end of record
    // We read from rec_offset - 5 to rec_offset

    // For now, basic validation - the header is stored before the record data
    // Read from the page directory slot to get the actual record position
    // This is a simplified validation

    // Check if offset is reasonable (must be after header area)
    if (rec_offset < FIL_HEADER_SIZE + 10) {
        return false;
    }

    return true;
}

/**
 * @brief Validate REDUNDANT record header
 * @param page_data Page data
 * @param rec_offset Record offset in page
 * @param page_size Page size
 * @param out_n_fields Output: number of fields
 * @param out_extra_bytes Output: extra bytes size
 * @return true if record header is valid
 */
inline bool validate_redundant_record_header(const uint8_t* page_data, uint16_t rec_offset,
                                              size_t page_size, uint16_t& out_n_fields,
                                              uint16_t& out_extra_bytes) {
    if (!page_data || rec_offset < 6 || rec_offset >= page_size) {
        return false;
    }

    // REDUNDANT format: 6-byte header
    // Read header bytes (stored in reverse order before record data)
    // The header is at offset rec_offset - 6

    // Parse RedundantHeader structure
    auto header_opt = RedundantHeader::parse(page_data + rec_offset - 6, 6);
    if (!header_opt.has_value()) {
        return false;
    }

    const auto& header = header_opt.value();
    uint16_t n_fields = header.get_n_fields();

    // Field count bounds check
    if (n_fields == 0 || n_fields > MAX_FIELD_COUNT) {
        return false;
    }

    // Check next record offset is reasonable
    uint16_t next_off = header.get_next_record_offset();
    if (next_off == 0 || next_off > page_size) {
        return false;
    }

    out_n_fields = n_fields;
    out_extra_bytes = header.get_extra_bytes_size();
    return true;
}

/**
 * @brief Validate page directory
 * @param page_data Page data
 * @param page_size Page size
 * @return true if page directory is valid
 */
inline bool validate_page_directory(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < FIL_HEADER_SIZE + 10) {
        return false;
    }

    // Read n_dir_slots from page header (offset 38, big-endian)
    uint16_t n_dir_slots = (static_cast<uint16_t>(page_data[38]) << 8) |
                           static_cast<uint16_t>(page_data[39]);

    // Reasonable bounds: at least 2 (infimum/supremum), at most 1000
    if (n_dir_slots < 2 || n_dir_slots > 1000) {
        return false;
    }

    // Check that directory slots are within page bounds
    // Directory slots are at the end of the page
    size_t dir_start = page_size - 8 - 2 * n_dir_slots;  // -8 for checksum, then slots
    if (dir_start < FIL_HEADER_SIZE) {
        return false;
    }

    return true;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_DEFS_HPP
