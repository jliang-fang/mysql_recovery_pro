#include "dbrecovery/innodb/frm_parser.hpp"
#include <fstream>
#include <iostream>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// FRM 文件魔数
constexpr uint16_t FRM_TYPE_TABLE = 0x01fe;
constexpr uint16_t FRM_TYPE_VIEW = 0x5954;

// MySQL 数据库类型
constexpr uint8_t DB_TYPE_INNODB = 12;

// MySQL 类型定义
enum class MySQLFieldType : uint8_t {
    DECIMAL = 0,
    TINY = 1,
    SHORT = 2,
    LONG = 3,
    FLOAT = 4,
    DOUBLE = 5,
    NULL_TYPE = 6,
    TIMESTAMP = 7,
    LONGLONG = 8,
    INT24 = 9,
    DATE = 10,
    TIME = 11,
    DATETIME = 12,
    YEAR = 13,
    NEWDATE = 14,
    VARCHAR = 15,
    BIT = 16,
    JSON = 17,
    NEWDECIMAL = 246,
    ENUM = 247,
    SET = 248,
    TINY_BLOB = 249,
    MEDIUM_BLOB = 250,
    LONG_BLOB = 251,
    BLOB = 252,
    VAR_STRING = 253,
    STRING = 254,
    GEOMETRY = 255
};

// 字符集 ID 到名称映射 (部分)
static const std::map<uint8_t, std::string> CHARSET_MAP = {
    {1, "latin1"},
    {3, "latin1"},
    {4, "latin1"},
    {8, "latin1"},
    {33, "utf8"},
    {45, "utf8mb4"},
    {83, "utf8"},
    {94, "utf8mb4"},
    {224, "utf8"},
    {255, "binary"}
};

std::optional<catalog::TableSchema> FRMParser::parse(const std::string& frm_path) {
    std::ifstream file(frm_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[WARN] FRMParser: Failed to open file: " << frm_path << std::endl;
        return std::nullopt;
    }

    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        std::cerr << "[WARN] FRMParser: Failed to read file: " << frm_path << std::endl;
        return std::nullopt;
    }

    return parse(data.data(), size);
}

