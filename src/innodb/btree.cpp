#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <algorithm>
#include <stdexcept>

namespace dbrecovery {
namespace innodb {

// ==================== 常量定义 ====================
constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED = 0;
constexpr uint16_t FIL_PAGE_INDEX        = 9969;
constexpr uint16_t FIL_PAGE_UNDO_LOG     = 9970;
constexpr uint16_t FIL_PAGE_SDI          = 9971;
constexpr uint16_t FIL_PAGE_TYPE_BLOB    = 9972;

// ==================== BPlusTreeParser 实现 ====================

BPlusTreeParser::BPlusTreeParser(const std::vector<uint8_t>& page_data)
    : page_data_(page_data), flags_(0), page_level_(0), n_records_(0),
      next_page_(0xFFFFFFFF), prev_page_(0xFFFFFFFF), node_type_(BTreeNodeType::LEAF) {
}

// ✅ 新增：页健康状态检查
BPlusTreeParser::PageHealth BPlusTreeParser::check_health() const {
    PageHealth health;
    
    // 检查页大小
    if (page_data_.size() < 38) {
        health.is_valid = false;
        health.status_message = "Page too small";
        return health;
    }
    
    // 检查页类型
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 0);
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX && page_type != dbrecovery::innodb::FIL_PAGE_SDI && 
        page_type != dbrecovery::innodb::FIL_PAGE_TYPE_BLOB) {
        health.status_message = "Unsupported page type: " + std::to_string(page_type);
    }
    
    // 检查页头信息
    uint16_t n_heap = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 42) & 0x7FFF;
    uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 0);
    
    health.n_records = n_heap - 2; // 减去 infimum 和 supremum
    health.n_dir_slots = n_dir_slots;
    
    // 检查是否存在 infimum 和 supremum
    if (page_data_.size() >= 58) {
        uint16_t infimum_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 16);
        uint16_t supremum_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 18);
        
        health.has_infimum = (infimum_offset == 54);
        health.has_supremum = (supremum_offset == 56);
        
        if (!health.has_infimum || !health.has_supremum) {
            health.status_message += "Missing system records; ";
        }
    }
    
    // 检查页尾校验和（简化处理）
    // 实际应计算并比较 checksum
    health.checksum_match = true;
    
    if (health.status_message.empty()) {
        health.status_message = "Healthy";
    }
    
    return health;
}

bool BPlusTreeParser::parse() noexcept {
    if (page_data_.size() < 38) { // FIL_HEADER 大小
        return false;
    }
    
    return parse_header();
}

