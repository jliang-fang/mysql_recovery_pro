#ifndef DBRECOVERY_INNODB_FRM_PARSER_HPP
#define DBRECOVERY_INNODB_FRM_PARSER_HPP

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief FRM 文件解析器
 *
 * 从 MySQL .frm 文件中提取表结构元数据
 *
 * FRM 文件格式:
 * - 偏移 0-1: FRM_TYPE (0x01fe = 表, 0x5954 = 视图)
 * - 偏移 2+: 头部信息
 */
class FRMParser {
public:
    /**
     * @brief 解析 .frm 文件
     * @param frm_path .frm 文件路径
     * @return 表结构，如果解析失败返回 nullopt
     */
    static std::optional<catalog::TableSchema> parse(const std::string& frm_path);

    /**
     * @brief 解析 .frm 文件数据
     * @param data 文件数据
     * @param size 数据大小
     * @return 表结构，如果解析失败返回 nullopt
     */
    static std::optional<catalog::TableSchema> parse(const uint8_t* data, size_t size);

    /**
     * @brief 检查是否为有效的 FRM 文件
     * @param data 文件数据
     * @param size 数据大小
     * @return true 如果是有效的 FRM 文件
     */
    static bool is_valid_frm(const uint8_t* data, size_t size);

private:
    /**
     * @brief FRM 文件头部
     */
    struct FRMHeader {
        uint16_t frm_type;          // 0x01fe = table, 0x5954 = view
        uint8_t frm_version;        // >= 9
        uint8_t legacy_db_type;     // 12 = InnoDB
        uint16_t io_size;           // I/O 大小对齐
        uint32_t frm_length;        // FRM 文件长度
        uint16_t tmp_key_length;     // 临时键长度
        uint16_t rec_length;         // 记录长度
        uint32_t create_info_max_rows;
        uint32_t create_info_min_rows;
        uint16_t key_info_length;
        uint16_t create_info_table_option;
        uint32_t avg_row_length;
        uint8_t default_table_charset;
        uint8_t row_type;           // 行格式
        uint8_t charset_high;
        uint16_t stats_sample_pages;
        uint8_t stats_auto_recalc;
        uint32_t key_length;
        uint32_t mysql_version_id;   // e.g., 50700 for MySQL 5.7
        uint32_t extra_size;
        uint16_t extra_rec_buf_length;
        uint8_t default_part_db_type;
        uint16_t key_block_size;
    };

    /**
     * @brief 列定义信息
     */
    struct ColumnDef {
        uint16_t field_length;
        uint32_t recpos;            // 记录中的位置
        uint16_t pack_flag;
        uint8_t unireg_type;
        uint8_t charset_low;
        uint8_t interval_nr;
        uint8_t field_type;
        uint8_t collation_id;
        uint16_t comment_length;
        std::string name;
        std::string comment;
        std::vector<std::string> elements;  // ENUM/SET values
    };

    /**
     * @brief 键定义信息
     */
    struct KeyDef {
        uint8_t key_type;
        uint8_t key_algorithm;
        uint16_t flags;
        std::string name;
        std::vector<uint32_t> parts;  // 列索引
    };

    // 读取 FRM 头部
    static bool read_header(const uint8_t* data, size_t size, FRMHeader& header);

    // 读取列定义
    static bool read_columns(const uint8_t* data, size_t offset,
                            const FRMHeader& header,
                            std::vector<ColumnDef>& columns);

    // 读取键定义
    static bool read_keys(const uint8_t* data, size_t offset,
                         const FRMHeader& header,
                         std::vector<KeyDef>& keys);

    // 读取默认值
    static bool read_default_values(const uint8_t* data, size_t offset,
                                   const FRMHeader& header,
                                   std::vector<std::string>& defaults);

    // 获取 MySQL 类型名称
    static std::string get_mysql_type_name(uint8_t type_id, uint32_t length);

    // 转换字符集 ID 到名称
    static std::string charset_id_to_name(uint8_t charset_id);

    // 辅助: 从缓冲区读取 uint16/uint32 (little-endian)
    static uint16_t read_uint16(const uint8_t* data);
    static uint32_t read_uint32(const uint8_t* data);
    static uint64_t read_uint64(const uint8_t* data);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_FRM_PARSER_HPP