std::optional<catalog::TableSchema> FRMParser::parse(const uint8_t* data, size_t size) {
    if (!is_valid_frm(data, size)) {
        return std::nullopt;
    }

    FRMHeader header;
    if (!read_header(data, size, header)) {
        return std::nullopt;
    }

    catalog::TableSchema schema;

    // 计算记录偏移量
    uint32_t record_offset = header.io_size + header.tmp_key_length + header.rec_length;
    record_offset = ((record_offset / header.io_size) + 1) * header.io_size;

    // 解析列
    std::vector<ColumnDef> columns;
    size_t col_offset = record_offset + 258;  // 列定义开始位置
    if (!read_columns(data, col_offset, header, columns)) {
        std::cerr << "[WARN] FRMParser: Failed to read columns" << std::endl;
        return std::nullopt;
    }

    // 解析键
    std::vector<KeyDef> keys;
    size_t key_offset = header.io_size;
    if (!read_keys(data, key_offset, header, keys)) {
        std::cerr << "[WARN] FRMParser: Failed to read keys" << std::endl;
    }

    // 填充 TableSchema
    schema.table_name = "unknown_table";
    schema.row_format = header.row_type;
    schema.page_size = header.io_size > 0 ? header.io_size : 16384;

    // 添加列
    for (size_t i = 0; i < columns.size(); ++i) {
        const auto& col_def = columns[i];
        catalog::ColumnInfo col;
        col.name = col_def.name;
        col.mysql_type_id = col_def.field_type;
        col.column_length = col_def.field_length;
        col.charset_id = col_def.collation_id;
        col.collation_id = col_def.collation_id;
        // pack_flag bit 15 (0x8000) indicates nullable
        col.is_nullable = (col_def.pack_flag & 0x8000) != 0;
        // pack_flag bit 0: 0 = UNSIGNED, 1 = SIGNED (inverse logic)
        // is_signed = true means signed, is_signed = false means unsigned
        col.is_signed = (col_def.pack_flag & 1) == 1;
        col.comment = col_def.comment;

        // Extract DECIMAL precision/scale from field_length and pack_flag
        if (col_def.field_type == 246) {  // NEWDECIMAL
            col.numeric_precision = col_def.field_length - 2;
            col.numeric_scale = (col_def.pack_flag >> 8) & 31;
        }

        // Transfer ENUM/SET values
        if (col_def.field_type == 247) {  // ENUM
            col.enum_values = col_def.elements;
        } else if (col_def.field_type == 248) {  // SET
            col.set_values = col_def.elements;
        }

        schema.columns[i + 1] = col;  // ordinal position is 1-based
    }

    // 添加主键信息
    for (const auto& key : keys) {
        if (key.key_type == 1) {  // PRIMARY
            // 创建主键索引信息
            catalog::IndexInfo pk_index;
            pk_index.is_primary = true;
            pk_index.name = "PRIMARY";
            pk_index.is_unique = true;
            pk_index.algorithm = key.key_algorithm;
            pk_index.columns.reserve(key.parts.size());
            for (uint32_t col_idx : key.parts) {
                // Use actual column name if available
                if (col_idx < columns.size()) {
                    pk_index.columns.push_back(columns[col_idx].name);
                } else {
                    pk_index.columns.push_back("col" + std::to_string(col_idx));
                }
                pk_index.column_offsets.push_back(col_idx);
            }
            schema.indexes.push_back(pk_index);
            break;
        }
    }

    std::cout << "[INFO] FRMParser: Parsed " << columns.size() << " columns, "
              << keys.size() << " keys from .frm file" << std::endl;

    return schema;
}

bool FRMParser::is_valid_frm(const uint8_t* data, size_t size) {
    if (size < 64) {  // 最小的 FRM 头部大小
        return false;
    }

    uint16_t frm_type = read_uint16(data);
    return frm_type == FRM_TYPE_TABLE || frm_type == FRM_TYPE_VIEW;
}

bool FRMParser::read_header(const uint8_t* data, size_t size, FRMHeader& header) {
    if (size < 64) {
        return false;
    }

    // 读取 FRM 类型 (offset 0-1)
    header.frm_type = read_uint16(data);

    // 检查是否为支持的类型
    if (header.frm_type != FRM_TYPE_TABLE && header.frm_type != FRM_TYPE_VIEW) {
        std::cerr << "[WARN] FRMParser: Unsupported FRM type: 0x"
                  << std::hex << header.frm_type << std::dec << std::endl;
        return false;
    }

    // 读取版本 (offset 2)
    header.frm_version = data[2];
    if (header.frm_version < 9) {
        std::cerr << "[WARN] FRMParser: Unsupported FRM version: "
                  << static_cast<int>(header.frm_version) << std::endl;
        return false;
    }

    // 读取 legacy_db_type (offset 3)
    header.legacy_db_type = data[3];

    // Skip _0 (offset 4-5)
    // 读取 io_size (offset 6-7) - following ibd2sql's frm2sdi.py
    header.io_size = read_uint16(data + 6);
    if (header.io_size == 0) {
        header.io_size = 1024;  // 默认值
    }

    // Skip _1 (offset 8-9)
    // 读取 frm_length (offset 10-13)
    header.frm_length = read_uint32(data + 10);

    // 读取 tmp_key_length (offset 14-15)
    header.tmp_key_length = read_uint16(data + 14);

    // 读取 rec_length (offset 16-17)
    header.rec_length = read_uint16(data + 16);

    // 读取 create_info (offset 18-25)
    header.create_info_max_rows = read_uint32(data + 18);
    header.create_info_min_rows = read_uint32(data + 22);

    // 读取 key_info_length (offset 28-29)
    header.key_info_length = read_uint16(data + 28);

    // 读取 create_info_table_option (offset 30-31)
    header.create_info_table_option = read_uint16(data + 30);

    // 读取 avg_row_length (offset 34-37)
    header.avg_row_length = read_uint32(data + 34);

    // 读取 default_table_charset (offset 38)
    header.default_table_charset = data[38];

    // 读取 row_type (offset 40)
    header.row_type = data[40];

    // 读取 charset_high (offset 41)
    header.charset_high = data[41];

    // 读取 stats_sample_pages (offset 42-43)
    header.stats_sample_pages = read_uint16(data + 42);

    // 读取 stats_auto_recalc (offset 44)
    header.stats_auto_recalc = data[44];

    // 读取 key_length (offset 47-50)
    header.key_length = read_uint32(data + 47);

    // 读取 mysql_version_id (offset 51-54)
    header.mysql_version_id = read_uint32(data + 51);

    // 读取 extra_size (offset 55-58)
    header.extra_size = read_uint32(data + 55);

    // 读取 extra_rec_buf_length (offset 59-60)
    header.extra_rec_buf_length = read_uint16(data + 59);

    // 读取 default_part_db_type (offset 61)
    header.default_part_db_type = data[61];

    // 读取 key_block_size (offset 62-63)
    header.key_block_size = read_uint16(data + 62);

    return true;
}

