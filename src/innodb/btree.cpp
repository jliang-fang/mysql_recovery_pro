#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <algorithm>
#include <stdexcept>

namespace dbrecovery {
namespace innodb {

// ==================== BPlusTreeParser 实现 ====================

BPlusTreeParser::BPlusTreeParser(const std::vector<uint8_t>& page_data)
    : page_data_(page_data), flags_(0) {
}

bool BPlusTreeParser::parse() noexcept {
    if (page_data_.size() < 38) { // FIL_HEADER 大小
        return false;
    }
    
    return parse_header();
}

// ✅ 修复：删除重复定义的 parse_header()
bool BPlusTreeParser::parse_header() noexcept {
    // 解析 FIL Header (38 bytes)
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 0);
    
    // 检查页类型：支持更多 InnoDB 页类型
    bool is_valid_page_type = false;
    switch (page_type) {
        case dbrecovery::innodb::FIL_PAGE_INDEX:      // B+Tree 索引页
        case dbrecovery::innodb::FIL_PAGE_SDI:        // SDI 元数据页
        case dbrecovery::innodb::FIL_PAGE_TYPE_BLOB:  // BLOB 数据页
            is_valid_page_type = true;
            break;
        default:
            // 忽略其他类型页（如 UNDO_LOG）
            return false;
    }
    
    if (!is_valid_page_type) {
        return false;
    }
    
    // 解析 FIL Header 中的 flags (offset 24)
    if (page_data_.size() < 26) {
        return false;
    }
    flags_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 24);
    
    // 解析 B+Tree Header (38 bytes into page)
    page_level_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38);
    n_records_ = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 42);
    next_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 46);
    prev_page_ = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 50);
    
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
    if (page_data_.size() < 58) { // 至少需要58字节（56+2）
        return false;
    }
        
    uint16_t infimum_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 54);
    uint16_t supremum_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 56);
        
    if (infimum_offset < 38 + 16 || infimum_offset >= page_data_.size() ||
        supremum_offset < 38 + 16 || supremum_offset >= page_data_.size()) {
        return false;
    }
        
    record_offsets_.push_back(infimum_offset);
    record_offsets_.push_back(supremum_offset);
        
    return parse_user_records();
}

bool BPlusTreeParser::parse_user_records() noexcept {
    uint16_t dir_start = 60; // DIRECTORY START offset
    
    if (dir_start + 2 > page_data_.size()) {
        return false;
    }
    
    uint16_t dir_size = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), dir_start);
    
    for (uint16_t i = 0; i < dir_size; ++i) {
        uint16_t slot_offset = dir_start + 2 + i * 2;
        if (slot_offset + 2 > page_data_.size()) {
            break;
        }
        
        uint16_t record_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), slot_offset);
        if (record_offset > 0 && record_offset < 8192) {
            record_offsets_.push_back(record_offset);
        }
    }
    
    return true;
}

