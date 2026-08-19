#ifndef DBRECOVERY_INNODB_RECORD_PARSER_HPP
#define DBRECOVERY_INNODB_RECORD_PARSER_HPP

/**
 * @file record_parser.hpp
 * @brief MySQL 5.6-9.0 全版本兼容记录解析层
 *
 * 支持的行格式：
 * - REDUNDANT (MySQL 4.1-5.0)
 * - COMPACT (MySQL 5.0+)
 * - DYNAMIC (MySQL 5.7+)
 * - COMPRESSED (MySQL 5.7+)
 */

#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include <variant>
#include <map>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/innodb/roll_ptr_decoder.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== 字段类型定义 ====================
/**
 * @brief ColumnType: 字段类型枚举
 */
enum class ColumnType : uint8_t {
    NULL_TYPE = 0,
    TINY_INT = 1,
    SHORT_INT = 2,
    LONG_INT = 3,
    FLOAT = 4,
    DOUBLE = 5,
    LONGLONG = 8,
    INT24 = 9,
    DATE = 10,
    TIME = 11,
    DATETIME = 12,
    YEAR = 13,
    VARCHAR = 15,
    VAR_STRING = 253,
    BIT = 16,
    TIMESTAMP2 = 17,
    DATETIME2 = 18,
    TIME2 = 19,
    JSON = 245,
    TINY_BLOB = 249,
    MEDIUM_BLOB = 250,
    LONG_BLOB = 251,
    BLOB = 252,
    CHAR = 254,
    TEXT = 252,
};

/**
 * @brief ColumnInfo: 字段元信息
 */
struct ColumnInfo {
    uint16_t position;          // 字段在表中的位置
    ColumnType type;            // 字段类型
    uint32_t length;            // 字段长度（字节）
    bool is_nullable;           // 是否允许NULL
    bool is_unsigned;           // 是否无符号（仅数值类型）
    bool is_binary;             // 是否二进制字符串
    uint32_t char_set;          // 字符集ID
    uint16_t precision;         // 精度（DECIMAL类型）
    uint16_t scale;             // 小数位数
    
    std::string name;           // 字段名
    std::string default_value;  // 默认值
    
    std::string to_string() const {
        std::string result = name + " " + type_to_string(type);
        if (is_unsigned && is_numeric_type(type)) {
            result += " UNSIGNED";
        }
        if (!is_nullable) {
            result += " NOT NULL";
        }
        return result;
    }
    
    static std::string type_to_string(ColumnType type) {
        switch (type) {
            case ColumnType::NULL_TYPE: return "NULL";
            case ColumnType::TINY_INT: return "TINYINT";
            case ColumnType::SHORT_INT: return "SMALLINT";
            case ColumnType::LONG_INT: return "INT";
            case ColumnType::FLOAT: return "FLOAT";
            case ColumnType::DOUBLE: return "DOUBLE";
            case ColumnType::LONGLONG: return "BIGINT";
            case ColumnType::INT24: return "MEDIUMINT";
            case ColumnType::DATE: return "DATE";
            case ColumnType::TIME: return "TIME";
            case ColumnType::DATETIME: return "DATETIME";
            case ColumnType::YEAR: return "YEAR";
            case ColumnType::VARCHAR: return "VARCHAR";
            case ColumnType::VAR_STRING: return "VAR_STRING";
            case ColumnType::BIT: return "BIT";
            case ColumnType::TIMESTAMP2: return "TIMESTAMP";
            case ColumnType::DATETIME2: return "DATETIME2";
            case ColumnType::TIME2: return "TIME2";
            case ColumnType::JSON: return "JSON";
            case ColumnType::TINY_BLOB: return "TINYBLOB";
            case ColumnType::MEDIUM_BLOB: return "MEDIUMBLOB";
            case ColumnType::LONG_BLOB: return "LONGBLOB";
            case ColumnType::BLOB: return "BLOB";
            case ColumnType::CHAR: return "CHAR";
            // TEXT 和 BLOB 类型相同，使用 BLOB 的返回值
            default: return "UNKNOWN";
        }
    }
    
    static bool is_numeric_type(ColumnType type) {
        return type == ColumnType::TINY_INT ||
               type == ColumnType::SHORT_INT ||
               type == ColumnType::LONG_INT ||
               type == ColumnType::FLOAT ||
               type == ColumnType::DOUBLE ||
               type == ColumnType::LONGLONG ||
               type == ColumnType::INT24;
    }
};

/**
 * @brief ColumnValue: 字段值（variant类型）
 */