bool FRMParser::read_columns(const uint8_t* data, size_t offset,
                            const FRMHeader& header,
                            std::vector<ColumnDef>& columns) {
    if (offset >= header.frm_length) {
        return false;
    }

    // FRM column section starts at record_offset + 258
    // Reference: ibd2sql frm2sdi.py _read_columns()
    size_t pos = offset;

    // Read column section header
    if (pos + 30 > header.frm_length) {
        return false;
    }

    uint16_t fields = read_uint16(data + pos);      // Number of fields
    pos += 2;
    uint16_t field_pos = read_uint16(data + pos);   // Position
    pos += 2;
    // Skip 6 bytes (_0)
    pos += 6;
    uint16_t n_length = read_uint16(data + pos);
    pos += 2;
    uint16_t interval_count = read_uint16(data + pos);
    pos += 2;
    uint16_t interval_parts = read_uint16(data + pos);
    pos += 2;
    uint16_t int_length = read_uint16(data + pos);
    pos += 2;
    // Skip 6 bytes (_1)
    pos += 6;
    uint16_t null_fields = read_uint16(data + pos);
    pos += 2;
    uint16_t comment_length = read_uint16(data + pos);
    pos += 2;
    uint16_t gcol_screen_length = read_uint16(data + pos);
    pos += 2;
    // Skip 5 bytes (_2)
    pos += 5;
    uint8_t fields_per_screen = data[pos];
    pos += 1;

    if (fields == 0 || fields > 1024) {
        std::cerr << "[WARN] FRMParser: Invalid field count: " << fields << std::endl;
        return false;
    }

    // Skip 42 bytes to reach field names section
    pos += 42;

    // Read field names
    std::vector<std::string> field_names;
    int screens_read = 1;
    int col_in_screen = 1;
    int fields_per_screen_n = 0;

    for (uint16_t i = 0; i < fields && pos < header.frm_length; i++) {
        // Handle screen transitions
        if (col_in_screen == fields_per_screen) {
            screens_read++;
            col_in_screen = 2;
            // Skip 8 bytes and terminator for screen transition
            pos += 8;
            // Read terminator byte (ibd2sql: _terminator = self.data.read(1))
            // Then skip any space terminators
            if (pos < header.frm_length) {
                uint8_t terminator = data[pos++];
                while (pos < header.frm_length && data[pos] == ' ') {
                    pos++;
                }
            }
            fields_per_screen_n++;
        } else {
            col_in_screen++;
        }

        if (pos + 3 > header.frm_length) break;

        // Read ordinal position (2 bytes)
        uint16_t ordinal_position = read_uint16(data + pos);
        pos += 2;

        // Read name size (1 byte)
        uint8_t name_size = data[pos];
        pos += 1;

        if (name_size == 0 || pos + name_size > header.frm_length) break;

        // Read column name (name_size bytes, last byte is usually null/space)
        std::string name(reinterpret_cast<const char*>(data + pos), name_size > 1 ? name_size - 1 : name_size);
        pos += name_size;

        field_names.push_back(name);
    }

    // Read field metadata
    for (uint16_t i = 0; i < fields && pos + 17 <= header.frm_length; i++) {
        ColumnDef col;
        col.name = field_names[i];

        // Skip 3 bytes (_0)
        pos += 3;

        // field_length (2 bytes)
        col.field_length = read_uint16(data + pos);
        pos += 2;

        // recpos (3 bytes)
        col.recpos = 0;
        for (int j = 0; j < 3; j++) {
            col.recpos |= (data[pos + j] << (j * 8));
        }
        pos += 3;

        // pack_flag (2 bytes)
        col.pack_flag = read_uint16(data + pos);
        pos += 2;

        // unireg_type (1 byte)
        col.unireg_type = data[pos];
        pos += 1;

        // charset_low (1 byte)
        col.charset_low = data[pos];
        pos += 1;

        // interval_nr (1 byte)
        col.interval_nr = data[pos];
        pos += 1;

        // field_type (1 byte)
        col.field_type = data[pos];
        pos += 1;

        // collation_id (1 byte)
        col.collation_id = data[pos];
        pos += 1;

        // comment_length (2 bytes)
        col.comment_length = read_uint16(data + pos);
        pos += 2;

        col.comment = "";
        columns.push_back(col);
    }

    // Skip name section (NAMESIZE bytes) + 2 bytes (based on ibd2sql)
    // Read column comments
    size_t total_name_size = 0;
    for (const auto& fn : field_names) {
        total_name_size += fn.size() + 1;  // name + null terminator
    }

    // Safety check: don't skip past file end
    if (pos + total_name_size + 2 > header.frm_length) {
        std::cout << "[INFO] FRMParser: Found " << columns.size() << " columns (skip comments)" << std::endl;
        return !columns.empty();
    }
    pos += total_name_size + 2;

    // Read column comments
    for (uint16_t i = 0; i < fields && pos < header.frm_length; i++) {
        if (columns[i].comment_length > 0 && columns[i].comment_length < 65536 &&
            pos + columns[i].comment_length <= header.frm_length) {
            columns[i].comment = std::string(reinterpret_cast<const char*>(data + pos), columns[i].comment_length);
            pos += columns[i].comment_length;
        }
    }

    // Read ENUM/SET values (interval section)
    // Reference: ibd2sql frm2sdi.py lines 344-358
    for (uint16_t i = 0; i < columns.size() && pos < header.frm_length; i++) {
        if (columns[i].field_type == 247 || columns[i].field_type == 248) {  // ENUM or SET
            std::string element;
            std::vector<std::string> elements;
            uint8_t terminator = data[pos++];

            while (pos < header.frm_length) {
                uint8_t byte = data[pos++];
                if (byte == 0x00) {
                    break;  // End of interval list
                } else if (byte == terminator) {
                    if (!element.empty()) {
                        elements.push_back(element);
                        element.clear();
                    }
                } else {
                    element += static_cast<char>(byte);
                }
            }
            columns[i].elements = elements;
        }
    }

    std::cout << "[INFO] FRMParser: Found " << columns.size() << " columns" << std::endl;
    return !columns.empty();
}