std::vector<std::map<std::string, std::string>> 
BPlusTreeParser::parse_records(const catalog::TableSchema& schema) const {
    std::vector<std::map<std::string, std::string>> result;
    
    for (size_t i = 2; i < record_offsets_.size(); ++i) {
        uint16_t offset = record_offsets_[i];
        
        if (offset + 5 > page_data_.size()) {
            continue;
        }
        
        std::map<std::string, std::string> record;
        
        uint8_t header_size = 5; // COMPACT 记录头固定大小
        
        if (offset + header_size > page_data_.size()) {
            continue;
        }
        
        uint8_t info_bits = page_data_[offset];
        bool is_deleted = (info_bits & REC_INFO_DELETED_FLAG) != 0;
        bool is_extern = (info_bits & REC_INFO_EXTERN_FLAG) != 0;
        
        // ✅ 修复：正确解析 n_fields
        // COMPACT 格式记录头：
        // byte[0]: info_bits (8 bits)
        // byte[1]: [n_fields(4 bits)][next_rec_offset_high(4 bits)]
        // byte[2]: next_rec_offset_low(8 bits)
        // n_fields 实际占用 9 bits，但只使用高7位（0-126），存储在 byte1[7:4] 和 byte2[7:4]
        
        uint8_t n_fields_high = (page_data_[offset + 1] & 0xF0) >> 4;
        uint8_t n_fields_low = (page_data_[offset + 2] & 0xF0) >> 4;
        uint16_t n_fields = static_cast<uint16_t>((n_fields_high << 4) | n_fields_low);
        
        // 兼容性检查：某些版本可能使用不同格式
        if (n_fields == 0 || n_fields > 1023) {
            std::cerr << "[WARN] Invalid field count: " << n_fields 
                      << " at offset " << offset << ", skipping record\n";
            continue;
        }
        
        // 调试信息（可选）
        // std::cout << "[DEBUG] Record at offset " << offset << ": n_fields=" << n_fields << "\n";
        
        if (is_deleted) {
            continue;
        }
        
        // 构建 NULL 位图映射
        std::vector<uint32_t> physical_column_ordinals;
        for (const auto& pair : schema.columns) {
            const auto& col = pair.second;
            if (!col.is_off_page && col.virtual_gen_store_type == 0) {
                physical_column_ordinals.push_back(pair.first);
            }
        }
        
        uint32_t nullable_columns = 0;
        for (uint32_t col_idx : physical_column_ordinals) {
            auto it = schema.columns.find(col_idx);
            if (it != schema.columns.end() && it->second.is_nullable) {
                nullable_columns++;
            }
        }
        
        uint32_t null_bytes = (nullable_columns + 7) / 8;
        const uint8_t* null_ptr = page_data_.data() + offset + header_size;
        
        std::map<uint32_t, uint32_t> column_nullable_index;
        uint32_t nullable_idx = 0;
        for (uint32_t col_idx : physical_column_ordinals) {
            auto it = schema.columns.find(col_idx);
            if (it != schema.columns.end() && it->second.is_nullable) {
                column_nullable_index[col_idx] = nullable_idx++;
            }
        }
        
        size_t data_pos = offset + header_size + null_bytes;
        
        for (const auto& pair : schema.columns) {
            const auto& col = pair.second;
            
            if (data_pos >= page_data_.size()) {
                break;
            }
            
            std::string field_value;
            
            bool is_null = false;
            auto it = column_nullable_index.find(pair.first);
            if (col.is_nullable && it != column_nullable_index.end() && null_bytes > 0) {
                uint32_t col_idx = it->second;
                if (col_idx < nullable_columns) {
                    uint32_t byte_idx = col_idx / 8;
                    uint32_t bit_idx = col_idx % 8;
                    is_null = (null_ptr[byte_idx] & (1U << bit_idx)) != 0;
                }
            }
            
            if (is_null) {
                field_value = "NULL";
            } else {
                if (is_extern && col.is_off_page) {
                    field_value = "[BLOB_EXTERNAL]";
                    if (data_pos + 20 <= page_data_.size()) {
                        data_pos += 20;
                    }
                } else {
                    switch (col.mysql_type_id) {
                        case 3: { // MYSQL_TYPE_LONG
                            if (data_pos + 4 <= page_data_.size()) {
                                int32_t val = static_cast<int32_t>(dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos));
                                field_value = std::to_string(val);
                                data_pos += 4;
                            }
                            break;
                        }
                        case 8: { // MYSQL_TYPE_LONGLONG
                            if (data_pos + 8 <= page_data_.size()) {
                                int64_t val = static_cast<int64_t>(dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos));
                                field_value = std::to_string(val);
                                data_pos += 8;
                            }
                            break;
                        }
                        case 15: { // MYSQL_TYPE_VARCHAR
                            bool use_two_byte_prefix = (col.column_length > 255);
                            
                            if (use_two_byte_prefix) {
                                if (data_pos + 2 <= page_data_.size()) {
                                    uint16_t len = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), data_pos);
                                    data_pos += 2;
                                    
                                    if (len > 0 && data_pos + len <= page_data_.size()) {
                                        field_value = std::string(
                                            reinterpret_cast<const char*>(page_data_.data() + data_pos), 
                                            len
                                        );
                                        data_pos += len;
                                    }
                                }
                            } else {
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
                            }
                            break;
                        }
                        case 245: { // MYSQL_TYPE_JSON
                            if (data_pos + 2 <= page_data_.size()) {
                                uint16_t len = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), data_pos);
                                data_pos += 2;
                                
                                if (len > 0 && data_pos + len <= page_data_.size()) {
                                    field_value = "[JSON_DATA]";
                                    data_pos += len;
                                }
                            }
                            break;
                        }
                        case 12: { // MYSQL_TYPE_DATETIME
                            if (data_pos + 8 <= page_data_.size()) {
                                uint64_t val = dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos);
                                field_value = "[DATETIME:" + std::to_string(val) + "]";
                                data_pos += 8;
                            }
                            break;
                        }
                        default:
                            field_value = "[UNIMPLEMENTED_TYPE_" + std::to_string(col.mysql_type_id) + "]";
                            if (col.column_length > 0 && col.column_length < 1024) {
                                data_pos += col.column_length;
                            }
                            break;
                    }
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
    
    catalog::TableSchema schema; // TODO: 从 SDI 加载
    
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
    
    if (parser.node_type() == BTreeNodeType::LEAF) {
        auto records = parser.parse_records(schema);
        records_.insert(records_.end(), records.begin(), records.end());
    } else if (parser.node_type() == BTreeNodeType::INTERNAL || parser.node_type() == BTreeNodeType::ROOT) {
        const auto& record_offsets = parser.record_offsets();
        
        for (size_t i = 2; i < record_offsets.size(); ++i) {
            uint16_t offset = record_offsets[i];
            
            if (offset + 5 > it->second.size()) {
                continue;
            }
            
            // 内部节点记录格式：[5-8字节] 子页号（偏移 offset+4）
            uint32_t child_page_no = dbrecovery::innodb::read_le<uint32_t>(it->second.data(), offset + 4);
            
            if (child_page_no != 0xFFFFFFFF && child_page_no < 0xFFFFFFF0) {
                traverse_tree(child_page_no, schema);
            }
        }
    }
    
    return true;
}

} // namespace innodb
} // namespace dbrecovery
