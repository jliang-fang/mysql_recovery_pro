#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <algorithm>
#include <stdexcept>

namespace dbrecovery {
namespace innodb {

// ==================== BPlusTreeParser 实现 ====================

BPlusTreeParser::BPlusTreeParser(const std::vector<uint8_t>& page_data)
    : page_data_(page_data) {
}

bool BPlusTreeParser::parse() noexcept {
    if (page_data_.size() < 38) { // FIL_HEADER 大小
        return false;
    }
    
    return parse_header();
}

bool BPlusTreeParser::parse_header() noexcept {
    // 解析 FIL Header (38 bytes)
    uint16_t page_type = MachRead::read_le<uint16_t>(page_data_.data(), 0);
    
    // 检查页类型
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX && 
        page_type != dbrecovery::innodb::FIL_PAGE_SDI) {
        return false;
    }
    
    // 解析 B+Tree Header (38 bytes into page)
    page_level_ = MachRead::read_le<uint16_t>(page_data_.data(), 38);
    n_records_ = MachRead::read_le<uint16_t>(page_data_.data(), 42);
    next_page_ = MachRead::read_le<uint32_t>(page_data_.data(), 46);
    prev_page_ = MachRead::read_le<uint32_t>(page_data_.data(), 50);
    
    // 确定节点类型
    if (page_level_ == 0) {
        node_type_ = BTreeNodeType::LEAF;
    } else if (prev_page_ == 0xFFFFFFFF && next_page_ == 0xFFFFFFFF) {
        node_type_ = BTreeNodeType::ROOT;
    } else {
        node_type_ = BTreeNodeType::INTERNAL;
    }
    