bool FRMParser::read_keys(const uint8_t* data, size_t offset,
                         const FRMHeader& header,
                         std::vector<KeyDef>& keys) {
    if (header.key_info_length == 0 || offset >= header.frm_length) {
        return true;  // 没有键信息不算错误
    }

    // 读取键数量
    uint8_t key_count = data[offset];
    if (key_count & 0x80) {
        key_count = (key_count & 0x7f) | ((data[offset + 1]) << 7);
    }

    size_t key_info_offset = offset + 4;  // 跳过键计数

    for (uint8_t i = 0; i < key_count && key_info_offset < header.frm_length; ++i) {
        KeyDef key;

        key.key_type = data[key_info_offset];
        key.key_algorithm = data[key_info_offset + 1];
        key.flags = read_uint16(data + key_info_offset + 2);

        key_info_offset += 4;

        // 读取键名
        size_t name_start = key_info_offset;
        while (key_info_offset < header.frm_length && data[key_info_offset] != 0) {
            key_info_offset++;
        }
        if (key_info_offset > name_start) {
            key.name = std::string(reinterpret_cast<const char*>(data + name_start),
                                  key_info_offset - name_start);
        }
        key_info_offset++;  // 跳过 null 终结符

        // 读取键部分
        uint8_t key_parts = data[key_info_offset++];
        for (uint8_t p = 0; p < key_parts && key_info_offset < header.frm_length; ++p) {
            uint32_t col_idx = read_uint32(data + key_info_offset);
            key.parts.push_back(col_idx);
            key_info_offset += 4;
        }

        keys.push_back(key);
    }

    return true;
}

