#ifndef DBRECOVERY_RECOVERY_ENHANCED_RECOVERY_HPP
#define DBRECOVERY_RECOVERY_ENHANCED_RECOVERY_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <optional>
#include <filesystem>
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/common/version_compat.hpp"

namespace dbrecovery {
namespace recovery {

/**
 * @brief 扫描到的表信息
 */
struct ScannedTable {
    std::string table_name;
    std::string database_name;
    std::string ibd_path;
    uint64_t table_id;
    uint32_t total_pages;
    uint32_t valid_pages;
    uint32_t index_pages;
    uint32_t sdi_pages;
    uint32_t undo_pages;
    uint32_t blob_pages;
    uint64_t estimated_row_count;
    catalog::TableSchema schema;
    bool has_sdi;
    bool is_corrupted;
    std::string corruption_reason;
};

/**
 * @brief 扫描结果
 */
struct ScanResult {
    std::string data_dir;
    uint64_t total_size;
    uint32_t total_files;
    uint32_t ibd_files;
    uint32_t sdi_files;
    uint32_t frm_files;
    std::vector<ScannedTable> tables;
    std::vector<std::string> errors;
    uint64_t scan_time_ms;
};

/**
 * @brief 表名搜索结果
 */
struct TableSearchResult {
    std::string table_name;
    std::string database_name;
    std::string ibd_path;
    uint64_t table_id;
    bool found_in_sdi;
    bool found_in_filename;
    catalog::TableSchema schema;
};

/**
 * @brief 磁盘扫描器 - 支持全盘扫描和设备扫描
 */
class DiskScanner {
public:
    DiskScanner();
    ~DiskScanner();

    // 禁止拷贝
    DiskScanner(const DiskScanner&) = delete;
    DiskScanner& operator=(const DiskScanner&) = delete;

    /**
     * @brief 设置页大小
     */
    void set_page_size(uint32_t size);

    /**
     * @brief 设置扫描深度（目录递归深度）
     */
    void set_max_depth(int depth);

    /**
     * @brief 扫描数据目录
     * 
     * @param data_dir 数据目录路径
     * @return true 找到表
     * @return false 未找到表
     */
    bool scan_data_directory(const std::string& data_dir);

    /**
     * @brief 扫描单个目录
     * 
     * @param dir_path 目录路径
     * @param depth 当前深度
     * @return true 找到表
     * @return false 未找到表
     */
    bool scan_directory(const std::string& dir_path, int depth = 0);

    /**
     * @brief 扫描原始设备
     * 
     * @param device_path 设备路径
     * @return true 找到 InnoDB 数据
     * @return false 未找到
     */
    bool scan_raw_device(const std::string& device_path);

    /**
     * @brief 扫描整个磁盘
     * 
     * @param disk_path 磁盘路径（如 /dev/sda）
     * @return true 找到 InnoDB 数据
     * @return false 未找到
     */
    bool scan_disk(const std::string& disk_path);

    /**
     * @brief 获取扫描到的表列表
     */
    [[nodiscard]] const std::vector<ScannedTable>& get_scanned_tables() const;

    /**
     * @brief 获取扫描结果
     */
    [[nodiscard]] const ScanResult& get_scan_result() const;

    /**
     * @brief 获取扫描到的 SDI 页
     */
    [[nodiscard]] const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& 
    get_sdi_pages() const;

    /**
     * @brief 获取扫描到的索引页
     */
    [[nodiscard]] const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& 
    get_index_pages() const;

    /**
     * @brief 获取扫描到的 BLOB 页
     */
    [[nodiscard]] const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& 
    get_blob_pages() const;

private:
    uint32_t page_size_;
    int max_depth_;
    ScanResult scan_result_;
    std::vector<ScannedTable> scanned_tables_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> sdi_pages_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> index_pages_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> blob_pages_;

    // 表名到路径的映射
    std::unordered_map<std::string, std::string> table_name_to_path_;

    // 扫描统计
    uint64_t total_bytes_scanned_;
    uint32_t pages_analyzed_;

    /**
     * @brief 分析单个 .ibd 文件
     */
    bool analyze_ibd_file(const std::string& path);

    /**
     * @brief 从 SDI 页解析表结构
     */
    std::optional<catalog::TableSchema> parse_schema_from_sdi(
        const std::vector<uint8_t>& sdi_payload);

