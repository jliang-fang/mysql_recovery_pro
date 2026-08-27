#include "dbrecovery/innodb/record_parser.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// ==================== RedundantRecordParser 实现 ====================

std::optional<RecordHeaderBase> RedundantRecordParser::parse_header(
    const uint8_t* data, size_t size, size_t offset) {
    
    if (!data || offset + 6 > size) {
        return std::nullopt;
    }
    
    RedundantRecordHeader header;
    header.offset = static_cast<uint16_t>(offset);
    header.row_format = RowFormat::REDUNDANT;
    
    // 读取 info_bits (byte 0)
    uint8_t info_bits = data[offset];
    header.info_bits = info_bits;
    
    // 解析状态位
    header.n_owned = info_bits & 0x0F;  // 低4位
    header.status = (info_bits >> 4) & 0x03;  // 位4-5
    bool next_rec_high = (info_bits & 0x80) != 0;  // 位7
    
    // 读取 next_record_offset (bytes 1-2)
    uint16_t next_rec_low = read_le<uint16_t>(data, offset + 1);
    header.next_offset = static_cast<uint16_t>(
        (next_rec_high ? 0x4000 : 0) | (next_rec_low & 0x3FFF));
    
    // 读取 heap_no (bytes 3-4)
    header.heap_no = read_le<uint16_t>(data, offset + 3) & 0x3FFF;
    
    // 读取 record_length (bytes 5-6)
    header.record_length = read_le<uint16_t>(data, offset + 5) & 0x3FFF;
    
    // 设置状态标志
    uint8_t status = header.status;
    header.is_infimum = (status == REC_STATUS_INFIMUM);
    header.is_supremum = (status == REC_STATUS_SUPREMUM);
    header.is_node_ptr = (status == REC_STATUS_NODE_PTR);
    header.is_min_rec = (info_bits & REC_INFO_MIN_REC_FLAG) != 0;
    header.is_deleted = (info_bits & REC_INFO_DELETED_FLAG) != 0;
    
    // 解析变长字段长度列表
    size_t var_len_start = offset + 7;
    size_t var_len_pos = 0;
    while (var_len_start + var_len_pos + 1 <= size && 
           var_len_pos < header.record_length) {
        size_t read_size = 0;
        uint16_t len = read_var_len(data, var_len_start + var_len_pos, read_size);
        if (read_size == 0) break;
        header.var_len.push_back(static_cast<uint8_t>(len));
        var_len_pos += read_size;
    }
    
    // 解析可空字段长度列表
    // 在 Redundant 格式中，可空字段长度在变长字段长度之后
    size_t null_len_start = var_len_start + var_len_pos;
    size_t null_len_pos = 0;
    while (null_len_start + null_len_pos < size && 
           null_len_pos < header.record_length) {
        if (null_len_start + null_len_pos + 2 > size) break;
        uint16_t len = read_le<uint16_t>(data, null_len_start + null_len_pos);
        header.null_len.push_back(len);
        null_len_pos += 2;
    }
    
    return header;
}