bool FRMParser::read_default_values(const uint8_t* data, size_t offset,
                                   const FRMHeader& header,
                                   std::vector<std::string>& defaults) {
    // 默认值存储在 FRM 文件的特定区域
    // 简化实现
    return true;
}

std::string FRMParser::get_mysql_type_name(uint8_t type_id, uint32_t length) {
    switch (static_cast<MySQLFieldType>(type_id)) {
        case MySQLFieldType::DECIMAL:
            return "decimal";
        case MySQLFieldType::TINY:
            return "tinyint";
        case MySQLFieldType::SHORT:
            return "smallint";
        case MySQLFieldType::LONG:
            return "int";
        case MySQLFieldType::FLOAT:
            return "float";
        case MySQLFieldType::DOUBLE:
            return "double";
        case MySQLFieldType::TIMESTAMP:
            return "timestamp";
        case MySQLFieldType::LONGLONG:
            return "bigint";
        case MySQLFieldType::DATE:
            return "date";
        case MySQLFieldType::TIME:
            return "time";
        case MySQLFieldType::DATETIME:
            return "datetime";
        case MySQLFieldType::YEAR:
            return "year";
        case MySQLFieldType::VARCHAR:
            return length > 255 ? "varchar" : "varchar";
        case MySQLFieldType::BIT:
            return "bit";
        case MySQLFieldType::JSON:
            return "json";
        case MySQLFieldType::NEWDECIMAL:
            return "decimal";
        case MySQLFieldType::ENUM:
            return "enum";
        case MySQLFieldType::SET:
            return "set";
        case MySQLFieldType::BLOB:
        case MySQLFieldType::TINY_BLOB:
        case MySQLFieldType::MEDIUM_BLOB:
        case MySQLFieldType::LONG_BLOB:
            return "blob";
        case MySQLFieldType::GEOMETRY:
            return "geometry";
        default:
            return "unknown";
    }
}

std::string FRMParser::charset_id_to_name(uint8_t charset_id) {
    auto it = CHARSET_MAP.find(charset_id);
    if (it != CHARSET_MAP.end()) {
        return it->second;
    }
    return "latin1";  // 默认字符集
}

uint16_t FRMParser::read_uint16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t FRMParser::read_uint32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t FRMParser::read_uint64(const uint8_t* data) {
    return static_cast<uint64_t>(read_uint32(data)) |
           (static_cast<uint64_t>(read_uint32(data + 4)) << 32);
}

} // namespace innodb
} // namespace dbrecovery
