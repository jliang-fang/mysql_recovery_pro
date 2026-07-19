#ifndef DBRECOVERY_EXPORT_DATA_EXPORTER_HPP
#define DBRECOVERY_EXPORT_DATA_EXPORTER_HPP

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>

#include "dbrecovery/innodb/index_page_parser.hpp"

namespace dbrecovery {
namespace exporter {

/**
 * @brief Escape a string for JSON output
 */
std::string escape_json(const std::string& str);

/**
 * @brief DataExporter: 数据导出基类
 */
class DataExporter {
public:
    DataExporter() = default;
    virtual ~DataExporter() = default;

    // 禁止拷贝
    DataExporter(const DataExporter&) = delete;
    DataExporter& operator=(const DataExporter&) = delete;

    /**
     * @brief 导出单条记录
     *
     * @param record 记录数据
     * @return true 成功
     * @return false 失败
     */
    virtual bool export_record(const std::map<std::string, std::string>& record) = 0;

    /**
     * @brief 获取导出结果
     *
     * @return std::string 导出的字符串
     */
    virtual std::string get_result() const = 0;

    /**
     * @brief 导出到文件
     *
     * @param file_path 文件路径
     * @return true 成功
     * @return false 失败
     */
    bool export_to_file(const std::string& file_path) {
        std::ofstream file(file_path);
        if (!file) {
            std::cerr << "[ERROR] Failed to open file for writing: " << file_path << std::endl;
            return false;
        }

        std::string result = get_result();
        // DEBUG: Check result size
        static int export_debug = 0;
        if (export_debug < 3) {
            if (dbrecovery::innodb::g_verbose)             std::cerr << "[DEBUG EXPORT_FILE] result size=" << result.size() << "\n";
            export_debug++;
        }

        file << result;
        file.close();

        return true;
    }
};

/**
 * @brief CSVExporter: CSV 格式导出器
 */
class CSVExporter : public DataExporter {
public:
    explicit CSVExporter(const std::vector<std::string>& headers);
    ~CSVExporter() override;

    bool export_record(const std::map<std::string, std::string>& record) override;
    std::string get_result() const override;

private:
    std::vector<std::string> headers_;
    std::vector<std::string> rows_;
};

/**
 * @brief SQLOutputter: SQL 格式导出器
 */
class SQLOutputter : public DataExporter {
public:
    explicit SQLOutputter(const std::string& table_name);
    explicit SQLOutputter(const std::string& table_name, const std::vector<std::string>& column_order);
    ~SQLOutputter() override;

    bool export_record(const std::map<std::string, std::string>& record) override;
    std::string get_result() const override;

    // Batch INSERT: groups multiple rows into single INSERT statement
    std::string get_result_batch(size_t batch_size) const;

private:
    std::string table_name_;
    std::vector<std::string> column_order_;
    std::vector<std::string> sql_statements_;
};

/**
 * @brief IBDExporter: IBD 格式导出器（修复后的表空间）
 */
class IBDExporter : public DataExporter {
public:
    explicit IBDExporter(uint32_t page_size = 16384);
    ~IBDExporter() override;

    bool export_record(const std::map<std::string, std::string>& record) override;
    std::string get_result() const override;

    /**
     * @brief 导出到 IBD 文件
     *
     * @param file_path 文件路径
     * @param table_schema 表结构
     * @return true 成功
     * @return false 失败
     */
    bool export_to_ibd(const std::string& file_path,
                      const std::map<std::string, std::string>& table_schema);

private:
    uint32_t page_size_;
    std::vector<std::vector<uint8_t>> pages_;
    std::vector<std::map<std::string, std::string>> stored_records_;
};

/**
 * @brief JSONExporter: JSON format exporter
 *
 * Outputs records as a JSON array of objects.
 * Supports pretty-printing and streaming to avoid memory issues with large datasets.
 */
class JSONExporter : public DataExporter {
public:
    explicit JSONExporter(bool pretty = false);
    ~JSONExporter() override;

    bool export_record(const std::map<std::string, std::string>& record) override;
    std::string get_result() const override;

    // Write directly to file (streaming mode, avoids full in-memory result)
    bool export_to_file_streaming(const std::string& file_path);

private:
    bool pretty_;
    std::vector<std::string> rows_;
};

/**
 * @brief 导出工具类
 */
class Exporter {
public:
    Exporter() = default;
    ~Exporter() = default;

    /**
     * @brief 导出为 CSV
     *
     * @param records 记录数据
     * @param headers 表头
     * @param output_path 输出文件路径
     * @return true 成功
     * @return false 失败
     */
    static bool export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                              const std::vector<std::string>& headers,
                              const std::string& output_path);

    /**
     * @brief 导出为 SQL
     *
     * @param records 记录数据
     * @param table_name 表名
     * @param output_path 输出文件路径
     * @return true 成功
     * @return false 失败
     */
    static bool export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                              const std::string& table_name,
                              const std::string& output_path);

    /**
     * @brief 导出为 SQL（保持列顺序）
     *
     * @param records 记录数据
     * @param table_name 表名
     * @param column_order 列顺序
     * @param output_path 输出文件路径
     * @return true 成功
     * @return false 失败
     */
    static bool export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                              const std::string& table_name,
                              const std::vector<std::string>& column_order,
                              const std::string& output_path);

    /**
     * @brief 导出为 IBD
     *
     * @param records 记录数据
     * @param output_path 输出文件路径
     * @return true 成功
     * @return false 失败
     */
    static bool export_to_ibd(const std::vector<std::map<std::string, std::string>>& records,
                              const std::string& output_path);

    /**
     * @brief 导出为 JSON
     *
     * @param records 记录数据
     * @param output_path 输出文件路径
     * @param pretty 是否美化输出
     * @return true 成功
     * @return false 失败
     */
    static bool export_to_json(const std::vector<std::map<std::string, std::string>>& records,
                               const std::string& output_path,
                               bool pretty = false);
};

} // namespace exporter
} // namespace dbrecovery

#endif // DBRECOVERY_EXPORT_DATA_EXPORTER_HPP
