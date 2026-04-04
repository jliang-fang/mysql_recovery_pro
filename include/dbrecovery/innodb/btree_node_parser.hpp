#ifndef DBRECOVERY_INNODB_BTREE_NODE_PARSER_HPP
#define DBRECOVERY_INNODB_BTREE_NODE_PARSER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <memory>
#include <sstream>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief B+Tree 节点详细信息
 */
struct BTreeNodeInfo {
    uint32_t page_no;
    uint16_t page_type;
    uint32_t prev_page;
    uint32_t next_page;
    uint32_t level;
    uint16_t n_dir_slots;
    uint16_t n_heap;
    uint16_t heap_top;
    uint16_t free_space;
    uint16_t free_offset;
    uint16_t garbage_end;
    uint16_t last_insert;
    uint16_t direction;
    uint16_t n_direct_pages;
    
    // 记录信息
    uint16_t n_records;
    uint16_t infimum_offset;
    uint16_t supremum_offset;
    std::vector<uint16_t> record_offsets;
    
    // 行格式信息
    uint8_t btr_format;
    uint16_t flags;
    
    // 页健康状态
    bool has_infimum;
    bool has_supremum;
    bool is_valid;
    std::string status_message;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "BTreeNodeInfo{page_no=" << page_no
            << ", type=0x" << std::hex << page_type << std::dec
            << ", level=" << level
            << ", prev=" << prev_page
            << ", next=" << next_page
            << ", n_records=" << n_records
            << ", n_heap=" << n_heap
            << ", heap_top=" << heap_top
            << ", free_space=" << free_space
            << ", valid=" << std::boolalpha << is_valid << std::noboolalpha
            << "}";
        return oss.str();
    }
};

/**
 * @brief 记录详细信息
 */
struct RecordInfo {
    uint16_t offset;
    uint16_t next_offset;
    uint16_t heap_no;
    uint8_t info_bits;
    bool is_deleted;
    bool is_min_rec;
    bool is_extern;
    uint16_t n_fields;
    
    // 字段数据
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> fields;
    
    // 原始数据
    std::vector<uint8_t> raw_data;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "RecordInfo{offset=" << offset
            << ", heap_no=" << heap_no
            << ", next=" << next_offset
            << ", deleted=" << std::boolalpha << is_deleted << std::noboolalpha
            << ", min_rec=" << is_min_rec
            << ", extern=" << is_extern
            << ", n_fields=" << n_fields
            << ", raw_size=" << raw_data.size() << "}";
        return oss.str();
    }
};

// ✅ 新增：页类型验证函数（在类之前声明）
inline bool is_valid_page_type(uint16_t type) {
    switch (type) {
        case FIL_PAGE_INDEX:
        case FIL_PAGE_UNDO_LOG:
        case FIL_PAGE_TYPE_ALLOCATED:
        case FIL_PAGE_SDI:
        case FIL_PAGE_TYPE_BLOB:
        case FIL_PAGE_COMPRESSED:
            return true;
        default:
            return false;
    }
}

/**
 * @brief B+Tree 节点解析器
 * @details 深度解析 B+Tree 节点的各个组成部分
 */
class BPlusTreeNodeParser {
public:
    explicit BPlusTreeNodeParser(const std::vector<uint8_t>& page_data, size_t page_size = 16384);

    /**
     * @brief 解析节点
     * @return true 解析成功
     */
    bool parse();

    /**
     * @brief 获取节点信息
     */
    const BTreeNodeInfo& node_info() const { return node_info_; }

    /**
     * @brief 获取记录信息列表
     */
    const std::vector<RecordInfo>& records() const { return records_; }

    /**
     * @brief 获取页头信息
     */
    const PageHeader& page_header() const { return page_header_; }

    /**
     * @brief 解析单个记录
     */
    std::optional<RecordInfo> parse_record(size_t offset) const;

    /**
     * @brief 解析记录的字段数据
     */
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> parse_record_fields(
        const uint8_t* record_data, uint16_t n_fields, size_t record_size) const;

    /**
     * @brief 检查页是否有效
     */
    bool is_valid() const { return node_info_.is_valid; }

    /**
     * @brief 获取错误信息
     */
    const std::string& error_message() const { return error_message_; }

    /**
     * @brief 获取页类型字符串
     */
    static std::string page_type_to_string(uint16_t type);

private:
    std::vector<uint8_t> page_data_;
    size_t page_size_;
    BTreeNodeInfo node_info_;
    PageHeader page_header_;
    std::vector<RecordInfo> records_;
    std::string error_message_;

    // 解析页头
    bool parse_page_header();

    // 解析记录目录
    bool parse_record_directory();

    // 解析用户记录
    bool parse_user_records();

    // 解析 INFIMUM/SUPREMUM 记录
    bool parse_system_records();

    // 解析单个记录头
    std::optional<RecordHeader> parse_record_header(const uint8_t* data) const;

