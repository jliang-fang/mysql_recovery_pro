#ifndef DBRECOVERY_INNODB_BLOB_POINTER_H
#define DBRECOVERY_INNODB_BLOB_POINTER_H

#include <cstdint>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// InnoDB BLOB 指针结构（用于外部存储的大对象指针）
// 参考 MySQL 5.7 / 8.0 的 DICT_MAX_INDEX_COL_LEN + BLOB 处理逻辑
// 注意：实际布局依赖于页格式（Antelope vs Barracuda）和行格式

constexpr uint32_t BLOB_POINTER_SIZE_56 = 20; // Antelope (MySQL 5.6/5.7)
constexpr uint32_t BLOB_POINTER_SIZE_80 = 24; // Barracuda (MySQL 8.0+)

struct BlobPointer {
    // 字段含义参考 InnoDB 的 externally stored column pointer
    // Layout (Barracuda, big-endian order in disk, but we store as native endian for parsing):
    // [0:3]   : space_id (4 bytes) — NEW in MySQL 8.0
    // [4:7]   : page_no of first BLOB page (4 bytes)
    // [8:11]  : offset within that page (4 bytes) — bit31 = overflow flag
    // [12:15] : len_low (4 bytes): lower 32 bits of total length
    // [16:19] : len_high (4 bytes): upper 32 bits of total length (if > 4GB)
    // [20:23] : mtype, ptype, and flags (packed into 4 bytes) — unused in recovery logic usually

    uint32_t space_id;      // ✅ 新增：表空间 ID（Barracuda 必需）
    uint32_t page_no;       // 首个 BLOB 页号（0 表示未分配）
    uint32_t offset;        // 页内偏移（bit31 = overflow flag，清除后为实际偏移）
    uint64_t length;        // 总长度（低 32 + 高 32 合并）

    BlobPointer() : space_id(0), page_no(0), offset(0), length(0) {}

    explicit BlobPointer(const uint8_t* data, bool is_barracuda = true) {
        if (is_barracuda) {
            std::memcpy(&space_id, data + 0, sizeof(space_id));
            std::memcpy(&page_no,  data + 4, sizeof(page_no));
            std::memcpy(&offset,   data + 8, sizeof(offset));
            uint32_t len_low, len_high;
            std::memcpy(&len_low,   data + 12, sizeof(len_low));
            std::memcpy(&len_high,  data + 16, sizeof(len_high));
            length = (static_cast<uint64_t>(len_high) << 32) | len_low;
        } else {
            // Antelope: skip space_id
            std::memcpy(&page_no,  data + 0, sizeof(page_no));
            std::memcpy(&offset,   data + 4, sizeof(offset));
            uint32_t len_low, len_high;
            std::memcpy(&len_low,   data + 8, sizeof(len_low));
            std::memcpy(&len_high,  data + 12, sizeof(len_high));
            length = (static_cast<uint64_t>(len_high) << 32) | len_low;
        }
    }

    void serialize(uint8_t* out, bool is_barracuda = true) const {
        if (is_barracuda) {
            std::memcpy(out + 0, &space_id, sizeof(space_id));
            std::memcpy(out + 4, &page_no,  sizeof(page_no));
            uint32_t offset_for_write = offset;
            std::memcpy(out + 8, &offset_for_write, sizeof(offset_for_write));
            uint32_t len_low = static_cast<uint32_t>(length & 0xFFFFFFFF);
            uint32_t len_high = static_cast<uint32_t>((length >> 32) & 0xFFFFFFFF);
            std::memcpy(out + 12, &len_low,   sizeof(len_low));
            std::memcpy(out + 16, &len_high,  sizeof(len_high));
        } else {
            // Antelope: omit space_id
            std::memcpy(out + 0, &page_no,  sizeof(page_no));
            uint32_t offset_for_write = offset;
            std::memcpy(out + 4, &offset_for_write, sizeof(offset_for_write));
            uint32_t len_low = static_cast<uint32_t>(length & 0xFFFFFFFF);
            uint32_t len_high = static_cast<uint32_t>((length >> 32) & 0xFFFFFFFF);
            std::memcpy(out + 8,  &len_low,   sizeof(len_low));
            std::memcpy(out + 12, &len_high,  sizeof(len_high));
        }
        // [20:23] 留空或设为 0（实际中含类型信息，但 recovery 中通常忽略）
        std::memset(out + (is_barracuda ? 20 : 16), 0, 4);
    }

    bool is_null() const { return page_no == 0 && length == 0; }
    
    // ✅ 新增：检查是否为 overflow BLOB（bit31 of offset set）
    bool is_overflow() const { return (offset & 0x80000000) != 0; }
    
    // ✅ 新增：清除 overflow 标志后的偏移
    uint32_t clear_overflow_flag() const { return offset & 0x7FFFFFFF; }
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BLOB_POINTER_H