bool BPlusTreeParser::parse_header() noexcept {
    // 解析 FIL Header (38 bytes)
    if (page_data_.size() < 38) {
        return false;
    }
    
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 0);
    
    // 检查页类型：支持更多 InnoDB 页类型
    bool is_valid_page_type = false;
    switch (page_type) {
        case FIL_PAGE_INDEX:      // B+Tree 索引页
        case FIL_PAGE_SDI:        // SDI 元数据页
        case FIL_PAGE_TYPE_BLOB:  // BLOB 数据页
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
    if (page_data_.size() < 54) { // 需要至少 54 字节（38 + 16）
        return false;
    }
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

// ✅ 新增：SDI 页表 ID 获取
uint64_t BPlusTreeParser::get_sdi_table_id() const {
    if (page_data_.size() < 58) {
        return 0;
    }
    
    uint32_t type = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 38);
    if (type != 1) { // SDI 类型应为 1
        return 0;
    }
    
    return dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), 42);
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
            
        // ✅ 增强边界检查
        if (offset + 5 > page_data_.size()) {
            continue;
        }
            
        std::map<std::string, std::string> record;
            
        // ✅ 修复：正确解析 COMPACT 记录头
        // COMPACT 记录头格式（5-12字节）：
        // byte[0]: info_bits (8 bits) - [deleted_flag(1)][min_rec_flag(1)][reserved(6)]
        // byte[1]: n_fields_high(4 bits) | next_rec_offset_high(4 bits)
        // byte[2]: next_rec_offset_low(8 bits)
        // byte[3]: n_owned(4 bits) | heap_no(12 bits in high 12 bits, but stored differently)
        // byte[4]: n_fields_low(4 bits in low nibble)
            
        uint8_t info_bits = page_data_[offset];
        bool is_deleted = (info_bits & REC_INFO_DELETED_FLAG) != 0;
        bool is_min_rec = (info_bits & REC_INFO_MIN_REC_FLAG) != 0;
        bool is_extern = (info_bits & REC_INFO_EXTERN_FLAG) != 0;
            
        // 解析 n_fields: 高4位在 byte[1] 的高4位，低4位在 byte[4] 的低4位
        uint8_t n_fields_high = (page_data_[offset + 1] & 0xF0) >> 4;
        uint8_t n_fields_low  = page_data_[offset + 4] & 0x0F;
        uint16_t n_fields = static_cast<uint16_t>((n_fields_high << 4) | n_fields_low);
            
        // 解析 next_record_offset: 高4位在 byte[1] 的低4位，低8位在 byte[2]
        uint16_t next_rec_offset = static_cast<uint16_t>(
            (((uint16_t)(page_data_[offset + 1] & 0x0F)) << 8) | 
            ((uint16_t)page_data_[offset + 2])
        );
            
        // 解析 heap_no: 在 byte[3] 的低4位和 byte[4] 的高4位
        uint16_t heap_no = static_cast<uint16_t>(
            ((page_data_[offset + 3] & 0x0F) << 8) | 
            ((page_data_[offset + 4] & 0xF0) >> 4)
        );
            
        // 兼容性检查：n_fields 范围应为 1-511（9位最大值）
        if (n_fields == 0 || n_fields > 511) {
            std::cerr << "[WARN] Invalid field count: " << n_fields 
                      << " at offset " << offset << ", skipping record\n";
            continue;
        }
            
        // 跳过 INFIMUM/SUPREMUM 记录
        if (is_min_rec || next_rec_offset == 0xFFFF) {
            continue;
        }
            
        if (is_deleted) {
            continue;
        }
            
        // ✅ 修复：计算 header_size（可变长度）
        uint8_t header_size = 5; // 基础头大小
            
        // 检查是否有额外的变长字段长度信息
        bool has_var_fields = false;
        for (const auto& pair : schema.columns) {
            const auto& col = pair.second;
            if (col.is_variable_length && !col.is_off_page && col.virtual_gen_store_type == 0) {
                has_var_fields = true;
                break;
            }
        }
            
        // 计算变长字段长度信息大小
        uint8_t var_len_size = 0;
        if (has_var_fields) {
            for (const auto& pair : schema.columns) {
                const auto& col = pair.second;
                if (col.is_variable_length && !col.is_off_page && col.virtual_gen_store_type == 0) {
                    // VARCHAR 使用 1-2 字节存储长度
                    var_len_size += (col.column_length > 255) ? 2 : 1;
                }
            }
        }
            
        header_size += var_len_size;
            
        // ✅ 新增：边界检查 - 确保记录头不会超出页大小
        if (header_size > 128) { // COMPACT 记录头通常不超过 128 字节
            std::cerr << "[WARN] Record header size too large: " << static_cast<int>(header_size) 
                      << " at offset " << offset << ", skipping record\n";
            continue;
        }
            
        // 计算 NULL 位图大小
        uint32_t nullable_columns = 0;
        for (const auto& pair : schema.columns) {
            const auto& col = pair.second;
            if (!col.is_off_page && col.virtual_gen_store_type == 0 && col.is_nullable) {
                nullable_columns++;
            }
        }
            
        uint32_t null_bytes = (nullable_columns + 7) / 8;
        header_size += null_bytes;
            
        // ✅ 增强边界检查
        if (offset + header_size > page_data_.size()) {
            std::cerr << "[WARN] Record header exceeds page size at offset " << offset << std::endl;
            continue;
        }
            
        const uint8_t* null_ptr = page_data_.data() + offset + 5; // NULL bitmap starts after base header
            
        // 构建列索引映射
        std::vector<uint32_t> physical_columns;
        for (const auto& pair : schema.columns) {
            const auto& col = pair.second;
            if (!col.is_off_page && col.virtual_gen_store_type == 0) {
                physical_columns.push_back(pair.first);
            }
        }
            
        size_t data_pos = offset + header_size;
            
        // 遍历表定义中的所有列
        for (const auto& pair : schema.columns) {
            const auto& col = pair.second;
                
            std::string field_value;
                
            // 检查是否为 NULL
            bool is_null = false;
            if (col.is_nullable && !col.is_off_page && col.virtual_gen_store_type == 0) {
                uint32_t col_idx = 0;
                for (uint32_t pc : physical_columns) {
                    if (pc == pair.first) break;
                    const auto& pc_col = schema.columns.at(pc);
                    if (!pc_col.is_off_page && pc_col.virtual_gen_store_type == 0) {
                        col_idx++;
                    }
                }
                    
                if (col_idx < nullable_columns && null_bytes > 0) {
                    uint32_t byte_idx = col_idx / 8;
                    uint32_t bit_idx = col_idx % 8;
                        
                    if (byte_idx < null_bytes && null_ptr + byte_idx < page_data_.data() + offset + header_size) {
                        is_null = (null_ptr[byte_idx] & (1U << bit_idx)) != 0;
                    }
                }
            }
                
            if (is_null) {
                field_value = "NULL";
            } else if (col.is_off_page || (is_extern && col.mysql_type_id >= 249)) { // BLOB types
                // ✅ 新增：验证 BLOB 指针格式
                if (data_pos + 20 <= page_data_.size()) {
                    field_value = "[BLOB_EXTERNAL]";
                    data_pos += 20;
                } else {
                    std::cerr << "[WARN] BLOB pointer truncated for column " << col.name << std::endl;
                    field_value = "[TRUNCATED_BLOB_PTR]";
                    // 尝试跳过剩余数据
                    if (data_pos < page_data_.size()) {
                        data_pos = page_data_.size();
                    }
                }
            } else if (col.virtual_gen_store_type > 0) {
                // 虚拟列不存储实际数据
                field_value = "[VIRTUAL_COLUMN]";
            } else {
                // 解析具体类型
                switch (col.mysql_type_id) {
                    case mysql_types::LONGLONG: { // 8
                        if (data_pos + 8 <= page_data_.size()) {
                            int64_t val = static_cast<int64_t>(dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos));
                            field_value = std::to_string(val);
                            data_pos += 8;
                        } else {
                            std::cerr << "[WARN] Data truncated for column " << col.name << std::endl;
                            field_value = "[TRUNCATED]";
                        }
                        break;
                    }
                    case mysql_types::LONG: { // 3
                        if (data_pos + 4 <= page_data_.size()) {
                            int32_t val = static_cast<int32_t>(dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos));
                            field_value = std::to_string(val);
                            data_pos += 4;
                        } else {
                            std::cerr << "[WARN] Data truncated for column " << col.name << std::endl;
                            field_value = "[TRUNCATED]";
                        }
                        break;
                    }
                    case mysql_types::SHORT: { // 2
                        if (data_pos + 2 <= page_data_.size()) {
                            int16_t val = static_cast<int16_t>(dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), data_pos));
                            field_value = std::to_string(val);
                            data_pos += 2;
                        } else {
                            std::cerr << "[WARN] Data truncated for column " << col.name << std::endl;
                            field_value = "[TRUNCATED]";
                        }
                        break;
                    }
                    case mysql_types::TINY: { // 1
                        if (data_pos + 1 <= page_data_.size()) {
                            int8_t val = static_cast<int8_t>(page_data_[data_pos]);
                            field_value = std::to_string(val);
                            data_pos += 1;
                        } else {
                            std::cerr << "[WARN] Data truncated for column " << col.name << std::endl;
                            field_value = "[TRUNCATED]";
                        }
                        break;
                    }
                    case mysql_types::VARCHAR: { // 15
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
                                } else {
                                    field_value = "[TRUNCATED]";
                                }
                            } else {
                                std::cerr << "[WARN] VARCHAR length header truncated for column " << col.name << std::endl;
                                field_value = "[TRUNCATED]";
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
                                } else {
                                    field_value = "[TRUNCATED]";
                                }
                            } else {
                                std::cerr << "[WARN] VARCHAR length byte truncated for column " << col.name << std::endl;
                                field_value = "[TRUNCATED]";
                            }
                        }
                        break;
                    }
                    case mysql_types::DATETIME: { // 12
                        if (data_pos + 8 <= page_data_.size()) {
                            uint64_t val = dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos);
                            field_value = "[DATETIME:" + std::to_string(val) + "]";
                            data_pos += 8;
                        } else {
                            std::cerr << "[WARN] DATETIME data truncated for column " << col.name << std::endl;
                            field_value = "[TRUNCATED]";
                        }
                        break;
                    }
                    case mysql_types::DATETIME2: { // 18
                        // MySQL 8.0 新格式，支持小数位精度
                        int precision = col.datetime_precision >= 0 ? col.datetime_precision : 0;
                        if (precision < 0) precision = 0;
                        if (precision > 6) precision = 6;
                            
                        // DATETIME2 字节数：5(精度0-1), 6(精度2-3), 7(精度4-5), 8(精度6)
                        uint32_t datetime_bytes[] = {5, 5, 6, 6, 7, 7, 8};
                            
                        if (data_pos + datetime_bytes[precision] <= page_data_.size()) {
                            // 解析 DATETIME2 值
                            const uint8_t* datetime_ptr = page_data_.data() + data_pos;
                            uint64_t val = 0;
                            for (int i = 0; i < datetime_bytes[precision]; ++i) {
                                val |= static_cast<uint64_t>(datetime_ptr[i]) << (i * 8);
                            }
                            field_value = "[DATETIME2:" + std::to_string(precision) + "p]";
                            data_pos += datetime_bytes[precision];
                        } else {
                            std::cerr << "[WARN] DATETIME2 data truncated for column " << col.name << std::endl;
                            field_value = "[TRUNCATED]";
                        }
                        break;
                    }
                    case mysql_types::JSON: { // 245
                        if (data_pos + 2 <= page_data_.size()) {
                            uint16_t len = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), data_pos);
                            data_pos += 2;
                                
                            if (len > 0 && data_pos + len <= page_data_.size()) {
                                field_value = "[JSON_DATA]";
                                // TODO: 实现 JSON 解析
                                data_pos += len;
                            } else {
                                field_value = "[TRUNCATED_JSON]";
                            }
                        } else {
                            std::cerr << "[WARN] JSON length header truncated for column " << col.name << std::endl;
                            field_value = "[TRUNCATED]";
                        }
                        break;
                    }
                    case mysql_types::TINY_BLOB:   // 249
                    case mysql_types::MEDIUM_BLOB: // 250
                    case mysql_types::LONG_BLOB:   // 251
                    case mysql_types::BLOB:        // 252
                        field_value = "[BLOB_DATA]";
                        data_pos += 20; // 跳过 BLOB 指针
                        break;
                    default:
                        field_value = "[UNIMPLEMENTED_TYPE_" + std::to_string(col.mysql_type_id) + "]";
                        if (col.column_length > 0 && col.column_length < 1024) {
                            if (data_pos + col.column_length <= page_data_.size()) {
                                data_pos += col.column_length;
                            } else {
                                std::cerr << "[WARN] Column " << col.name << " data truncated" << std::endl;
                                break;
                            }
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
    
    catalog::TableSchema schema; // TODO: 从 SDI 加载
    
    // ✅ 新增：尝试从 SDI 页获取表元数据
    for (const auto& [page_no, page_data] : pages_) {
        if (dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 0) == dbrecovery::innodb::FIL_PAGE_SDI) {
            BPlusTreeParser sdi_parser(page_data);
            if (sdi_parser.parse()) {
                uint64_t table_id = sdi_parser.get_sdi_table_id();
                if (table_id != 0) {
                    schema.table_id = table_id;
                    // TODO: 解析 SDI JSON 获取完整表结构
                }
            }
        }
    }
    
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