    // 获取下一条记录的偏移
    uint16_t get_next_record_offset(const uint8_t* current, uint16_t current_offset) const;
};

// ==================== 实现 ====================

inline BPlusTreeNodeParser::BPlusTreeNodeParser(const std::vector<uint8_t>& page_data, size_t page_size)
    : page_data_(page_data), page_size_(page_size) {
    // 验证页大小
    if (page_size_ != 4096 && page_size_ != 8192 &&
        page_size_ != 16384 && page_size_ != 32768 && page_size_ != 65536) {
        page_size_ = 16384;
    }
    
    // 初始化节点信息
    node_info_.page_no = 0;
    node_info_.page_type = 0;
    node_info_.is_valid = false;
}

inline bool BPlusTreeNodeParser::parse() {
    // 清理之前的状态
    node_info_ = BTreeNodeInfo{};
    records_.clear();
    error_message_.clear();
    
    // 检查页大小
    if (page_data_.size() < page_size_) {
        error_message_ = "Page data size mismatch";
        return false;
    }
    
    // 解析页头
    if (!parse_page_header()) {
        return false;
    }
    
    // 解析系统记录（INFIMUM/SUPREMUM）
    if (!parse_system_records()) {
        error_message_ = "Failed to parse system records";
        return false;
    }
    
    // 解析记录目录
    if (!parse_record_directory()) {
        error_message_ = "Failed to parse record directory";
        return false;
    }
    
    // 解析用户记录
    if (!parse_user_records()) {
        error_message_ = "Failed to parse user records";
        return false;
    }
    
    node_info_.is_valid = true;
    return true;
}

inline bool BPlusTreeNodeParser::parse_page_header() {
    // 读取页类型
    node_info_.page_type = read_le<uint16_t>(page_data_.data(), 16);

    // 验证页类型
    if (!is_valid_page_type(node_info_.page_type)) {
        error_message_ = "Invalid page type: 0x" + std::to_string(node_info_.page_type);
        return false;
    }

    // 读取页号
    node_info_.page_no = read_le<uint32_t>(page_data_.data(), 20);

    // 读取页链指针
    node_info_.prev_page = read_le<uint32_t>(page_data_.data(), 0);
    node_info_.next_page = read_le<uint32_t>(page_data_.data(), 4);

    // 读取页级别
    node_info_.level = read_le<uint16_t>(page_data_.data(), 38 + 4);
    
    // 解析 Page Header
    auto header_opt = PageHeader::parse(page_data_.data() + FIL_HEADER_SIZE, page_size_ - FIL_HEADER_SIZE);
    if (!header_opt.has_value()) {
        error_message_ = "Failed to parse page header";
        return false;
    }
    
    page_header_ = header_opt.value();
    
    // 复制页头字段到节点信息
    node_info_.n_dir_slots = page_header_.n_dir_slots;
    node_info_.n_heap = page_header_.n_heap;
    node_info_.heap_top = page_header_.heap_top;
    node_info_.free_offset = page_header_.free_offset;
    node_info_.garbage_end = page_header_.garbage_end;
    node_info_.last_insert = page_header_.last_insert;
    node_info_.direction = page_header_.direction;
    node_info_.n_direct_pages = page_header_.n_direct_pages;
    
    // 计算空闲空间
    node_info_.free_space = static_cast<uint16_t>(page_size_ - page_header_.heap_top);

    // 计算记录数量
    node_info_.n_records = static_cast<uint16_t>(page_header_.n_heap - 2);  // 减去 INFIMUM 和 SUPREMUM

    return true;
}

inline bool BPlusTreeNodeParser::parse_system_records() {
    // INFIMUM 记录偏移（固定为 54）
    node_info_.infimum_offset = PAGE_INFIMUM_REC_OFFSET_ABS;
    
    // 检查 INFIMUM 记录
    if (node_info_.infimum_offset + 5 > page_data_.size()) {
        error_message_ = "INFIMUM record out of bounds";
        return false;
    }
    
    auto infimum_header = parse_record_header(page_data_.data() + node_info_.infimum_offset);
    if (!infimum_header.has_value()) {
        error_message_ = "Failed to parse INFIMUM record header";
        return false;
    }
    
    node_info_.has_infimum = true;
    
    // SUPREMUM 记录偏移（heap_top - 5）
    node_info_.supremum_offset = page_header_.heap_top - 5;
    
    // 检查 SUPREMUM 记录
    if (node_info_.supremum_offset + 5 > page_data_.size()) {
        error_message_ = "SUPREMUM record out of bounds";
        return false;
    }
    
    auto supremum_header = parse_record_header(page_data_.data() + node_info_.supremum_offset);
    if (!supremum_header.has_value()) {
        error_message_ = "Failed to parse SUPREMUM record header";
        return false;
    }
    
    node_info_.has_supremum = true;
    
    return true;
}