std::vector<ColumnData> RedundantRecordParser::parse_columns(
    const uint8_t* data, size_t size, size_t offset,
    const RecordHeaderBase& header_base, const std::vector<ColumnInfo>& columns) {
    
    std::vector<ColumnData> result;
    
    const RedundantRecordHeader& header = 
        static_cast<const RedundantRecordHeader&>(header_base);
    
    // 跳过记录头和变长字段长度列表
    size_t data_pos = offset + 7 + header.var_len.size();
    
    // 跳过可空字段长度列表
    data_pos += header.null_len.size() * 2;
    
    // 解析每个字段
    for (size_t i = 0; i < columns.size(); ++i) {
        ColumnData col_data;
        col_data.info = columns[i];
        
        if (i < header.null_len.size() && header.null_len[i] == 0xFFFF) {
            // NULL 值
            col_data.value = std::monostate{};
            result.push_back(col_data);
            continue;
        }
        
        if (i < header.var_len.size()) {
            // 变长字段
            uint8_t len = header.var_len[i];
            if (data_pos + len > size) {
                break;
            }
            
            if (columns[i].is_binary) {
                col_data.value = std::vector<uint8_t>(
                    data + data_pos, data + data_pos + len);
            } else {
                col_data.value = std::string(
                    reinterpret_cast<const char*>(data + data_pos), len);
            }
            data_pos += len;
        } else {
            // 固定长度字段
            if (data_pos + columns[i].length > size) {
                break;
            }
            
            const uint8_t* field_data = data + data_pos;
            
            switch (columns[i].type) {
                case ColumnType::TINY_INT:
                    if (columns[i].is_unsigned) {
                        col_data.value = *field_data;
                    } else {
                        col_data.value = static_cast<int8_t>(*field_data);
                    }
                    break;
                    
                case ColumnType::SHORT_INT:
                    if (columns[i].is_unsigned) {
                        col_data.value = read_le<uint16_t>(field_data);
                    } else {
                        col_data.value = static_cast<int16_t>(read_le<uint16_t>(field_data));
                    }
                    break;
                    
                case ColumnType::LONG_INT:
                    if (columns[i].is_unsigned) {
                        col_data.value = read_le<uint32_t>(field_data);
                    } else {
                        col_data.value = static_cast<int32_t>(read_le<uint32_t>(field_data));
                    }
                    break;
                    
                case ColumnType::LONGLONG:
                    if (columns[i].is_unsigned) {
                        col_data.value = read_le<uint64_t>(field_data);
                    } else {
                        col_data.value = static_cast<int64_t>(read_le<uint64_t>(field_data));
                    }
                    break;
                    
                case ColumnType::FLOAT:
                    // 简化：直接读取4字节
                    col_data.value = *reinterpret_cast<const float*>(field_data);
                    break;
                    
                case ColumnType::DOUBLE:
                    // 简化：直接读取8字节
                    col_data.value = *reinterpret_cast<const double*>(field_data);
                    break;
                    
                default:
                    // 其他类型作为二进制数据
                    col_data.value = std::vector<uint8_t>(
                        field_data, field_data + columns[i].length);
                    break;
            }
            data_pos += columns[i].length;
        }
        
        result.push_back(col_data);
    }
    
    return result;
}

uint16_t RedundantRecordParser::get_record_length(
    const uint8_t* data, size_t size, size_t offset) {
    
    if (!data || offset + 7 > size) {
        return 0;
    }
    
    return read_le<uint16_t>(data, offset + 5) & 0x3FFF;
}

bool RedundantRecordParser::is_system_record(
    const uint8_t* data, size_t size, size_t offset) {
    
    if (!data || offset >= size) {
        return false;
    }
    
    uint8_t info_bits = data[offset];
    uint8_t status = info_bits & 0x07;
    
    return status == REC_STATUS_INFIMUM || status == REC_STATUS_SUPREMUM;
}

uint16_t RedundantRecordParser::read_var_len(
    const uint8_t* data, size_t offset, size_t& read_size) const {

    if (offset >= read_size) {
        read_size = 0;
        return 0;
    }

    uint8_t first_byte = data[offset];

    if (first_byte < 0x80) {
        // 1字节编码：0xxxxxxx (0-127)
        read_size = 1;
        return first_byte;
    } else {
        // 2字节编码：1xxxxxxx xxxxxxxx (128-32767)
        if (offset + 1 >= read_size) {
            read_size = 0;
            return 0;
        }
        read_size = 2;
        uint16_t val = static_cast<uint16_t>(first_byte & 0x7F);
        val = static_cast<uint16_t>((val << 8) | data[offset + 1]);
        return val;
    }
}

// ==================== CompactRecordParser 实现 ====================