    /**
     * @brief 从文件名推断表名
     */
    std::pair<std::string, std::string> infer_table_from_path(const std::string& path);
};

/**
 * @brief 表名搜索器 - 通过表名搜索表
 */
class TableNameSearcher {
public:
    TableNameSearcher();
    ~TableNameSearcher();

    // 禁止拷贝
    TableNameSearcher(const TableNameSearcher&) = delete;
    TableNameSearcher& operator=(const TableNameSearcher&) = delete;

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置页大小
     */
    void set_page_size(uint32_t size);

    /**
     * @brief 搜索表名
     * 
     * @param table_name 表名（支持通配符 * 和 ?）
     * @return 搜索结果列表
     */
    std::vector<TableSearchResult> search_table(const std::string& table_name);

    /**
     * @brief 搜索所有表
     */
    std::vector<TableSearchResult> search_all_tables();

    /**
     * @brief 从 SDI 搜索表
     */
    std::vector<TableSearchResult> search_from_sdi(const std::string& table_name);

    /**
     * @brief 从文件名搜索表
     */
    std::vector<TableSearchResult> search_from_filename(const std::string& table_name);

    /**
     * @brief 获取搜索到的表数量
     */
    [[nodiscard]] size_t get_search_count() const;

private:
    std::string data_dir_;
    uint32_t page_size_;
    std::vector<TableSearchResult> search_results_;

    /**
     * @brief 递归搜索目录
     */
    void search_directory(const std::string& dir, const std::string& pattern);

    /**
     * @brief 检查文件名是否匹配
     */
    bool match_filename(const std::string& filename, const std::string& pattern);

    /**
     * @brief 解析表名和数据库名
     */
    std::pair<std::string, std::string> parse_table_name(const std::string& path);
};

/**
 * @brief 批量恢复器 - 批量恢复多个表
 */
class BulkRecovery {
public:
    BulkRecovery();
    ~BulkRecovery();

    // 禁止拷贝
    BulkRecovery(const BulkRecovery&) = delete;
    BulkRecovery& operator=(const BulkRecovery&) = delete;

    /**
     * @brief 设置输出目录
     */
    void set_output_directory(const std::string& dir);

    /**
     * @brief 设置格式
     */
    void set_format(const std::string& format);

    /**
     * @brief 添加要恢复的表
     */
    void add_table(const ScannedTable& table);

    /**
     * @brief 批量恢复所有表
     */
    bool recover_all();

    /**
     * @brief 批量恢复指定表
     */
    bool recover_tables(const std::vector<ScannedTable>& tables);

    /**
     * @brief 获取恢复统计
     */
    [[nodiscard]] const std::map<std::string, uint64_t>& get_statistics() const;

private:
    std::string output_dir_;
    std::string format_;
    std::vector<ScannedTable> tables_;
    std::map<std::string, uint64_t> statistics_;

    /**
     * @brief 恢复单个表
     */
    bool recover_single_table(const ScannedTable& table);
};

/**
 * @brief 自动表结构推断器
 */
class AutoSchemaInferer {
public:
    AutoSchemaInferer();
    ~AutoSchemaInferer();

    /**
     * @brief 从页数据推断表结构
     */
    std::optional<catalog::TableSchema> infer_from_page(
        const std::vector<uint8_t>& page_data);

    /**
     * @brief 从多个页推断表结构
     */
    std::optional<catalog::TableSchema> infer_from_pages(
        const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    /**
     * @brief 从 SDI 推断表结构
     */
    std::optional<catalog::TableSchema> infer_from_sdi(
        const std::vector<uint8_t>& sdi_payload);

    /**
     * @brief 从记录推断列信息
     */
    std::vector<catalog::ColumnInfo> infer_columns_from_records(
        const std::vector<std::map<std::string, std::string>>& records);

private:
    uint32_t page_size_;
    std::vector<catalog::ColumnInfo> inferred_columns_;

    /**
     * @brief 分析记录头推断列数
     */
    uint32_t infer_column_count_from_header(const uint8_t* record_header);

    /**
     * @brief 推断列类型
     */
    uint8_t infer_column_type(const std::string& value);
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_ENHANCED_RECOVERY_HPP
