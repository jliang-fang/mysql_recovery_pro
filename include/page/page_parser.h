#ifndef PAGE_PAGE_PARSER_H
#define PAGE_PAGE_PARSER_H

#include <cstdint>
#include <memory>
#include <variant>
#include <unordered_map>
#include <optional>
#include <vector>
#include <string>

#include "dbrecovery/innodb/page_defs.hpp"

namespace mysql_recovery {

// ==================== 基础类型定义 ====================
// MySQL versions supported: 5.6, 5.7, 8.0, 8.4
enum class MySQLVersion {
    UNKNOWN = 0,
    MYSQL_56 = 56,      // MySQL 5.6
    MYSQL_57 = 57,      // MySQL 5.7
    MYSQL_80 = 80,      // MySQL 8.0
    MYSQL_84 = 84,      // MySQL 8.4
};

enum class RowFormat {
    UNKNOWN = 0,
    REDUNDANT = 1,
    COMPACT = 2,
    DYNAMIC = 3,
    COMPRESSED = 4,
    PAGE_COMPRESSED = 5,
};

enum class PageType {
    UNKNOWN = 0,
    INDEX = 9969,       // 0x26F1 - B+Tree index page (MySQL 8.0)
    FIL_PAGE_TYPE = 9969, // Alias for INDEX
    SYSTEM = 9984,      // 0x2718 - System page
    UNDO_LOG = 9970,    // 0x26F2 - Undo log page
    ALLOCATED = 0,      // Freshly allocated page
    FSP_HDR = 0x45BF,   // File space header page
    XDES = 0x45BE,      // Extent descriptor page
    SDI = 9971,         // 0x26F3 - SDI Index page (MySQL 8.0+)
    BLOB = 9972,        // 0x26F4 - BLOB page
    COMPRESSED = 9973,  // 0x26F5 - Compressed page
};

inline PageType page_type_from_fil(uint16_t raw_fil_type) {
    switch (raw_fil_type) {
        case dbrecovery::innodb::FIL_PAGE_INDEX:
            return PageType::INDEX;
        case dbrecovery::innodb::FIL_PAGE_UNDO_LOG:
            return PageType::UNDO_LOG;
        case dbrecovery::innodb::FIL_PAGE_SDI:
            return PageType::SDI;
        case dbrecovery::innodb::FIL_PAGE_SDI_ZIP:
            return PageType::BLOB;
        case dbrecovery::innodb::FIL_PAGE_COMPRESSED:
        case dbrecovery::innodb::FIL_PAGE_COMPRESSED_AND_ENCRYPTED:
            return PageType::COMPRESSED;
        case dbrecovery::innodb::FIL_PAGE_TYPE_ALLOCATED:
            return PageType::ALLOCATED;
        default:
            return PageType::UNKNOWN;
    }
}

// ==================== Page Header 抽象基类 ====================
/**
 * @brief Page Header 抽象基类
 * @details 统一不同 MySQL 版本的 Page Header 接口
 */
class PageHeaderBase {
public:
    virtual ~PageHeaderBase() = default;
    
    // 核心属性
    virtual uint32_t get_prev_page() const = 0;
    virtual uint32_t get_next_page() const = 0;
    virtual uint64_t get_lsn() const = 0;
    virtual PageType get_page_type() const = 0;
    virtual uint16_t get_level() const = 0;
    virtual uint16_t get_n_recs() const = 0;
    virtual uint16_t get_free_ptr() const = 0;
    virtual uint16_t get_free_size() const = 0;
    virtual uint16_t get_n_dir_slots() const = 0;
    virtual uint16_t get_data_start() const = 0;

    // 校验相关
    virtual bool verify_checksum() const = 0;
    virtual uint32_t calculate_checksum() const = 0;

    // 版本信息
    virtual MySQLVersion get_version() const = 0;
    virtual RowFormat get_row_format() const = 0;

    // 辅助信息
    virtual uint32_t get_space_id() const = 0;
    virtual uint32_t get_page_offset() const = 0;
    virtual uint16_t get_max_trx_id() const = 0;

    // 加密支持 (MySQL 8.4+)
    virtual bool is_encrypted() const = 0;
    virtual uint32_t get_encryption_key_version() const = 0;

    // 压缩支持
    virtual bool is_compressed() const = 0;
    virtual uint16_t get_zip_size() const = 0;

    // 字符串化（调试用）
    virtual std::string to_string() const = 0;
};

// ==================== 行格式解析器 ====================
/**
 * @brief 行格式解析器基类
 * @details 支持 Redundant/Compact/Dynamic 等行格式
 */
class RowParser {
public:
    struct RecordHeader {
        uint16_t heap_no : 14;
        uint16_t n_owned : 4;
        uint16_t rec_type : 3;
        uint16_t deleted_flag : 1;
        uint16_t min_rec_flag : 1;
        uint16_t record_length : 13;
        uint32_t next_record_offset : 15;
    };
    
    struct ColumnInfo {
        uint16_t offset;
        uint16_t length;
        bool is_null;
        bool is_variable;
        uint32_t col_type;
        std::string col_name;
    };
    