    return parse_infimum_supremum();
}

bool BPlusTreeParser::parse_infimum_supremum() noexcept {
    // Infimum 和 Supremum 是系统记录，固定偏移
    // Infimum: 49 bytes after page start (after FIL_HEADER + BTR_HEADER)
    // Supremum: follows infimum
    
    uint16_t infimum_offset = MachRead::read_le<uint16_t>(page_data_.data(), 54);
    uint16_t supremum_offset = MachRead::read_le<uint16_t>(page_data_.data(), 56);
    
    record_offsets_.push_back(infimum_offset);
    record_offsets_.push_back(supremum_offset);
    
    return parse_user_records();
}

bool BPlusTreeParser::parse_user_records() noexcept {
    // 解析用户记录偏移数组
    uint16_t dir_start = 60; // DIRECTORY START offset
    
    if (dir_start + 2 > page_data_.size()) {
        return false;
    }
    
    uint16_t dir_size = MachRead::read_le<uint16_t>(page_data_.data(), dir_start);
    
    for (uint16_t i = 0; i < dir_size; ++i) {
        uint16_t slot_offset = dir_start + 2 + i * 2;
        if (slot_offset + 2 > page_data_.size()) {
            break;
        }
        
        uint16_t record_offset = MachRead::read_le<uint16_t>(page_data_.data(), slot_offset);
        if (record_offset > 0 && record_offset < 8192) { // 合理范围检查
            record_offsets_.push_back(record_offset);
        }
    }
    
    return true;
}

std::vector<std::map<std::string, std::string>> 
BPlusTreeParser::parse_records(const catalog::TableSchema& schema) const {
    std::vector<std::map<std::string, std::string>> result;
    
    // 跳过系统记录 (infimum, supremum)
    for (size_t i = 2; i < record_offsets_.size(); ++i) {
        uint16_t offset = record_offsets_[i];
        
        if (offset + 5 > page_data_.size()) { // 至少需要记录头
            continue;
        }
        
        std::map<std::string, std::string> record;
        
        // ✅ Step 1: 解析记录头（COMPACT 格式）
        uint8_t header_size = 5; // COMPACT 记录头固定大小
        
        if (offset + header_size > page_data_.size()) {
            continue;
        }
        
        uint8_t flags = page_data_[offset];
        bool is_deleted = (flags & REC_INFO_DELETED_FLAG) != 0;
        bool is_extern = (flags & 0x40) != 0; // 外部存储标志（BLOB）
        uint16_t n_fields = ((flags & 0x07) << 8) | (page_data_[offset + 1] >> 4);
        
        if (is_deleted) {
            continue; // 跳过已删除记录
        }
        
        // ✅ Step 2: 解析 NULL 位图（仅对可空列）
        uint32_t nullable_columns = 0;
        for (const auto& pair : schema.columns) {
            if (pair.second.is_nullable) {
                nullable_columns++;
            }
        }
        
        uint32_t null_bytes = (nullable_columns + 7) / 8;
        const uint8_t* null_ptr = page_data_.data() + offset + header_size;
        
        // ✅ Step 3: 解析字段数据
        size_t data_pos = offset + header_size + null_bytes;
        
        for (const auto& pair : schema.columns) {
            const auto& col = pair.second;
            
            if (data_pos >= page_data_.size()) {
                break;
            }
            
            std::string field_value;
            
            // ✅ Step 3.1: 检查 NULL（正确计算列索引）
            bool is_null = false;
            uint32_t col_idx = 0;
            for (uint32_t j = 1; j < pair.first; ++j) {
                if (schema.columns.count(j) && schema.columns.at(j).is_nullable) {
                    col_idx++;
                }
            }
            
            if (col.is_nullable && null_bytes > 0 && col_idx < nullable_columns) {
                uint32_t byte_idx = col_idx / 8;
                uint32_t bit_idx = col_idx % 8;
                // ✅ 修正：MySQL 使用小端序存储位图（低位在前）
                is_null = (null_ptr[byte_idx] & (1 << bit_idx)) != 0;
            }
            
            if (is_null) {
                field_value = "NULL";
            } else {
                // ✅ Step 3.2: 根据类型解码字段
                switch (col.mysql_type_id) {
                    case 3: { // MYSQL_TYPE_LONG (4字节有符号整数)
                        if (data_pos + 4 <= page_data_.size()) {
                            int32_t val = static_cast<int32_t>(MachRead::read_le<uint32_t>(page_data_.data(), data_pos));
                            field_value = std::to_string(val);
                            data_pos += 4;
                        }
                        break;
                    }
                    case 8: { // MYSQL_TYPE_LONGLONG (8字节有符号整数)
                        if (data_pos + 8 <= page_data_.size()) {
                            int64_t val = static_cast<int64_t>(MachRead::read_le<uint64_t>(page_data_.data(), data_pos));
                            field_value = std::to_string(val);
                            data_pos += 8;
                        }
                        break;
                    }
                    case 15: { // MYSQL_TYPE_VARCHAR (变长字符串)
                        if (data_pos + 1 <= page_data_.size()) {
                            uint8_t len = page_data_[data_pos];
                            data_pos++;
                            
                            if (len > 0 && data_pos + len <= page_data_.size()) {
                                field_value = std::string(
                                    reinterpret_cast<const char*>(page_data_.data() + data_pos), 
                                    len
                                );
                                data_pos += len;
                            }
                        }
                        break;
                    }
                    case 245: { // MYSQL_TYPE_JSON (JSON类型)
                        if (data_pos + 2 <= page_data_.size()) {
                            uint16_t len = MachRead::read_le<uint16_t>(page_data_.data(), data_pos);
                            data_pos += 2;
                            
                            if (len > 0 && data_pos + len <= page_data_.size()) {
                                // 简化处理，实际应解析JSON结构
                                field_value = "[JSON_DATA]";
                                data_pos += len;
                            }
                        }
                        break;
                    }
                    case 12: { // MYSQL_TYPE_DATETIME (8字节)
                        if (data_pos + 8 <= page_data_.size()) {
                            uint64_t val = MachRead::read_le<uint64_t>(page_data_.data(), data_pos);
                            // 简化处理，实际应解析为日期时间字符串
                            field_value = "[DATETIME:" + std::to_string(val) + "]";
                            data_pos += 8;
                        }
                        break;
                    }
                    default:
                        field_value = "[UNIMPLEMENTED_TYPE_" + std::to_string(col.mysql_type_id) + "]";
                        // 跳过未知类型的数据
                        if (col.column_length > 0 && col.column_length < 1024) {
                            data_pos += col.column_length;
                        }
                        break;
                }
            }
            
            record[col.name] = field_value;
        }
        
        result.push_back(record);
    }
    
    return result;
}

// ==================== BPlusTreeReconstructor 实现 ====================

BPlusTreeReconstructor::BPlusTreeReconstructor(uint32_t root_page_no)
    : root_page_no_(root_page_no) {
}

void BPlusTreeReconstructor::add_page(uint32_t page_no, std::vector<uint8_t> page_data) {
    pages_[page_no] = std::move(page_data);
}

bool BPlusTreeReconstructor::reconstruct() noexcept {
    if (pages_.empty()) {
        return false;
    }
    
    // 获取表结构（需要从 SDI 或其他元数据源获取）
    catalog::TableSchema schema;
    
    // 从根页开始遍历
    return traverse_tree(root_page_no_, schema);
}

bool BPlusTreeReconstructor::traverse_tree(uint32_t page_no, const catalog::TableSchema& schema) noexcept {
    auto it = pages_.find(page_no);
    if (it == pages_.end()) {
        std::cerr << "[ERROR] Page " << page_no << " not found in cache" << std::endl;
        return false;
    }
    
    BPlusTreeParser parser(it->second);
    if (!parser.parse()) {
        std::cerr << "[ERROR] Failed to parse page " << page_no << std::endl;
        return false;
    }
    
    // ✅ Step 1: 根据节点类型处理
    if (parser.node_type() == BTreeNodeType::LEAF) {
        // 叶子节点：直接解析记录
        auto records = parser.parse_records(schema);
        records_.insert(records_.end(), records.begin(), records.end());
    } else if (parser.node_type() == BTreeNodeType::INTERNAL) {
        // ✅ Step 2: 内部节点：递归遍历子节点
        // 解析内部节点的页指针（每个记录包含下级页号）
        const auto& record_offsets = parser.record_offsets();
        
        for (size_t i = 2; i < record_offsets.size(); ++i) { // 跳过 infimum/supremum
            uint16_t offset = record_offsets[i];
            
            if (offset + 10 > it->second.size()) {
                continue;
            }
            
            // ✅ Step 2.1: 解析记录中的页指针（第5-8字节是下级页号）
            uint32_t child_page_no = MachRead::read_le<uint32_t>(it->second.data(), offset + 4);
            
            if (child_page_no != 0xFFFFFFFF) {
                // ✅ Step 2.2: 递归遍历子节点
                traverse_tree(child_page_no, schema);
            }
        }
    } else if (parser.node_type() == BTreeNodeType::ROOT) {
        // 根节点：可能是叶子也可能是内部，根据页级别判断
        if (parser.page_level() == 0) {
            auto records = parser.parse_records(schema);
            records_.insert(records_.end(), records.begin(), records.end());
        } else {
            const auto& record_offsets = parser.record_offsets();
            
            for (size_t i = 2; i < record_offsets.size(); ++i) {
                uint16_t offset = record_offsets[i];
                
                if (offset + 10 > it->second.size()) {
                    continue;
                }
                
                uint32_t child_page_no = MachRead::read_le<uint32_t>(it->second.data(), offset + 4);
                
                if (child_page_no != 0xFFFFFFFF) {
                    traverse_tree(child_page_no, schema);
                }
            }
        }
    }
    
    return true;
}

} // namespace innodb
} // namespace dbrecovery
