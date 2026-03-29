#ifndef DBRECOVERY_INNODB_CONSTANTS_HPP
#define DBRECOVERY_INNODB_CONSTANTS_HPP

#include <cstdint>

namespace dbrecovery {
namespace innodb {

// ==================== DEPRECATED: Constants moved to page_defs.hpp ====================
// This file is kept for backward compatibility only.
// All constants have been moved to page_defs.hpp for better organization.

// ✅ P0-3 修复：移除重复定义，使用 page_defs.hpp 中的定义
// The following constants are now defined in page_defs.hpp:
// - FIL_PAGE_TYPE values (FIL_PAGE_INDEX, FIL_PAGE_UNDO_LOG, etc.)
// - FIL_PAGE_OFFSET, FIL_PAGE_LSN, etc.
// - PAGE_N_DIR_SLOTS, PAGE_HEAP_TOP, etc.
// - REC_STATUS_*, REC_INFO_* flags
// - INNODB_PAGE_SIZE

// For backward compatibility, include page_defs.hpp
// Users should migrate to page_defs.hpp for all new code

// ==================== Remaining constants (not in page_defs.hpp) ====================

// File format identifiers (deprecated, use page_defs.hpp equivalents)
constexpr uint32_t FIL_PAGE_DATA_END = 8;      // offset of data end in FIL header
constexpr uint32_t FIL_PAGE_CHECKSUM = 0;      // offset of checksum field

// Record header constants (deprecated, use page_defs.hpp equivalents)
constexpr size_t REC_N_OLD_EXTRA_BYTES = 5;
constexpr size_t REC_N_NEW_EXTRA_BYTES = 12;

// B-tree node pointer size (in bytes)
constexpr uint32_t NODEPTR_SIZE = 6;

// Maximum index ID length in bytes
constexpr uint32_t MAX_INDEX_ID_LEN = 1024;

// FSP page types (deprecated, use page_defs.hpp equivalents)
constexpr uint16_t FSP_PAGE_TYPE_ALLOCATED = 0;
constexpr uint16_t FSP_PAGE_TYPE_DATA = 1;
constexpr uint16_t FSP_PAGE_TYPE_UNDO_LOG = 2;
constexpr uint16_t FSP_PAGE_TYPE_INDEX = 3;
constexpr uint16_t FSP_PAGE_TYPE_SYS = 4;
constexpr uint16_t FSP_PAGE_TYPE_TRX_SYS = 5;
constexpr uint16_t FSP_PAGE_TYPE_FSEG_HEADER = 6;
constexpr uint16_t FSP_PAGE_TYPE_IBUF_FREE_LIST = 7;
constexpr uint16_t FSP_PAGE_TYPE_IBUF_BITMAP = 8;
constexpr uint16_t FSP_PAGE_TYPE_SYSTEM_HEADER = 9;
constexpr uint16_t FSP_PAGE_TYPE_TEMPORARY = 10;
constexpr uint16_t FSP_PAGE_TYPE_BLOB = 11;

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_CONSTANTS_HPP