std::optional<RecordHeaderBase> CompactRecordParser::parse_header(
    const uint8_t* data, size_t size, size_t offset) {
    
    if (!data || offset + 5 > size) {
        return std::nullopt;
    }
    
    CompactRecordHeader header;
    header.offset = static_cast<uint16_t>(offset);
    header.row_format = RowFormat::COMPACT;
    
    // 读取 info_bits (byte 0)
    uint8_t info_bits = data[offset];
    header.info_bits = info_bits;
    
    // 解析状态位 (低3位)
    uint8_t status = info_bits & 0x07;
    header.is_infimum = (status == REC_STATUS_INFIMUM);
    header.is_supremum = (status == REC_STATUS_SUPREMUM);
    header.is_node_ptr = (status == REC_STATUS_NODE_PTR);
    header.is_min_rec = (info_bits & REC_INFO_MIN_REC_FLAG) != 0;
    header.is_deleted = (info_bits & REC_INFO_DELETED_FLAG) != 0;
    
    // 读取 next_record_offset (bytes 1-2)
    header.next_offset = read_le<uint16_t>(data, offset + 1) & 0x3FFF;
    
    // 读取 heap_no (bytes 3-4)
    header.heap_no = read_le<uint16_t>(data, offset + 3) & 0x3FFF;
    
    // 读取 n_fields (byte 5)
    if (offset + 5 < size) {
        header.n_fields = (info_bits & 0xF8) >> 3;
        if (offset + 6 < size) {
            header.n_fields = static_cast<uint16_t>(
                (header.n_fields << 4) | (data[offset + 5] >> 4));
        }
    }
    
    // 解析变长字段长度列表
    size_t var_len_start = offset + 6;
    size_t var_len_pos = 0;
    while (var_len_start + var_len_pos < size) {
        size_t read_size = 0;
        uint16_t len = read_var_len(data, var_len_start + var_len_pos, read_size);
        if (read_size == 0) break;
        header.var_len.push_back(static_cast<uint8_t>(len));
        var_len_pos += read_size;
    }
    
    // 解析可空字段位图
    size_t null_bitmap_start = var_len_start + var_len_pos;
    size_t null_bytes = get_null_bytes(header.n_fields);
    for (size_t i = 0; i < null_bytes; ++i) {
        if (null_bitmap_start + i < size) {
            header.null_bitmap.push_back(data[null_bitmap_start + i]);
        }
    }
    
    return header;
}

std::vector<ColumnData> CompactRecordParser::parse_columns(
    const uint8_t* data, size_t size, size_t offset,
    const RecordHeaderBase& header_base, const std::vector<ColumnInfo>& columns) {
    
    std::vector<ColumnData> result;
    
    const CompactRecordHeader& header = 
        static_cast<const CompactRecordHeader&>(header_base);
    
    // 跳过记录头
    size_t data_pos = offset + 6;
    
    // 跳过变长字段长度列表
    data_pos += header.var_len.size();
    
    // 跳过可空字段位图
    data_pos += header.null_bitmap.size();
    
    // 解析每个字段
    for (size_t i = 0; i < columns.size(); ++i) {
        ColumnData col_data;
        col_data.info = columns[i];
        
        // 检查 NULL 位
        if (columns[i].is_nullable) {
            size_t byte_idx = i / 8;
            size_t bit_idx = i % 8;
            if (byte_idx < header.null_bitmap.size()) {
                if ((header.null_bitmap[byte_idx] & (1 << bit_idx)) != 0) {
                    col_data.value = std::monostate{};
                    result.push_back(col_data);
                    continue;
                }
            }
        }
        
        if (i < header.var_len.size()) {
            // 变长字段
            uint8_t len = header.var_len[i];
            if (data_pos + len > size) {
                break;
            }
            
            if (columns[i].is_binary) {
                col_data.value = std::vector<uint8_t>(
                    data + data_pos, data + data_pos + len);
            } else {
                col_data.value = std::string(
                    reinterpret_cast<const char*>(data + data_pos), len);
            }
            data_pos += len;
        } else {
            // 固定长度字段
            if (data_pos + columns[i].length > size) {
                break;
            }
            
            const uint8_t* field_data = data + data_pos;
            
            switch (columns[i].type) {
                case ColumnType::TINY_INT:
                    if (columns[i].is_unsigned) {
                        col_data.value = *field_data;
                    } else {
                        col_data.value = static_cast<int8_t>(*field_data);
                    }
                    break;
                    
                case ColumnType::SHORT_INT:
                    if (columns[i].is_unsigned) {
                        col_data.value = read_le<uint16_t>(field_data);
                    } else {
                        col_data.value = static_cast<int16_t>(read_le<uint16_t>(field_data));
                    }
                    break;
                    
                case ColumnType::LONG_INT:
                    if (columns[i].is_unsigned) {
                        col_data.value = read_le<uint32_t>(field_data);
                    } else {
                        col_data.value = static_cast<int32_t>(read_le<uint32_t>(field_data));
                    }
                    break;
                    
                case ColumnType::LONGLONG:
                    if (columns[i].is_unsigned) {
                        col_data.value = read_le<uint64_t>(field_data);
                    } else {
                        col_data.value = static_cast<int64_t>(read_le<uint64_t>(field_data));
                    }
                    break;
                    
                case ColumnType::FLOAT:
                    col_data.value = *reinterpret_cast<const float*>(field_data);
                    break;
                    
                case ColumnType::DOUBLE:
                    col_data.value = *reinterpret_cast<const double*>(field_data);
                    break;
                    
                default:
                    col_data.value = std::vector<uint8_t>(
                        field_data, field_data + columns[i].length);
                    break;
            }
            data_pos += columns[i].length;
        }
        
        result.push_back(col_data);
    }
    
    return result;
}

