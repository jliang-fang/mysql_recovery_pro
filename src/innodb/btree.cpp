#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <algorithm>
#include <cstring>

namespace dbrecovery {
namespace innodb {

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
    
    // 检查页类型（FIL_PAGE_TYPE 在偏移0处是4字节）
    uint32_t page_type = dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), 0);
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX && page_type != dbrecovery::innodb::FIL_PAGE_SDI && 
        page_type != dbrecovery::innodb::FIL_PAGE_TYPE_BLOB) {
        health.status_message = "Unsupported page type: " + std::to_string(page_type);
    }
    
    // 检查页头信息
    // PAGE_N_HEAP 在偏移 42（BTR_N_LEVELS + 4），PAGE_N_DIR_SLOTS 在偏移 38（FIL_HEADER_SIZE）
    uint16_t n_heap = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 42) & 0x7FFF;
    uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 38 + 0);
    
    health.n_records = n_heap - 2; // 减去 infimum 和 supremum
    health.n_dir_slots = n_dir_slots;
    
    // 检查是否存在 infimum 和 supremum
    // infimum 应该在页目录开始位置 (38 + 16 = 54)
    // supremum 应该在 infimum 之后 (56)
    if (page_data_.size() >= 58) {
        uint16_t infimum_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 54);
        uint16_t supremum_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), 56);
        
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
        
    // 验证偏移量有效性
    if (infimum_offset < 38 + 16 || infimum_offset >= page_data_.size() ||
        supremum_offset < 38 + 16 || supremum_offset >= page_data_.size()) {
        return false;
    }
        
    // 验证 infimum 和 supremum 的值
    if (infimum_offset != 54 || supremum_offset != 56) {
        std::cerr << "[WARN] Invalid system record offsets: infimum=" << infimum_offset 
                  << ", supremum=" << supremum_offset << std::endl;
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

// ✅ 新增：提取类型解析逻辑为独立函数
std::string BPlusTreeParser::parse_field_value(
    const catalog::ColumnInfo& col,
    size_t& data_pos,
    bool is_extern) const {
    
    // 检查是否为 NULL（需要结合 NULL 位图）
    // 注意：此函数在 parse_records 中被调用，NULL 位图已在 parse_records 中计算
    // 这里仅做占位处理，实际 NULL 检查在 parse_records 的循环中完成
    
    if (col.is_off_page || (is_extern && col.mysql_type_id >= 249)) { // BLOB types
        if (data_pos + 20 <= page_data_.size()) {
            data_pos += 20;
            return "[BLOB_EXTERNAL]";
        } else {
            std::cerr << "[WARN] BLOB pointer truncated for column " << col.name << std::endl;
            if (data_pos < page_data_.size()) {
                data_pos = page_data_.size();
            }
            return "[TRUNCATED_BLOB_PTR]";
        }
    }
    
    if (col.virtual_gen_store_type > 0) {
        return "[VIRTUAL_COLUMN]";
    }
    
    // 解析具体类型
    switch (col.mysql_type_id) {
        case mysql_types::LONGLONG: { // 8
            if (data_pos + 8 <= page_data_.size()) {
                int64_t val = static_cast<int64_t>(dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos));
                data_pos += 8;
                return std::to_string(val);
            } else {
                std::cerr << "[WARN] Data truncated for column " << col.name << std::endl;
                return "[TRUNCATED]";
            }
        }
        case mysql_types::LONG: { // 3
            if (data_pos + 4 <= page_data_.size()) {
                int32_t val = static_cast<int32_t>(dbrecovery::innodb::read_le<uint32_t>(page_data_.data(), data_pos));
                data_pos += 4;
                return std::to_string(val);
            } else {
                std::cerr << "[WARN] Data truncated for column " << col.name << std::endl;
                return "[TRUNCATED]";
            }
        }
        case mysql_types::SHORT: { // 2
            if (data_pos + 2 <= page_data_.size()) {
                int16_t val = static_cast<int16_t>(dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), data_pos));
                data_pos += 2;
                return std::to_string(val);
            } else {
                std::cerr << "[WARN] Data truncated for column " << col.name << std::endl;
                return "[TRUNCATED]";
            }
        }
        case mysql_types::TINY: { // 1
            if (data_pos + 1 <= page_data_.size()) {
                int8_t val = static_cast<int8_t>(page_data_[data_pos]);
                data_pos += 1;
                return std::to_string(val);
            } else {
                std::cerr << "[WARN] Data truncated for column " << col.name << std::endl;
                return "[TRUNCATED]";
            }
        }
        case mysql_types::VARCHAR: { // 15
            bool use_two_byte_prefix = (col.column_length > 255);
            
            if (use_two_byte_prefix) {
                if (data_pos + 2 <= page_data_.size()) {
                    uint16_t len = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), data_pos);
                    data_pos += 2;
                    
                    if (len > 0 && data_pos + len <= page_data_.size()) {
                        std::string result(
                            reinterpret_cast<const char*>(page_data_.data() + data_pos), 
                            len
                        );
                        data_pos += len;
                        return result;
                    } else {
                        return "[TRUNCATED]";
                    }
                } else {
                    std::cerr << "[WARN] VARCHAR length header truncated for column " << col.name << std::endl;
                    return "[TRUNCATED]";
                }
            } else {
                if (data_pos + 1 <= page_data_.size()) {
                    uint8_t len = page_data_[data_pos];
                    data_pos++;
                    
                    if (len > 0 && data_pos + len <= page_data_.size()) {
                        std::string result(
                            reinterpret_cast<const char*>(page_data_.data() + data_pos), 
                            len
                        );
                        data_pos += len;
                        return result;
                    } else {
                        return "[TRUNCATED]";
                    }
                } else {
                    std::cerr << "[WARN] VARCHAR length byte truncated for column " << col.name << std::endl;
                    return "[TRUNCATED]";
                }
            }
        }
        case mysql_types::DATETIME: { // 12
            if (data_pos + 8 <= page_data_.size()) {
                uint64_t val = dbrecovery::innodb::read_le<uint64_t>(page_data_.data(), data_pos);
                data_pos += 8;
                return "[DATETIME:" + std::to_string(val) + "]";
            } else {
                std::cerr << "[WARN] DATETIME data truncated for column " << col.name << std::endl;
                return "[TRUNCATED]";
            }
        }
        case mysql_types::DATETIME2: { // 18
            int precision = col.datetime_precision >= 0 ? col.datetime_precision : 0;
            if (precision < 0) precision = 0;
            if (precision > 6) precision = 6;
            
            // MySQL 8.0 DATETIME2 编码：前5字节是日期/时间值，后(precision+1)/2字节是小数部分
            size_t datetime_bytes = 5 + (precision + 1) / 2;
            
            if (data_pos + datetime_bytes <= page_data_.size()) {
                const uint8_t* datetime_ptr = page_data_.data() + data_pos;
                
                // 解析前5字节：year(2), month+day(1), hour+minute+second(2)
                uint16_t year = dbrecovery::innodb::read_le<uint16_t>(datetime_ptr, 0);
                uint8_t month = datetime_ptr[2] & 0x0F;
                uint8_t day = datetime_ptr[2] >> 4;
                uint8_t hour = datetime_ptr[3] & 0x1F;
                uint8_t minute = datetime_ptr[3] >> 5 | (datetime_ptr[4] & 0x03) << 3;
                uint8_t second = datetime_ptr[4] >> 2;
                
                // 解析小数部分
                uint64_t frac = 0;
                if (precision > 0) {
                    size_t frac_bytes = (precision + 1) / 2;
                    for (size_t i = 0; i < frac_bytes; ++i) {
                        frac |= static_cast<uint64_t>(datetime_ptr[5 + i]) << (i * 8);
                    }
                    // 根据精度缩放
                    static const uint64_t frac_scale[] = {0, 100000, 10000, 1000, 100, 10, 1};
                    frac = frac / frac_scale[precision];
                }
                
                data_pos += datetime_bytes;
                return std::to_string(year) + "-" + 
                       std::to_string(month) + "-" + 
                       std::to_string(day) + " " +
                       std::to_string(hour) + ":" + 
                       std::to_string(minute) + ":" + 
                       std::to_string(second) +
                       (precision > 0 ? "." + std::to_string(frac) : "");
            } else {
                std::cerr << "[WARN] DATETIME2 data truncated for column " << col.name << std::endl;
                return "[TRUNCATED]";
            }
        }
        case mysql_types::JSON: { // 245
            if (data_pos + 2 <= page_data_.size()) {
                uint16_t len = dbrecovery::innodb::read_le<uint16_t>(page_data_.data(), data_pos);
                data_pos += 2;
                
                if (len > 0 && data_pos + len <= page_data_.size()) {
                    data_pos += len;
                    return "[JSON_DATA]";
                } else {
                    return "[TRUNCATED_JSON]";
                }
            } else {
                std::cerr << "[WARN] JSON length header truncated for column " << col.name << std::endl;
                return "[TRUNCATED]";
            }
        }
        case mysql_types::TINY_BLOB:   // 249
        case mysql_types::MEDIUM_BLOB: // 250
        case mysql_types::LONG_BLOB:   // 251
        case mysql_types::BLOB:        // 252
            data_pos += 20; // 跳过 BLOB 指针
            return "[BLOB_DATA]";
        default:
            std::cerr << "[WARN] Unimplemented type: " << col.mysql_type_id << std::endl;
            if (col.column_length > 0 && col.column_length < 1024) {
                if (data_pos + col.column_length <= page_data_.size()) {
                    data_pos += col.column_length;
                } else {
                    std::cerr << "[WARN] Column " << col.name << " data truncated" << std::endl;
                }
            }
            return "[UNIMPLEMENTED_TYPE_" + std::to_string(col.mysql_type_id) + "]";
    }
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
        uint8_t info_bits = page_data_[offset];
        bool is_deleted = (info_bits & REC_INFO_DELETED_FLAG) != 0;
        bool is_min_rec = (info_bits & REC_INFO_MIN_REC_FLAG) != 0;
        // ✅ 使用 page_defs.hpp 中定义的 REC_INFO_EXTERN_FLAG
        bool is_extern = (info_bits & dbrecovery::innodb::REC_INFO_EXTERN_FLAG) != 0;
            
        // 解析 n_fields: 高4位在 byte[1] 的高4位，低4位在 byte[4] 的低4位
        uint8_t n_fields_high = (page_data_[offset + 1] & 0xF0) >> 4;
        uint8_t n_fields_low  = page_data_[offset + 4] & 0x0F;
        uint16_t n_fields = static_cast<uint16_t>((n_fields_high << 4) | n_fields_low);
            
        // 解析 next_record_offset: byte[1] 的低4位是高4位，byte[2] 是低8位
        // 这是一个 14 位的无符号整数，表示到下一个记录的偏移量（相对当前记录头）
        uint16_t next_rec_offset = static_cast<uint16_t>(
            ((static_cast<uint16_t>(page_data_[offset + 1] & 0x0F)) << 8) | 
            page_data_[offset + 2]
        );
            
        // 解析 heap_no: byte[3] 的低4位是低4位，byte[4] 的高4位是高4位
        uint16_t heap_no = static_cast<uint16_t>(
            ((static_cast<uint16_t>(page_data_[offset + 4] & 0xF0)) << 4) | 
            (page_data_[offset + 3] & 0x0F)
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
        if (header_size > 128) { // COMPACT 记录头通常不超过 64 字节，但考虑变长字段和NULL位图后可能更大
            std::cerr << "[WARN] Record header size too large: " << static_cast<int>(header_size) 
                      << " at offset " << offset << ", skipping record\n";
            continue;
        }
        
        // ✅ 新增：防止记录头超出页边界
        if (offset + header_size > page_data_.size()) {
            std::cerr << "[ERROR] Record header exceeds page size at offset " << offset 
                      << ", header_size=" << static_cast<int>(header_size) 
                      << ", page_offset=" << offset << ", page_size=" << page_data_.size() << std::endl;
            continue;
        }
            
        // 计算 NULL 位图大小：所有可空列（包括 BLOB）都需要在位图中占位
        // 注意：auto_increment 列隐式 NOT NULL，不需要在 NULL 位图中占位
        uint32_t nullable_columns = 0;
        for (const auto& pair : schema.columns) {
            const auto& col = pair.second;
            if (col.is_nullable && !col.is_auto_increment) {
                nullable_columns++;
            }
        }
        
        // ✅ 修复：确保 nullable_columns 不为负数（虽然类型是 unsigned）
        uint32_t null_bytes = 0;
        if (nullable_columns > 0) {
            null_bytes = (nullable_columns + 7) / 8;
        }
        
        // ✅ 防止整数溢出：null_bytes 不应超过合理范围（例如 128 字节）
        if (null_bytes > 128) {
            std::cerr << "[WARN] Null bitmap too large: " << null_bytes 
                      << " at offset " << offset << ", skipping record\n";
            continue;
        }
        
        header_size += null_bytes;
            
        // ✅ 增强边界检查
        if (offset + header_size > page_data_.size()) {
            std::cerr << "[WARN] Record header exceeds page size at offset " << offset 
                      << ", header_size=" << static_cast<int>(header_size) 
                      << ", page_size=" << page_data_.size() << std::endl;
            continue;
        }
            
        const uint8_t* null_ptr = page_data_.data() + offset + 5; // NULL bitmap starts after base header
            
        // 构建列索引映射：为每个可空列分配位图中的索引
        std::map<uint32_t, uint32_t> col_idx_map; // schema_ordinal -> bitmap_index
        uint32_t bitmap_index = 0;
        for (const auto& pair : schema.columns) {
            const auto& col = pair.second;
            if (col.is_nullable && !col.is_auto_increment) {
                col_idx_map[pair.first] = bitmap_index++;
            }
        }
            
        size_t data_pos = offset + header_size;
            
        // 遍历表定义中的所有列
        for (const auto& pair : schema.columns) {
            const auto& col = pair.second;
                
            std::string field_value;
                
            // 检查是否为 NULL（包括 BLOB 列）
            bool is_null = false;
            if (col.is_nullable && !col.is_auto_increment) {
                auto it = col_idx_map.find(pair.first);
                if (it != col_idx_map.end() && null_bytes > 0) {
                    uint32_t byte_idx = it->second / 8;
                    uint32_t bit_idx = it->second % 8;
                        
                    // ✅ 增强边界检查：确保 byte_idx 在有效范围内
                    if (byte_idx < null_bytes && 
                        null_ptr != nullptr &&
                        null_ptr + byte_idx < page_data_.data() + offset + header_size) {
                        is_null = (null_ptr[byte_idx] & (1U << bit_idx)) != 0;
                    }
                }
            }
                
            if (is_null) {
                field_value = "NULL";
            } else {
                // ✅ 使用提取的函数解析字段值
                field_value = parse_field_value(col, data_pos, is_extern);
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