using ColumnValue = std::variant<
    std::monostate,           // NULL
    bool,                     // BOOL/TINYINT(1)
    int8_t,                   // TINYINT
    uint8_t,                  // TINYINT UNSIGNED
    int16_t,                  // SMALLINT
    uint16_t,                 // SMALLINT UNSIGNED
    int32_t,                  // INT
    uint32_t,                 // INT UNSIGNED
    int64_t,                  // BIGINT
    uint64_t,                 // BIGINT UNSIGNED
    float,                    // FLOAT
    double,                   // DOUBLE
    std::vector<uint8_t>,     // BINARY/VARBINARY/BLOB
    std::string               // VARCHAR/CHAR/TEXT
>;

/**
 * @brief ColumnData: 字段数据
 */
struct ColumnData {
    ColumnInfo info;
    ColumnValue value;
    
    std::string to_string() const {
        if (std::holds_alternative<std::monostate>(value)) {
            return "NULL";
        }
        if (std::holds_alternative<bool>(value)) {
            return std::get<bool>(value) ? "1" : "0";
        }
        if (std::holds_alternative<int8_t>(value)) {
            return std::to_string(std::get<int8_t>(value));
        }
        if (std::holds_alternative<uint8_t>(value)) {
            return std::to_string(std::get<uint8_t>(value));
        }
        if (std::holds_alternative<int16_t>(value)) {
            return std::to_string(std::get<int16_t>(value));
        }
        if (std::holds_alternative<uint16_t>(value)) {
            return std::to_string(std::get<uint16_t>(value));
        }
        if (std::holds_alternative<int32_t>(value)) {
            return std::to_string(std::get<int32_t>(value));
        }
        if (std::holds_alternative<uint32_t>(value)) {
            return std::to_string(std::get<uint32_t>(value));
        }
        if (std::holds_alternative<int64_t>(value)) {
            return std::to_string(std::get<int64_t>(value));
        }
        if (std::holds_alternative<uint64_t>(value)) {
            return std::to_string(std::get<uint64_t>(value));
        }
        if (std::holds_alternative<float>(value)) {
            return std::to_string(std::get<float>(value));
        }
        if (std::holds_alternative<double>(value)) {
            return std::to_string(std::get<double>(value));
        }
        if (std::holds_alternative<std::vector<uint8_t>>(value)) {
            auto& vec = std::get<std::vector<uint8_t>>(value);
            std::string result = "0x";
            for (uint8_t b : vec) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02x", b);
                result += hex;
            }
            return result;
        }
        if (std::holds_alternative<std::string>(value)) {
            return std::get<std::string>(value);
        }
        return "UNKNOWN";
    }
};

// ==================== 记录头结构 ====================
/**
 * @brief RecordHeaderBase: 记录头基类
 */
struct RecordHeaderBase {
    uint16_t offset;            // 记录在页中的偏移
    uint16_t next_offset;       // 下一条记录的偏移
    uint8_t info_bits;          // 信息位
    uint16_t heap_no;           // 堆编号
    
    // 状态标志
    bool is_deleted;            // 已删除标志
    bool is_min_rec;            // 最小记录标志
    bool is_infimum;            // INFIMUM系统记录
    bool is_supremum;           // SUPREMUM系统记录
    bool is_node_ptr;           // 节点指针记录（非叶子节点）
    
    // 行格式信息
    RowFormat row_format;
    
    RecordHeaderBase()
        : offset(0), next_offset(0), info_bits(0), heap_no(0),
          is_deleted(false), is_min_rec(false), is_infimum(false),
          is_supremum(false), is_node_ptr(false), row_format(RowFormat::COMPACT) {}
};

/**
 * @brief RedundantRecordHeader: Redundant格式记录头
 *
 * Layout (6 + 变长字段长度列表):
 * - 1 byte: n_owned (4 bits) + status (2 bits) + flag (1 bit) + next_rec_high (1 bit)
 * - 2 bytes: next_record_offset (14 bits)
 * - 3 bytes: heap_no (14 bits)
 * - 2 bytes: record_length (14 bits)
 * - 变长字段长度列表 (每个字段1-2字节)
 * - 可空字段长度列表 (每个字段1字节，如果允许NULL)
 */
struct RedundantRecordHeader : public RecordHeaderBase {
    uint16_t record_length;     // 记录长度
    uint8_t n_owned;            // 本记录拥有的记录数
    uint8_t status;             // 记录状态
    
    // 变长字段长度
    std::vector<uint8_t> var_len;
    
    // 可空字段长度
    std::vector<uint16_t> null_len;
    
    RedundantRecordHeader() : record_length(0), n_owned(0), status(0) {}
};

/**
 * @brief CompactRecordHeader: Compact格式记录头
 *
 * Layout (5 + 变长字段长度列表):
 * - 1 byte: n_owned (4 bits) + status (3 bits)
 * - 2 bytes: next_record_offset (14 bits)
 * - 3 bytes: heap_no (14 bits)
 * - 1 byte: info_bits + n_fields_high (5 bits)
 * - 1 byte: n_fields_low (4 bits) + extra_bytes (4 bits)
 * - 变长字段长度列表 (每个字段1-2字节)
 * - 可空字段位图 (每8个字段1字节)
 */
