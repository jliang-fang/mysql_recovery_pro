#ifndef DBRECOVERY_INNODB_PAGE_PARSER_HPP
#define DBRECOVERY_INNODB_PAGE_PARSER_HPP
#include <vector>
#include <cstdint>
#include <cstddef>

namespace dbrecovery {
namespace innodb {

// ✅ MySQL 8.0 记录头信息常量定义
constexpr uint8_t REC_INFO_BITS_MASK = 0x07;      // 记录头最低3位是info bits
constexpr uint8_t REC_INFO_DELETED_FLAG = 0x01;   // 已删除标记
constexpr uint8_t REC_INFO_MIN_REC_FLAG = 0x02;   // 最小记录标记（infimum）
constexpr uint8_t REC_INFO_NEW_CHUNK_FLAG = 0x40; // 新数据块标志

// ✅ COMPACT 记录头格式标志位
constexpr uint8_t REC_STATUS_ORDINARY = 0;
constexpr uint8_t REC_STATUS_NODE_PTR = 1;
constexpr uint8_t REC_INFO_STATUS_MASK = 0x03;

// ✅ 行格式标志位（从 FSP_HEADER.flags 中提取）
constexpr uint32_t FSP_FLAGS_ATOMIC_BLOBS = (1 << 0);  // 原子BLOB
constexpr uint32_t FSP_FLAGS_COMPACT      = (0 << 1);  // COMPACT行格式
constexpr uint32_t FSP_FLAGS_DYNAMIC      = (1 << 1);  // DYNAMIC行格式
constexpr uint32_t FSP_FLAGS_COMPRESSED   = (2 << 1);  // COMPRESSED行格式
constexpr uint32_t FSP_FLAGS_MASK         = 0x3F;      // 标志位掩码

// ✅ B+树页类型常量
constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED = 0;
constexpr uint16_t FIL_PAGE_INDEX        = 9969;  // B+Tree索引页
constexpr uint16_t FIL_PAGE_UNDO_LOG     = 9970;  // Undo日志页
constexpr uint16_t FIL_PAGE_SDI          = 9971;  // SDI元数据页
constexpr uint16_t FIL_PAGE_TYPE_BLOB    = 9972;  // BLOB页

// ✅ 记录结构定义（包含完整信息）
struct Record {
    uint16_t offset;           // 记录在页中的偏移
    uint8_t info_bits;         // 记录头信息位
    bool is_deleted;           // 是否已删除
    uint16_t next_offset;      // 下一条记录的偏移（链表结构）
    uint16_t n_fields;         // 字段数量
    std::vector<uint8_t> null_bits;  // NULL位图
    
    Record() : offset(0), info_bits(0), is_deleted(false), 
               next_offset(0xFFFF), n_fields(0) {}
};

// ✅ 页头信息结构
struct PageHeader {
    uint16_t n_dir_slots;      // 目录槽数量
    uint16_t heap_top;         // 堆顶偏移
    uint16_t n_heap;           // 堆中记录数（低15位）
    uint16_t free_offset;      // 空闲空间起始偏移
    
    PageHeader() : n_dir_slots(0), heap_top(0), n_heap(0), free_offset(0) {}
};

class PageParser {
public:
    explicit PageParser(const uint8_t* buffer, size_t size);

    bool parse_header();
    
    std::vector<Record> parse_records() const;

    // ✅ 新增：获取页头信息
    [[nodiscard]] const PageHeader& header() const { return header_; }
    
    // ✅ 新增：获取原始数据指针
    const uint8_t* data() const;
    size_t size() const;
    
    // ✅ 新增：获取页类型
    [[nodiscard]] uint16_t type() const { return page_type_; }
    
    // ✅ 新增：获取页级别（B+树高度）
    [[nodiscard]] uint16_t level() const { return page_level_; }

private:
    uint16_t page_type_;
    uint16_t flags_;
    uint16_t page_level_;
    PageHeader header_;
    const uint8_t* buffer_;
    size_t size_;
    
    // 页链信息（用于校验）
    uint32_t fil_prev_ = 0xFFFFFFFF;
    uint32_t fil_next_ = 0xFFFFFFFF;
    uint32_t page_offset_ = 0;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_PARSER_HPP
