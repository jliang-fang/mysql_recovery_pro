#ifndef DBRECOVERY_INNODB_PAGE_EXPORTER_HPP
#define DBRECOVERY_INNODB_PAGE_EXPORTER_HPP

/**
 * @file page_exporter.hpp
 * @brief 页数据导出功能
 *
 * 支持将解析后的页数据导出为多种格式：
 * - JSON
 * - CSV
 * - SQL
 * - 原始二进制
 */

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <fstream>
#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== 页导出结果 ====================
struct PageExportResult {
    bool success;
    std::string error_message;
    std::string output;           // 导出的数据
    size_t bytes_exported;        // 导出的字节数
    size_t records_exported;      // 导出的记录数
};

// ==================== 页导出选项 ====================
struct PageExportOptions {
    size_t max_records = 1000;        // 最大记录数
    std::string output_format = "json"; // 输出格式: json, csv, sql, binary
    bool include_header = true;       // 包含页头
    bool include_records = true;      // 包含记录数据
    bool include_sdi = true;          // 包含 SDI 元数据
    bool include_checksum = false;    // 包含校验和
    bool include_hex = false;         // 以十六进制格式导出
};

// ==================== 页数据导出器 ====================
/**
 * @brief PageExporter: 页数据导出器
 *
 * 支持将页数据导出为多种格式
 */
class PageExporter {
public:
    explicit PageExporter(const PageExportOptions& options);

    // 从页数据导出
    PageExportResult export_page(const uint8_t* data, size_t size);

    // 从解析结果导出
    PageExportResult export_page(const PageParseResult& result);

    // 从页链导出
    PageExportResult export_page_chain(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    // 导出为 JSON
    std::string to_json(const PageParseResult& result);

    // 导出为 CSV
    std::string to_csv(const PageParseResult& result);

    // 导出为 SQL
    std::string to_sql(const PageParseResult& result, const std::string& table_name = "recovered_data");

    // 导出为原始二进制
    std::vector<uint8_t> to_binary(const PageParseResult& result);

    // 设置导出选项
    void set_options(const PageExportOptions& options);

private:
    PageExportOptions options_;

    // 辅助函数
    std::string escape_json(const std::string& str);
    std::string escape_csv(const std::string& str);
    std::string escape_sql(const std::string& str);
    std::string bytes_to_hex(const uint8_t* data, size_t size);
};

// ==================== 页数据导入器 ====================
/**
 * @brief PageImporter: 页数据导入器
 *
 * 支持从多种格式导入页数据
 */
class PageImporter {
public:
    // 从 JSON 导入
    static std::optional<std::vector<uint8_t>> from_json(const std::string& json_str);

    // 从 CSV 导入
    static std::optional<std::vector<uint8_t>> from_csv(const std::string& csv_str, size_t page_size = 16384);

    // 从 SQL 导入
    static std::optional<std::vector<uint8_t>> from_sql(const std::string& sql_str, size_t page_size = 16384);

    // 从二进制文件导入
    static std::optional<std::vector<uint8_t>> from_binary_file(const std::string& file_path);
};

// ==================== 页数据统计 ====================
/**
 * @brief PageStatistics: 页数据统计
 */
struct PageStatistics {
    size_t total_pages;
    size_t valid_pages;
    size_t invalid_pages;
    size_t total_records;
    size_t deleted_records;
    size_t total_bytes;
    std::map<uint16_t, size_t> page_type_counts;
    std::map<MySQLVersion, size_t> version_counts;
    std::map<RowFormat, size_t> format_counts;
    std::map<PageState, size_t> state_counts;
};

// ==================== 页统计器 ====================
/**
 * @brief PageStatisticsCalculator: 页统计计算器
 */
class PageStatisticsCalculator {
public:
    PageStatisticsCalculator();

    // 添加页进行统计
    void add_page(const uint8_t* data, size_t size);

    // 添加解析结果进行统计
    void add_page(const PageParseResult& result);

    // 计算统计信息
    PageStatistics calculate() const;

    // 重置统计
    void reset();

private:
    PageStatistics stats_;
};

// ==================== 页数据比较 ====================
/**
 * @brief PageComparator: 页数据比较器
 */
struct PageComparison {
    bool identical;
    std::vector<std::string> differences;
    size_t bytes_different;
    size_t records_different;
};

class PageComparator {
public:
    // 比较两个页
    static PageComparison compare(const uint8_t* data1, size_t size1,
                                   const uint8_t* data2, size_t size2);

    // 比较两个解析结果
    static PageComparison compare(const PageParseResult& result1,
                                   const PageParseResult& result2);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_EXPORTER_HPP