inline bool BPlusTreeNodeParser::parse_record_directory() {
    // 读取目录槽数量
    uint16_t n_dir_slots = page_header_.n_dir_slots;
    
    // 目录槽从偏移 38 开始
    size_t dir_start = FIL_HEADER_SIZE;
    size_t dir_end = dir_start + n_dir_slots * 2;
    
    if (dir_end > page_data_.size()) {
        error_message_ = "Record directory out of bounds";
        return false;
    }
    
    // 读取目录槽
    for (uint16_t i = 0; i < n_dir_slots; i++) {
        uint16_t slot = read_le<uint16_t>(page_data_.data(), dir_start + i * 2);
        if (slot >= FIL_HEADER_SIZE && slot < page_size_) {
            node_info_.record_offsets.push_back(slot);
        }
    }
    
    return true;
}

inline bool BPlusTreeNodeParser::parse_user_records() {
    // 从 INFIMUM 开始遍历记录
    uint16_t current_offset = node_info_.infimum_offset;
    
    while (true) {
        // 解析当前记录
        auto record_opt = parse_record(current_offset);
        if (!record_opt.has_value()) {
            break;
        }
        
        auto& record = record_opt.value();
        records_.push_back(record);
        
        // 检查是否是 SUPREMUM
        if (record.is_min_rec && record.offset == node_info_.supremum_offset) {
            break;
        }
        
        // 移动到下一条记录
        if (record.next_offset <= record.offset || record.next_offset >= page_size_) {
            break;
        }
        
        current_offset = record.next_offset;
    }
    
    return true;
}

inline std::optional<RecordHeader> BPlusTreeNodeParser::parse_record_header(const uint8_t* data) const {
    if (!data || page_size_ < 5) {
        return std::nullopt;
    }
    
    return RecordHeader::parse(data, 5);
}

inline std::optional<RecordInfo> BPlusTreeNodeParser::parse_record(size_t offset) const {
    if (offset + 5 > page_data_.size()) {
        return std::nullopt;
    }
    
    RecordInfo record{};
    record.offset = static_cast<uint16_t>(offset);
    
    // 解析记录头
    auto header_opt = parse_record_header(page_data_.data() + offset);
    if (!header_opt.has_value()) {
        return std::nullopt;
    }
    
    auto& header = header_opt.value();
    
    record.next_offset = header.get_next_record_offset();
    record.heap_no = header.get_heap_no();
    record.info_bits = header.info_bits;
    record.is_deleted = header.is_deleted();
    record.is_min_rec = header.is_min_rec();
    record.n_fields = 0;  // COMPACT 格式在第5字节
    
    // 读取 n_fields（COMPACT 格式）
    if (offset + 5 < page_data_.size()) {
        record.n_fields = page_data_[offset + 5];
    }
    
    // 读取字段数据
    record.fields = parse_record_fields(page_data_.data() + offset, record.n_fields, 
                                        page_data_.size() - offset);
    
    // 读取原始数据
    size_t record_end = record.next_offset > offset ? record.next_offset : offset + 100;
    if (record_end > page_data_.size()) {
        record_end = page_data_.size();
    }
    
    record.raw_data.assign(page_data_.data() + offset, page_data_.data() + record_end);
    
    return record;
}

inline std::vector<std::pair<uint16_t, std::vector<uint8_t>>> BPlusTreeNodeParser::parse_record_fields(
    const uint8_t* record_data, uint16_t n_fields, size_t record_size) const {
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> fields;
    
    if (n_fields == 0 || record_size < 6) {
        return fields;
    }
    
    // COMPACT 格式：第6字节开始是可变字段长度数组
    size_t pos = 6;
    
    for (uint16_t i = 0; i < n_fields && pos < record_size; i++) {
        uint16_t field_no = i;
        std::vector<uint8_t> field_data;
        
        // 读取字段长度（1或2字节）
        uint16_t field_len = 0;
        if (pos < record_size) {
            uint8_t len_byte = record_data[pos];
            if (len_byte & 0x80) {
                // 2字节长度
                if (pos + 1 < record_size) {
                    field_len = static_cast<uint16_t>(((len_byte & 0x7F) << 8) | record_data[pos + 1]);
                    pos += 2;
                }
            } else {
                // 1字节长度
                field_len = len_byte;
                pos++;
            }
        }
        
        // 读取字段数据
        if (pos + field_len <= record_size) {
            field_data.assign(record_data + pos, record_data + pos + field_len);
            pos += field_len;
        }
        
        fields.emplace_back(field_no, field_data);
    }
    
    return fields;
}

inline uint16_t BPlusTreeNodeParser::get_next_record_offset(const uint8_t* current, uint16_t current_offset) const {
    if (!current || current_offset + 5 > page_size_) {
        return 0xFFFF;
    }
    
    auto header = parse_record_header(current);
    if (!header.has_value()) {
        return 0xFFFF;
    }
    
    return header->get_next_record_offset();
}

inline std::string BPlusTreeNodeParser::page_type_to_string(uint16_t type) {
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

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BTREE_NODE_PARSER_HPP