uint16_t CompactRecordParser::get_record_length(
    const uint8_t* data, size_t size, size_t offset) {
    
    if (!data || offset + 6 > size) {
        return 0;
    }
    
    // Compact 格式中，记录长度需要计算
    // 从 next_record_offset 推断
    uint16_t next_offset = read_le<uint16_t>(data, offset + 1) & 0x3FFF;
    if (next_offset == 0x3FFF || next_offset <= offset) {
        return 0;
    }
    return static_cast<uint16_t>(next_offset - offset);
}

bool CompactRecordParser::is_system_record(
    const uint8_t* data, size_t size, size_t offset) {
    
    if (!data || offset >= size) {
        return false;
    }
    
    uint8_t info_bits = data[offset];
    uint8_t status = info_bits & 0x07;
    
    return status == REC_STATUS_INFIMUM || status == REC_STATUS_SUPREMUM;
}

uint16_t CompactRecordParser::read_var_len(
    const uint8_t* data, size_t offset, size_t& read_size) const {

    if (offset >= read_size) {
        read_size = 0;
        return 0;
    }

    uint8_t first_byte = data[offset];

    if (first_byte < 0x80) {
        // 1字节编码
        read_size = 1;
        return first_byte;
    } else {
        // 2字节编码
        if (offset + 1 >= read_size) {
            read_size = 0;
            return 0;
        }
        read_size = 2;
        uint16_t val = static_cast<uint16_t>((first_byte & 0x7F) << 8);
        val = static_cast<uint16_t>(val | data[offset + 1]);
        return val;
    }
}

size_t CompactRecordParser::get_null_bytes(size_t n_fields) const {
    return (n_fields + 7) / 8;
}

// ==================== DynamicRecordParser 实现 ====================

std::optional<RecordHeaderBase> DynamicRecordParser::parse_header(
    const uint8_t* data, size_t size, size_t offset) {
    
    auto base_header = CompactRecordParser::parse_header(data, size, offset);
    if (!base_header) {
        return std::nullopt;
    }
    
    DynamicRecordHeader& header = static_cast<DynamicRecordHeader&>(*base_header);
    header.row_format = RowFormat::DYNAMIC;
    
    // 解析 Instant DDL 元数据
    parse_instant_ddl_metadata(data, size, offset, header);
    
    return header;
}

