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
        col.is_nullable = false;  // 需要根据 pack_flag 判断
        col.comment = col_def.comment;

        // 根据 field_type 设置长度
        switch (static_cast<MySQLFieldType>(col_def.field_type)) {
            case MySQLFieldType::TINY:
                col.column_length = 1;
                break;
            case MySQLFieldType::SHORT:
                col.column_length = 2;
                break;
            case MySQLFieldType::LONG:
            case MySQLFieldType::TIMESTAMP:
                col.column_length = 4;
                break;
            case MySQLFieldType::LONGLONG:
            case MySQLFieldType::DOUBLE:
                col.column_length = 8;
                break;
            case MySQLFieldType::FLOAT:
                col.column_length = 4;
                break;
            case MySQLFieldType::DATETIME:
            case MySQLFieldType::DATE:
                col.column_length = 8;
                break;
            case MySQLFieldType::YEAR:
                col.column_length = 1;
                break;
            case MySQLFieldType::BLOB:
            case MySQLFieldType::TINY_BLOB:
            case MySQLFieldType::MEDIUM_BLOB:
            case MySQLFieldType::LONG_BLOB:
                col.column_length = 65535;  // 最大长度
                break;
            default:
                break;
        }

        schema.columns[i] = col;
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
                pk_index.columns.push_back("col" + std::to_string(col_idx));
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

    // 读取 FRM 类型 (offset 0)
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
    // 不再强制检查，因为 MySQL 5.6 可能不是 12

    // 读取 io_size (offset 4-5)
    header.io_size = read_uint16(data + 4);
    if (header.io_size == 0) {
        header.io_size = 1024;  // 默认值
    }

    // 读取 frm_length (offset 8-11)
    header.frm_length = read_uint32(data + 8);

    // 读取 tmp_key_length (offset 12-13)
    header.tmp_key_length = read_uint16(data + 12);

    // 读取 rec_length (offset 14-15)
    header.rec_length = read_uint16(data + 14);

    // 读取 create_info (offset 16-27)
    header.create_info_max_rows = read_uint32(data + 16);
    header.create_info_min_rows = read_uint32(data + 20);

    // 读取 key_info_length (offset 26-27)
    header.key_info_length = read_uint16(data + 26);

    // 读取 create_info_table_option (offset 28-29)
    header.create_info_table_option = read_uint16(data + 28);

    // 读取 avg_row_length (offset 32-35)
    header.avg_row_length = read_uint32(data + 32);

    // 读取 default_table_charset (offset 36)
    header.default_table_charset = data[36];

    // 读取 row_type (offset 38)
    header.row_type = data[38];

    // 读取 charset_high (offset 39)
    header.charset_high = data[39];

    // 读取 stats_sample_pages (offset 40-41)
    header.stats_sample_pages = read_uint16(data + 40);

    // 读取 stats_auto_recalc (offset 42)
    header.stats_auto_recalc = data[42];

    // 读取 key_length (offset 45-48)
    header.key_length = read_uint32(data + 45);

    // 读取 mysql_version_id (offset 49-52)
    header.mysql_version_id = read_uint32(data + 49);

    // 读取 extra_size (offset 53-56)
    header.extra_size = read_uint32(data + 53);

    // 读取 extra_rec_buf_length (offset 57-58)
    header.extra_rec_buf_length = read_uint16(data + 57);

    // 读取 default_part_db_type (offset 59)
    header.default_part_db_type = data[59];

    // 读取 key_block_size (offset 60-61)
    header.key_block_size = read_uint16(data + 60);

    return true;
}

bool FRMParser::read_columns(const uint8_t* data, size_t offset,
                            const FRMHeader& header,
                            std::vector<ColumnDef>& columns) {
    if (offset >= header.frm_length) {
        return false;
    }

    // FRM 文件中列的存储格式是复杂的
    // 这里使用简化的方法：扫描可打印字符序列作为列名

    size_t scan_start = offset;
    size_t max_scan = header.frm_length - offset;

    // 简单实现：查找以字母开头的标识符序列
    while (scan_start < offset + max_scan - 10) {
        // 跳过不可打印字符
        while (scan_start < offset + max_scan &&
               (data[scan_start] < 'a' || data[scan_start] > 'z') &&
               (data[scan_start] < 'A' || data[scan_start] > 'Z') &&
               data[scan_start] != '_') {
            scan_start++;
        }

        if (scan_start >= offset + max_scan - 10) break;

        // 读取标识符
        size_t name_start = scan_start;
        while (scan_start < offset + max_scan &&
               (isalnum(data[scan_start]) || data[scan_start] == '_')) {
            scan_start++;
        }

        if (scan_start > name_start + 1 && scan_start - name_start <= 64) {
            ColumnDef col;
            col.name = std::string(reinterpret_cast<const char*>(data + name_start),
                                   scan_start - name_start);

            // 简化：假设每列有固定大小的元数据
            // 实际实现需要更复杂的解析
            col.field_type = data[scan_start] & 0xFF;
            col.field_length = 255;
            col.collation_id = header.default_table_charset;

            columns.push_back(col);

            if (columns.size() >= 256) break;  // 限制最大列数
        }
    }

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