struct CompactRecordHeader : public RecordHeaderBase {
    uint16_t n_fields;          // 字段数量
    
    // 变长字段长度
    std::vector<uint8_t> var_len;
    
    // 可空字段位图
    std::vector<uint8_t> null_bitmap;
    
    // 系统列偏移
    uint16_t trx_id_offset;
    uint16_t roll_ptr_offset;
    uint16_t row_id_offset;
    
    CompactRecordHeader()
        : n_fields(0), trx_id_offset(0), roll_ptr_offset(0), row_id_offset(0) {}
};

/**
 * @brief DynamicRecordHeader: Dynamic格式记录头（Instant DDL支持）
 *
 * MySQL 8.0+ 特有，支持 Instant DDL
 */
struct DynamicRecordHeader : public CompactRecordHeader {
    uint8_t metadata_version;   // 元数据版本
    uint8_t instant_flags;      // Instant DDL标志
    
    // 新增列值
    std::vector<uint8_t> added_column_val;
    
    // 默认值
    std::vector<uint8_t> default_val;
    
    // Instant DDL元数据
    uint16_t n_added_cols;      // 新增列数量
    uint16_t n_default_cols;    // 默认值列数量
    
    DynamicRecordHeader()
        : metadata_version(0), instant_flags(0),
          n_added_cols(0), n_default_cols(0) {}
};

// ==================== 记录结构 ====================
/**
 * @brief RecordData: 解析后的记录数据
 */
struct RecordData {
    RecordHeaderBase header;
    
    // 字段数据
    std::vector<ColumnData> columns;
    
    // 系统列
    std::optional<uint64_t> trx_id;
    std::optional<uint64_t> roll_ptr;
    std::optional<uint64_t> row_id;
    
    // BLOB指针
    std::vector<uint64_t> blob_pointers;
    
    // 页内偏移
    uint16_t offset;
    uint16_t length;
    
    RecordData() : offset(0), length(0) {}
};

// ==================== 记录解析器接口 ====================
/**
 * @brief IRecordParser: 记录解析器接口
 */
class IRecordParser {
public:
    virtual ~IRecordParser() = default;
    
    // 解析记录头
    virtual std::optional<RecordHeaderBase> parse_header(
        const uint8_t* data, size_t size, size_t offset) = 0;
    
    // 解析记录数据
    virtual std::vector<ColumnData> parse_columns(
        const uint8_t* data, size_t size, size_t offset,
        const RecordHeaderBase& header, const std::vector<ColumnInfo>& columns) = 0;
    
    // 获取记录长度
    virtual uint16_t get_record_length(
        const uint8_t* data, size_t size, size_t offset) = 0;
    
    // 检查是否是系统记录
    virtual bool is_system_record(const uint8_t* data, size_t size, size_t offset) = 0;
};

// ==================== Redundant格式解析器 ====================
class RedundantRecordParser : public IRecordParser {
public:
    std::optional<RecordHeaderBase> parse_header(
        const uint8_t* data, size_t size, size_t offset) override;
    
    std::vector<ColumnData> parse_columns(
        const uint8_t* data, size_t size, size_t offset,
        const RecordHeaderBase& header, const std::vector<ColumnInfo>& columns) override;
    
    uint16_t get_record_length(
        const uint8_t* data, size_t size, size_t offset) override;
    
    bool is_system_record(const uint8_t* data, size_t size, size_t offset) override;
    
private:
    uint16_t read_var_len(const uint8_t* data, size_t offset, size_t& read_size) const;
};

// ==================== Compact格式解析器 ====================
class CompactRecordParser : public IRecordParser {
public:
    std::optional<RecordHeaderBase> parse_header(
        const uint8_t* data, size_t size, size_t offset) override;
    
    std::vector<ColumnData> parse_columns(
        const uint8_t* data, size_t size, size_t offset,
        const RecordHeaderBase& header, const std::vector<ColumnInfo>& columns) override;
    
    uint16_t get_record_length(
        const uint8_t* data, size_t size, size_t offset) override;
    
    bool is_system_record(const uint8_t* data, size_t size, size_t offset) override;
    
private:
    uint16_t read_var_len(const uint8_t* data, size_t offset, size_t& read_size) const;
    size_t get_null_bytes(size_t n_fields) const;
};

// ==================== Dynamic格式解析器 ====================
class DynamicRecordParser : public CompactRecordParser {
public:
    std::optional<RecordHeaderBase> parse_header(
        const uint8_t* data, size_t size, size_t offset) override;
    
    std::vector<ColumnData> parse_columns(
        const uint8_t* data, size_t size, size_t offset,
        const RecordHeaderBase& header, const std::vector<ColumnInfo>& columns) override;
    