std::vector<ColumnData> DynamicRecordParser::parse_columns(
    const uint8_t* data, size_t size, size_t offset,
    const RecordHeaderBase& header_base, const std::vector<ColumnInfo>& columns) {
    
    // Dynamic 格式在 Compact 基础上增加了新增列的处理
    std::vector<ColumnData> result = 
        CompactRecordParser::parse_columns(data, size, offset, header_base, columns);
    
    const DynamicRecordHeader& header = 
        static_cast<const DynamicRecordHeader&>(header_base);
    
    // 如果有新增列，添加默认值
    if (header.n_added_cols > 0 && header.n_default_cols > 0) {
        // 添加默认值到结果中
        for (uint16_t i = 0; i < header.n_default_cols; ++i) {
            if (i < columns.size()) {
                ColumnData col_data;
                col_data.info = columns[i];
                col_data.value = std::monostate{}; // 使用默认值
                result.push_back(col_data);
            }
        }
    }
    
    return result;
}

bool DynamicRecordParser::is_system_record(
    const uint8_t* data, size_t size, size_t offset) {
    
    return CompactRecordParser::is_system_record(data, size, offset);
}

bool DynamicRecordParser::parse_instant_ddl_metadata(
    const uint8_t* data, size_t size, size_t offset,
    DynamicRecordHeader& header) {
    
    // Instant DDL 元数据位于记录头之后
    // 位于变长字段长度列表和可空位图之后
    
    size_t meta_pos = offset + 6;
    
    // 跳过变长字段长度列表
    while (meta_pos < size) {
        uint8_t first_byte = data[meta_pos];
        meta_pos++;
        if (first_byte >= 0x80 && meta_pos < size) {
            meta_pos++;
        }
        if (first_byte < 0x80) break;
    }
    
    // 跳过可空位图
    size_t null_bytes = (header.n_fields + 7) / 8;
    meta_pos += null_bytes;
    
    // 读取 metadata_version
    if (meta_pos + 1 <= size) {
        header.metadata_version = data[meta_pos];
        meta_pos++;
    }
    
    // 读取 instant_flags
    if (meta_pos + 1 <= size) {
        header.instant_flags = data[meta_pos];
        meta_pos++;
    }
    
    // 读取 n_added_cols 和 n_default_cols
    if (meta_pos + 4 <= size) {
        header.n_added_cols = read_le<uint16_t>(data, meta_pos);
        header.n_default_cols = read_le<uint16_t>(data, meta_pos + 2);
    }
    
    return true;
}

// ==================== RecordParserFactory 实现 ====================

std::unique_ptr<IRecordParser> RecordParserFactory::create_parser(RowFormat format) {
    switch (format) {
        case RowFormat::REDUNDANT:
            return std::make_unique<RedundantRecordParser>();
        case RowFormat::COMPACT:
            return std::make_unique<CompactRecordParser>();
        case RowFormat::DYNAMIC:
        case RowFormat::COMPRESSED:
            return std::make_unique<DynamicRecordParser>();
        default:
            return std::make_unique<CompactRecordParser>();
    }
}

RowFormat RecordParserFactory::detect_row_format(
    const uint8_t* data, size_t size, size_t offset) {
    
    if (!data || offset >= size) {
        return RowFormat::UNKNOWN;
    }
    
    // 检查记录头长度
    // Redundant: 6 + 变长字段长度
    // Compact: 5 + 变长字段长度
    // Dynamic: 5 + 变长字段长度 + metadata_version
    
    uint8_t info_bits = data[offset];
    uint8_t status = info_bits & 0x07;
    
    // 检查是否是系统记录
    if (status == REC_STATUS_INFIMUM || status == REC_STATUS_SUPREMUM) {
        return RowFormat::COMPACT;  // 默认
    }
    
    // 检查 next_record_offset 格式
    if (offset + 6 <= size) {
        uint8_t n_fields_high = (info_bits & 0xF8) >> 3;
        if (n_fields_high > 0) {
            return RowFormat::COMPACT;
        }
    }
    
    return RowFormat::COMPACT;
}