    virtual ~RowParser() = default;
    
    // 解析记录头
    virtual RecordHeader parse_record_header(const uint8_t* data, RowFormat format) = 0;
    
    // 解析字段信息
    virtual std::vector<ColumnInfo> parse_columns(
        const uint8_t* data,
        const std::vector<uint8_t>& col_meta,
        RowFormat format
    ) = 0;
    
    // 生成建表语句 (Heuristic 模式)
    virtual std::string generate_create_statement(
        const std::vector<ColumnInfo>& cols,
        const std::unordered_map<uint32_t, std::string>& col_names
    ) = 0;
    
    // 从记录数据中提取字段值
    virtual std::vector<std::optional<std::string>> extract_column_values(
        const uint8_t* record_data,
        size_t record_len,
        const std::vector<ColumnInfo>& cols
    ) = 0;
};

// ==================== SDI (Serialized Dictionary Info) 解析器 ====================
/**
 * @brief SDI 解析器
 * @details MySQL 8.0+ 的序列化字典信息
 */
class SDIParser {
public:
    struct SDIEntry {
        std::string table_name;
        std::string database_name;
        std::string create_ddl;
        uint64_t table_id;
        uint32_t space_id;
        std::vector<SDIEntry> child_entries;
    };
    
    /**
     * @brief 从 SDI 页解析条目
     */
    static std::vector<SDIEntry> parse_sdi_page(const uint8_t* page_data, size_t page_size);
    
    /**
     * @brief 根据表名查找 SDI 条目
     */
    static std::optional<SDIEntry> find_table_by_name(
        const std::vector<SDIEntry>& sdi_entries,
        const std::string& table_name
    );
    
    /**
     * @brief 检查是否为 SDI 页
     */
    static bool is_sdi_page(const uint8_t* page_data, size_t page_size);
};

// ==================== 自动探测工厂 ====================
/**
 * @brief Page 自动探测工厂
 * @details 根据 Page 数据自动创建对应版本的 PageHeader
 */
class PageFactory {
private:
    // Magic number 定义
    static constexpr uint32_t INDEX_PAGE_MAGIC = 0x000026F1;  // FIL_PAGE_TYPE=INDEX
    static constexpr uint32_t SDI_PAGE_MAGIC = 0x000026F3;     // FIL_PAGE_TYPE=SDI
    static constexpr uint32_t UNDO_PAGE_MAGIC = 0x000026F2;    // FIL_PAGE_TYPE=UNDO_LOG

    // 页大小常量
    static constexpr size_t DEFAULT_PAGE_SIZE = 16384;
    static constexpr size_t MIN_PAGE_SIZE = 4096;
    static constexpr size_t MAX_PAGE_SIZE = 65536;

public:
    // SDI Signature (8 bytes) - public for external access
    static constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};

    /**
     * @brief 创建 PageHeader 实例
     * @param page_data 页数据指针
     * @param page_size 页大小（默认 16KB）
     * @return PageHeaderBase 智能指针
     */
    static std::unique_ptr<PageHeaderBase> create_page_header(
        const uint8_t* page_data,
        size_t page_size = DEFAULT_PAGE_SIZE
    );

    /**
     * @brief 检测 MySQL 版本
     * @param page_data 页数据指针
     * @param page_size 页大小
     * @return MySQLVersion 版本枚举
     */
    static MySQLVersion detect_version(const uint8_t* page_data, size_t page_size = DEFAULT_PAGE_SIZE);

    /**
     * @brief 检测行格式
     * @param page_data 页数据指针
     * @param version MySQL 版本
     * @return RowFormat 行格式枚举
     */
    static RowFormat detect_row_format(
        const uint8_t* page_data,
        MySQLVersion version,
        size_t page_size = DEFAULT_PAGE_SIZE
    );

    /**
     * @brief 检查是否为 SDI 页
     * @param page_data 页数据指针
     * @param page_size 页大小
     * @return bool 是否为 SDI 页
     */
    static bool is_sdi_page(const uint8_t* page_data, size_t page_size = DEFAULT_PAGE_SIZE);

    /**
     * @brief 从记录头检测行格式
     * @param record_data 记录数据指针
     * @param record_len 记录长度
     * @return RowFormat 行格式枚举
     */
    static RowFormat detect_row_format_from_record(
        const uint8_t* record_data,
        size_t record_len
    );

    /**
     * @brief 获取页类型字符串
     */
    static std::string page_type_to_string(uint16_t page_type);
};

// ==================== MySQL 5.6/5.7 Page Header 实现 ====================
/**
 * @brief MySQL 5.6/5.7 Page Header 实现
 * @details 支持 Redundant/Compact 行格式
 */
class PageHeaderV56 : public PageHeaderBase {
private:
    static constexpr size_t HEADER_SIZE = 96;  // 5.6/5.7 Page Header 总大小
    static constexpr uint16_t DATA_START_OFFSET = 38;  // 5.7 开始为 38

    std::unique_ptr<uint8_t[]> data_;
    size_t page_size_;

public:
    /**
     * @brief 构造函数
     * @param page_data 页数据指针
     * @param page_size 页大小
     */
    PageHeaderV56(const uint8_t* page_data, size_t page_size = 16384);