    bool is_system_record(const uint8_t* data, size_t size, size_t offset) override;
    
private:
    bool parse_instant_ddl_metadata(
        const uint8_t* data, size_t size, size_t offset,
        DynamicRecordHeader& header);
};

// ==================== 记录解析器工厂 ====================
class RecordParserFactory {
public:
    // 创建记录解析器
    static std::unique_ptr<IRecordParser> create_parser(RowFormat format);
    
    // 从记录头识别行格式
    static RowFormat detect_row_format(const uint8_t* data, size_t size, size_t offset);
    
    // 从页头识别行格式
    static RowFormat detect_row_format_from_page(const uint8_t* page_data, size_t page_size);
};

// ==================== 记录扫描器 ====================
class RecordScanner {
public:
    explicit RecordScanner(const std::vector<ColumnInfo>& columns);
    
    // 扫描页中的所有记录
    std::vector<RecordData> scan_page(
        const uint8_t* page_data, size_t page_size,
        RowFormat format = RowFormat::COMPACT);
    
    // 扫描指定范围的记录
    std::vector<RecordData> scan_range(
        const uint8_t* page_data, size_t page_size,
        uint16_t start_offset, uint16_t end_offset,
        RowFormat format = RowFormat::COMPACT);
    
private:
    std::vector<ColumnInfo> columns_;
};

// ==================== 记录重建器 ====================
class RecordReconstructor {
public:
    // 从损坏的记录中重建数据
    static std::vector<ColumnData> reconstruct_from_partial(
        const uint8_t* data, size_t size,
        const std::vector<ColumnInfo>& columns,
        size_t valid_bytes);
    
    // 从Undo Log记录重建
    static RecordData reconstruct_from_undo(
        const uint8_t* undo_data, size_t undo_size,
        const std::vector<ColumnInfo>& columns,
        UndoFormatProfile profile = UndoFormatProfile::UNRESOLVED);
};

// ==================== 工具函数 ====================
/**
 * @brief 从记录头读取info_bits
 */
inline uint8_t read_info_bits(const uint8_t* data, size_t offset, RowFormat format) {
    if (format == RowFormat::REDUNDANT) {
        return data[offset];
    } else {
        return data[offset];
    }
}

/**
 * @brief 从记录头读取next_record_offset
 */
inline uint16_t read_next_record_offset(const uint8_t* data, size_t offset, RowFormat format) {
    if (format == RowFormat::REDUNDANT) {
        // Redundant: 1 byte high + 2 bytes low = 14 bits
        uint8_t high = data[offset] & 0x01;
        uint16_t low = read_le<uint16_t>(data, offset + 1);
        return static_cast<uint16_t>((high << 14) | (low & 0x3FFF));
    } else {
        // Compact: 2 bytes = 14 bits
        return read_le<uint16_t>(data, offset + 1) & 0x3FFF;
    }
}

/**
 * @brief 从记录头读取heap_no
 */
inline uint16_t read_heap_no(const uint8_t* data, size_t offset, RowFormat format) {
    if (format == RowFormat::REDUNDANT) {
        // Redundant: 3 bytes = 14 bits
        return read_le<uint16_t>(data, offset + 3) & 0x3FFF;
    } else {
        // Compact: 3 bytes = 14 bits
        return read_le<uint16_t>(data, offset + 3) & 0x3FFF;
    }
}

/**
 * @brief 从记录头读取record_length
 */
inline uint16_t read_record_length(const uint8_t* data, size_t offset, RowFormat format) {
    if (format == RowFormat::REDUNDANT) {
        // Redundant: 2 bytes at offset + 5
        return read_le<uint16_t>(data, offset + 5) & 0x3FFF;
    } else {
        // Compact: calculated from next_record_offset
        return 0; // Will be calculated
    }
}

/**
 * @brief 检查是否是INFIMUM记录
 */
inline bool is_infimum_record(const uint8_t* data, size_t offset, RowFormat format) {
    uint8_t info_bits = read_info_bits(data, offset, format);
    uint8_t status = info_bits & 0x07;
    return status == REC_STATUS_INFIMUM;
}

/**
 * @brief 检查是否是SUPREMUM记录
 */
inline bool is_supremum_record(const uint8_t* data, size_t offset, RowFormat format) {
    uint8_t info_bits = read_info_bits(data, offset, format);
    uint8_t status = info_bits & 0x07;
    return status == REC_STATUS_SUPREMUM;
}

/**
 * @brief 检查是否是节点指针记录
 */
inline bool is_node_ptr_record(const uint8_t* data, size_t offset, RowFormat format) {
    uint8_t info_bits = read_info_bits(data, offset, format);
    uint8_t status = info_bits & 0x07;
    return status == REC_STATUS_NODE_PTR;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_RECORD_PARSER_HPP