RowFormat RecordParserFactory::detect_row_format_from_page(
    const uint8_t* page_data, size_t page_size) {
    
    if (!page_data || page_size < 40) {
        return RowFormat::UNKNOWN;
    }
    
    // 从页头的 flags 中提取行格式
    // flags 位于 offset 38 + 42 = 80
    if (page_size >= 82) {
        uint16_t flags = read_le<uint16_t>(page_data, 80);
        uint8_t row_format_val = flags & 0x07;
        
        switch (row_format_val) {
            case 0: return RowFormat::REDUNDANT;
            case 1: return RowFormat::COMPACT;
            case 2: return RowFormat::DYNAMIC;
            case 3: return RowFormat::COMPRESSED;
        }
    }
    
    // 默认返回 COMPACT
    return RowFormat::COMPACT;
}

// ==================== RecordScanner 实现 ====================

RecordScanner::RecordScanner(const std::vector<ColumnInfo>& columns)
    : columns_(columns) {
}

std::vector<RecordData> RecordScanner::scan_page(
    const uint8_t* page_data, size_t page_size,
    RowFormat format) {
    
    std::vector<RecordData> records;
    
    if (!page_data || page_size < 40) {
        return records;
    }
    
    // 读取页头信息
    uint16_t heap_top = read_le<uint16_t>(page_data, 40);
    uint16_t n_dir_slots = read_le<uint16_t>(page_data, 38);
    
    // 定位记录起始点（INFIMUM 记录）
    const uint8_t* rec = page_data + 54;  // INFIMUM 通常在 offset 54
    
    // 解析每条记录
    while (rec < page_data + heap_top) {
        RecordData record;
        record.offset = static_cast<uint16_t>(rec - page_data);
        
        // 解析记录头
        auto header_opt = RecordParserFactory::create_parser(format)->parse_header(
            page_data, page_size, record.offset);
        
        if (!header_opt) {
            break;
        }
        
        record.header = *header_opt;
        
        // 解析字段数据
        record.columns = RecordParserFactory::create_parser(format)->parse_columns(
            page_data, page_size, record.offset, record.header, columns_);
        
        // 检查是否是 SUPREMUM
        if (record.header.is_supremum) {
            break;
        }
        
        // 跳过系统记录
        if (!record.header.is_infimum && !record.header.is_deleted) {
            records.push_back(record);
        }
        
        // 移动到下一条记录
        if (record.header.next_offset == 0 || 
            record.header.next_offset <= record.offset ||
            record.header.next_offset >= heap_top) {
            break;
        }
        
        rec = page_data + record.header.next_offset;
    }
    
    return records;
}

std::vector<RecordData> RecordScanner::scan_range(
    const uint8_t* page_data, size_t page_size,
    uint16_t start_offset, uint16_t end_offset,
    RowFormat format) {
    
    std::vector<RecordData> records;
    
    if (!page_data || page_size < 40) {
        return records;
    }
    
    const uint8_t* rec = page_data + start_offset;
    
    while (rec < page_data + end_offset) {
        uint16_t rec_offset = static_cast<uint16_t>(rec - page_data);
        
        auto header_opt = RecordParserFactory::create_parser(format)->parse_header(
            page_data, page_size, rec_offset);
        
        if (!header_opt) {
            break;
        }
        
        RecordData record;
        record.header = *header_opt;
        record.offset = rec_offset;
        record.columns = RecordParserFactory::create_parser(format)->parse_columns(
            page_data, page_size, rec_offset, record.header, columns_);
        
        records.push_back(record);
        
        if (record.header.next_offset == 0 || 
            record.header.next_offset <= rec_offset ||
            record.header.next_offset >= end_offset) {
            break;
        }
        
        rec = page_data + record.header.next_offset;
    }
    
    return records;
}

// ==================== RecordReconstructor 实现 ====================