    // PageHeaderBase 接口实现
    uint32_t get_prev_page() const override;
    uint32_t get_next_page() const override;
    uint64_t get_lsn() const override;
    PageType get_page_type() const override;
    uint16_t get_level() const override;
    uint16_t get_n_recs() const override;
    uint16_t get_free_ptr() const override;
    uint16_t get_free_size() const override;
    uint16_t get_n_dir_slots() const override;
    uint16_t get_data_start() const override;

    bool verify_checksum() const override;
    uint32_t calculate_checksum() const override;

    MySQLVersion get_version() const override { return MySQLVersion::MYSQL_57; }
    RowFormat get_row_format() const override;

    uint32_t get_space_id() const override;
    uint32_t get_page_offset() const override;
    uint16_t get_max_trx_id() const override;

    // 加密支持 (MySQL 8.4+)
    bool is_encrypted() const override { return false; }
    uint32_t get_encryption_key_version() const override { return 0; }

    // 压缩支持
    bool is_compressed() const override;
    uint16_t get_zip_size() const override;

    std::string to_string() const override;

    // 5.6/5.7 特有方法
    uint16_t get_header_version() const;
    uint32_t get_file_flush_lsn() const;
};

// ==================== MySQL 8.0+ Page Header 实现 ====================
/**
 * @brief MySQL 8.0+ Page Header 实现
 * @details 支持 Dynamic/Compressed 行格式
 */
class PageHeaderV80 : public PageHeaderBase {
private:
    static constexpr size_t HEADER_SIZE = 120;  // 8.0+ Page Header 总大小
    static constexpr uint16_t DATA_START_OFFSET = 38;

    std::unique_ptr<uint8_t[]> data_;
    size_t page_size_;
    MySQLVersion version_;

public:
    /**
     * @brief 构造函数
     * @param page_data 页数据指针
     * @param page_size 页大小
     * @param version MySQL 版本
     */
    PageHeaderV80(
        const uint8_t* page_data,
        size_t page_size = 16384,
        MySQLVersion version = MySQLVersion::MYSQL_80
    );

    // PageHeaderBase 接口实现
    uint32_t get_prev_page() const override;
    uint32_t get_next_page() const override;
    uint64_t get_lsn() const override;
    PageType get_page_type() const override;
    uint16_t get_level() const override;
    uint16_t get_n_recs() const override;
    uint16_t get_free_ptr() const override;
    uint16_t get_free_size() const override;
    uint16_t get_n_dir_slots() const override;
    uint16_t get_data_start() const override;

    bool verify_checksum() const override;
    uint32_t calculate_checksum() const override;

    MySQLVersion get_version() const override { return version_; }
    RowFormat get_row_format() const override;

    uint32_t get_space_id() const override;
    uint32_t get_page_offset() const override;
    uint16_t get_max_trx_id() const override;

    // 加密支持 (MySQL 8.4+)
    bool is_encrypted() const override { return false; }
    uint32_t get_encryption_key_version() const override { return 0; }

    // 压缩支持
    bool is_compressed() const override;
    uint16_t get_zip_size() const override;

    std::string to_string() const override;

    // 8.0+ 特有方法
    uint64_t get_file_flush_lsn() const;
    uint16_t get_header_version() const;
    bool is_sdi_page() const;
};

// ==================== 行格式解析器实现 ====================
/**
 * @brief Compact 行格式解析器
 */
class CompactRowParser : public RowParser {
public:
    RecordHeader parse_record_header(const uint8_t* data, RowFormat format) override;
    std::vector<ColumnInfo> parse_columns(
        const uint8_t* data,
        const std::vector<uint8_t>& col_meta,
        RowFormat format
    ) override;
    std::string generate_create_statement(
        const std::vector<ColumnInfo>& cols,
        const std::unordered_map<uint32_t, std::string>& col_names
    ) override;
    std::vector<std::optional<std::string>> extract_column_values(
        const uint8_t* record_data,
        size_t record_len,
        const std::vector<ColumnInfo>& cols
    ) override;
};

/**
 * @brief Redundant 行格式解析器
 */
class RedundantRowParser : public RowParser {
public:
    RecordHeader parse_record_header(const uint8_t* data, RowFormat format) override;
    std::vector<ColumnInfo> parse_columns(
        const uint8_t* data,
        const std::vector<uint8_t>& col_meta,
        RowFormat format
    ) override;
    std::string generate_create_statement(
        const std::vector<ColumnInfo>& cols,
        const std::unordered_map<uint32_t, std::string>& col_names
    ) override;
    std::vector<std::optional<std::string>> extract_column_values(
        const uint8_t* record_data,
        size_t record_len,
        const std::vector<ColumnInfo>& cols
    ) override;
};

// ==================== SDI 解析器实现 ====================
class SDIParserImpl {
public:
    static std::vector<SDIParser::SDIEntry> parse_sdi_page_impl(
        const uint8_t* page_data,
        size_t page_size
    );
};

} // namespace mysql_recovery

#endif // PAGE_PAGE_PARSER_H
