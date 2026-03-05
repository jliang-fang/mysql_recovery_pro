#ifndef DBRECOVERY_INNODB_CONSTANTS_HPP
#define DBRECOVERY_INNODB_CONSTANTS_HPP

#include <cstdint>

namespace dbrecovery {
namespac e innodb {

// InnoDB page types
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

// InnoDB page size (default 16KB)
constexpr uint32_t INNODB_PAGE_SIZE = 16384;

// File format identifiers
constexpr uint32_t FIL_PAGE_DATA_END = 8; // offset of data end in FIL header
constexpr uint32_t FIL_PAGE_OFFSET = 4;   // offset of page number in FIL header
constexpr uint32_t FIL_PAGE_TYPE = 26;    // offset of page type in FIL header
constexpr uint32_t FIL_PAGE_CHECKSUM = 0; // offset of checksum field

// Record header constants
constexpr size_t REC_N_OLD_EXTRA_BYTES = 5;
constexpr size_t REC_N_NEW_EXTRA_BYTES = 12;

// Record status flags
constexpr uint8_t REC_STATUS_ORDINARY = 0;
constexpr uint8_t REC_STATUS_NODE_PTR = 1;
constexpr uint8_t REC_STATUS_INFIMUM = 2;
constexpr uint8_t REC_STATUS_SUPREMUM = 3;

// B-tree node pointer size (in bytes)
constexpr uint32_t NODEPTR_SIZE = 6;

// Maximum index ID length in bytes
constexpr uint32_t MAX_INDEX_ID_LEN = 1024;

// ✅ FIL_PAGE 偏移定义（InnoDB 页头）
constexpr uint32_t FIL_PAGE_PREV = 0;   // Previous page number (4 bytes)
constexpr uint32_t FIL_PAGE_NEXT = 4;   // Next page number (4 bytes)
constexpr uint32_t FIL_PAGE_LSN = 8;    // LSN of end of log (8 bytes)

// ✅ PAGE_HEADER 偏移定义（InnoDB 索引页头）
constexpr uint32_t PAGE_N_DIR_SLOTS = 0;   // Number of directory slots
constexpr uint32_t PAGE_HEAP_TOP = 2;      // Heap top position
constexpr uint32_t PAGE_N_HEAP = 4;        // Number of records (15 bits)
constexpr uint32_t PAGE_FREE = 6;          // Start of free space

// ✅ Record info bits (REC_INFO_*)
constexpr uint8_t REC_INFO_DELETED_FLAG = 0x1;   // Deleted record
constexpr uint8_t REC_INFO_NEW_CHUNK_FLAG = 0x80; // New chunk flag

// ✅ Page types (FIL_PAGE_TYPE values)
constexpr uint16_t FIL_PAGE_INDEX = 17855;    // B+ Tree index page
constexpr uint16_t FIL_PAGE_UNDO_LOG = 23114; // Undo log page

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_CONSTANTS_HPP