std::vector<ColumnData> RecordReconstructor::reconstruct_from_partial(
    const uint8_t* data, size_t size,
    const std::vector<ColumnInfo>& columns,
    size_t valid_bytes) {
    
    std::vector<ColumnData> result;
    
    size_t pos = 0;
    for (const auto& col : columns) {
        ColumnData col_data;
        col_data.info = col;
        
        if (pos + col.length > valid_bytes) {
            // 部分数据丢失，使用 NULL 或部分数据
            if (col.is_nullable) {
                col_data.value = std::monostate{};
            } else {
                // 尝试读取可用字节
                if (pos < valid_bytes) {
                    size_t available = valid_bytes - pos;
                    col_data.value = std::vector<uint8_t>(
                        data + pos, data + pos + available);
                } else {
                    col_data.value = std::monostate{};
                }
            }
        } else {
            // 完整数据
            if (col.is_binary) {
                col_data.value = std::vector<uint8_t>(
                    data + pos, data + pos + col.length);
            } else {
                col_data.value = std::string(
                    reinterpret_cast<const char*>(data + pos), col.length);
            }
        }
        
        pos += col.length;
        result.push_back(col_data);
    }
    
    return result;
}

RecordData RecordReconstructor::reconstruct_from_undo(
    const uint8_t* undo_data, size_t undo_size,
    const std::vector<ColumnInfo>& columns,
    UndoFormatProfile profile) {

    RecordData record;

    if (undo_data == nullptr || undo_size < 2 ||
        profile == UndoFormatProfile::UNRESOLVED) {
        return record;
    }

    // Undo Log 记录格式：
    // - 1 byte: undo record type
    // - 1 byte: field_count (or the 6-byte trx_id starts here for UPDATE/DELETE)
    // - trx_id + roll_ptr widths are pinned by the resolved profile
    //
    // Roll pointer storage width is version-dependent:
    //   LEGACY_6BYTE (5.6/5.7): 6 bytes big-endian
    //   MYSQL8_7BYTE (8.0+):    7 bytes big-endian
    // UNRESOLVED rejects the record before any byte is interpreted.

    size_t pos = 1;  // 跳过类型字节

    if (pos + 6 > undo_size) {
        return record;
    }

    // trx_id: 6 bytes little-endian (both profiles)
    uint64_t trx_id = 0;
    for (int i = 0; i < 6; ++i) {
        trx_id |= static_cast<uint64_t>(undo_data[pos + i]) << (8 * i);
    }
    record.trx_id = trx_id;
    pos += 6;

    // roll_ptr: profile-pinned width, big-endian storage
    const size_t roll_ptr_len =
        (profile == UndoFormatProfile::MYSQL8_7BYTE) ? 7 : 6;
    if (pos + roll_ptr_len > undo_size) {
        return record;
    }
    auto decoded = RollPtrDecoder::decode_storage(
        undo_data + pos, roll_ptr_len, profile);
    if (!decoded.is_valid()) {
        return record;
    }
    record.roll_ptr = RollPtrDecoder::encode(
        decoded.page_no, decoded.offset, decoded.flags);
    pos += roll_ptr_len;

    // 读取 n_fields
    if (pos + 2 > undo_size) {
        return RecordData{};
    }
    const uint16_t n_fields = read_le<uint16_t>(undo_data, pos);
    pos += 2;

    // 解析字段数据
    for (uint16_t i = 0; i < n_fields && i < columns.size(); ++i) {
        ColumnData col_data;
        col_data.info = columns[i];

        if (pos >= undo_size) {
            break;
        }

        // 读取变长字段长度
        uint8_t first_byte = undo_data[pos++];
        uint16_t len = first_byte;
        if (first_byte >= 0x80) {
            if (pos < undo_size) {
                len = static_cast<uint16_t>((first_byte & 0x7F) << 8 | undo_data[pos++]);
            }
        }

        if (pos + len > undo_size) {
            break;
        }

        if (columns[i].is_binary) {
            col_data.value = std::vector<uint8_t>(
                undo_data + pos, undo_data + pos + len);
        } else {
            col_data.value = std::string(
                reinterpret_cast<const char*>(undo_data + pos), len);
        }

        pos += len;
        record.columns.push_back(col_data);
    }

    // A truncated field list is not a partially recoverable structured record.
    // Reject the whole record so incomplete undo data cannot be exported.
    if (record.columns.size() != std::min<size_t>(n_fields, columns.size())) {
        return RecordData{};
    }

    return record;
}

} // namespace innodb
} // namespace dbrecovery
