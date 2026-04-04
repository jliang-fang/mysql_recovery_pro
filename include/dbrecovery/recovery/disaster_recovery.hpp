#ifndef DBRECOVERY_RECOVERY_DISASTER_RECOVERY_HPP
#define DBRECOVERY_RECOVERY_DISASTER_RECOVERY_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <optional>
#include <filesystem>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/recovery/enhanced_recovery.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/redo_log_forward.hpp"

namespace dbrecovery {
namespace recovery {

// ==================== 灾难恢复统计 ====================
struct DisasterRecoveryStats {
    uint64_t total_files_scanned = 0;
    uint64_t total_bytes_scanned = 0;
    uint32_t ibd_files_found = 0;
    uint32_t sdi_files_found = 0;
    uint32_t tables_recovered = 0;
    uint32_t tables_failed = 0;
    uint64_t total_records_recovered = 0;
    uint64_t total_data_size_bytes = 0;
    double scan_time_ms = 0.0;
    double recovery_time_ms = 0.0;
    double total_time_ms = 0.0;
    uint32_t pages_analyzed = 0;
    uint32_t corrupted_pages = 0;
    uint32_t pages_with_data = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "DisasterRecoveryStats{files=" << total_files_scanned
            << ", ibd=" << ibd_files_found
            << ", tables=" << tables_recovered
            << ", records=" << total_records_recovered
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

// ==================== 恢复的表 ====================
struct RecoveredTable {
    std::string database_name;
    std::string table_name;
    std::string ibd_path;
    uint64_t table_id;
    catalog::TableSchema schema;
    uint32_t total_pages;
    uint32_t valid_pages;
    uint64_t record_count;
    uint64_t data_size_bytes;
    std::vector<std::map<std::string, std::string>> records;
    std::string output_csv;
    std::string output_json;
    std::string output_sql;
    bool success;
    std::string error_message;
};

// ==================== 灾难恢复器 - 完整数据目录恢复 ====================
class DisasterRecovery {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit DisasterRecovery(uint32_t page_size = 16384);

    /**
     * @brief 析构函数
     */
    ~DisasterRecovery();

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置输出目录
     */
    void set_output_directory(const std::string& dir);

    /**
     * @brief 设置是否导出 CSV
     */
    void set_export_csv(bool enabled);

    /**
     * @brief 设置是否导出 JSON
     */
    void set_export_json(bool enabled);

    /**
     * @brief 设置是否导出 SQL
     */
    void set_export_sql(bool enabled);

    /**
     * @brief 设置是否使用多线程
     */
    void set_multithreaded(bool enabled);

    /**
     * @brief 设置线程数
     */
    void set_thread_count(uint32_t threads);

    /**
     * @brief 设置是否恢复 BLOB 数据
     */
    void set_blob_recovery(bool enabled);

    /**
     * @brief 设置是否恢复 Redo Log
     */
    void set_redo_log_recovery(bool enabled);

    /**
     * @brief 设置 Redo Log 路径
     */
    void set_redo_log_path(const std::string& path);

    /**
     * @brief 执行灾难恢复
     * @return 恢复的表列表
     */
    std::vector<RecoveredTable> recover();

    /**
     * @brief 从单个 .ibd 文件恢复
     */
    bool recover_from_ibd_file(const std::string& ibd_path, RecoveredTable& table);

    /**
     * @brief 获取统计信息
     */
    const DisasterRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取恢复的表
     */
    const std::vector<RecoveredTable>& get_recovered_tables() const { return recovered_tables_; }

    /**
     * @brief 获取扫描到的表
     */
    const std::vector<ScannedTable>& get_scanned_tables() const { return scanned_tables_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    uint32_t page_size_;
    std::string data_dir_;
    std::string output_dir_;
    bool export_csv_ = true;
    bool export_json_ = true;
    bool export_sql_ = true;
    bool multithreaded_ = true;
    uint32_t thread_count_ = 4;
    bool blob_recovery_ = true;
    bool redo_log_recovery_ = false;
    std::string redo_log_path_;

    DisasterRecoveryStats stats_;
    std::vector<ScannedTable> scanned_tables_;
    std::vector<RecoveredTable> recovered_tables_;

    // 扫描数据目录
    bool scan_data_directory();

    // 扫描单个目录
    bool scan_directory(const std::string& dir, int depth = 0);

    // 分析单个 .ibd 文件
    bool analyze_ibd_file(const std::string& path, ScannedTable& table);

    // 从 SDI 解析表结构
    std::optional<catalog::TableSchema> parse_schema_from_sdi(const std::string& ibd_path);

    // 恢复单个表
    bool recover_single_table(const ScannedTable& table, RecoveredTable& recovered);

    // 导出为 CSV
    bool export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                       const catalog::TableSchema& schema, const std::string& path);

    // 导出为 JSON
    bool export_to_json(const std::vector<std::map<std::string, std::string>>& records,
                        const catalog::TableSchema& schema, const std::string& path);

    // 导出为 SQL
    bool export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                       const catalog::TableSchema& schema, const std::string& path);

    // 多线程恢复
    void recover_multithreaded();

    // 单线程恢复
    void recover_singlethreaded();

    // 线程工作函数
    void worker_thread(uint32_t start_idx, uint32_t end_idx);
};

// ==================== 完全恢复器 - 从零开始恢复 ====================
class CompleteRecovery {
public:
    /**
     * @brief 构造函数
     */
    CompleteRecovery();

    /**
     * @brief 析构函数
     */
    ~CompleteRecovery();

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置输出目录
     */
    void set_output_directory(const std::string& dir);

    /**
     * @brief 设置是否扫描所有设备
     */
    void set_scan_all_devices(bool enabled);

    /**
     * @brief 设置是否恢复所有表
     */
    void set_recover_all(bool enabled);

    /**
     * @brief 设置表名过滤器
     */
    void set_table_filter(const std::string& pattern);

    /**
     * @brief 执行完全恢复
     * @return 恢复的表数量
     */
    uint32_t recover();

    /**
     * @brief 获取统计信息
     */
    const DisasterRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取恢复的表
     */
    const std::vector<RecoveredTable>& get_recovered_tables() const { return recovered_tables_; }

    /**
     * @brief 获取错误信息
     */
    const std::vector<std::string>& get_errors() const { return errors_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    std::string data_dir_;
    std::string output_dir_;
    bool scan_all_devices_ = false;
    bool recover_all_ = true;
    std::string table_filter_;
    std::string database_filter_;

    DisasterRecoveryStats stats_;
    std::vector<RecoveredTable> recovered_tables_;
    std::vector<std::string> errors_;

    // 扫描数据目录
    bool scan_data_directory();

    // 扫描原始设备
    bool scan_raw_device(const std::string& device_path);

    // 扫描整个磁盘
    bool scan_disk(const std::string& disk_path);

    // 从文件名推断表结构
    std::optional<catalog::TableSchema> infer_schema_from_filename(const std::string& path);

    // 从页数据推断表结构
    std::optional<catalog::TableSchema> infer_schema_from_pages(const std::string& ibd_path);

    // 恢复单个表
    bool recover_single_table(const std::string& ibd_path, const catalog::TableSchema& schema);

    // 导出恢复的数据
    bool export_recovered_data(const std::string& database, const std::string& table,
                               const std::vector<std::map<std::string, std::string>>& records,
                               const catalog::TableSchema& schema);
};

// ==================== 快速恢复器 - 仅恢复数据 ====================
class FastRecovery {
public:
    /**
     * @brief 构造函数
     */
    FastRecovery();

    /**
     * @brief 析构函数
     */
    ~FastRecovery();

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置输出目录
     */
    void set_output_directory(const std::string& dir);

    /**
     * @brief 设置是否跳过验证
     */
    void set_skip_validation(bool enabled);

    /**
     * @brief 设置是否仅恢复记录
     */
    void set_records_only(bool enabled);

    /**
     * @brief 执行快速恢复
     * @return 恢复的记录数量
     */
    uint64_t recover();

    /**
     * @brief 获取统计信息
     */
    const DisasterRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取恢复的记录
     */
    const std::vector<std::map<std::string, std::string>>& get_records() const { return all_records_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    std::string data_dir_;
    std::string output_dir_;
    bool skip_validation_ = false;
    bool records_only_ = false;

    DisasterRecoveryStats stats_;
    std::vector<std::map<std::string, std::string>> all_records_;
    std::unordered_map<std::string, std::vector<std::map<std::string, std::string>>> table_records_;

    // 扫描并恢复所有记录
    bool scan_and_recover_records();

    // 从单个 .ibd 文件恢复记录
    bool recover_records_from_ibd(const std::string& ibd_path);

    // 从页数据恢复记录
    std::vector<std::map<std::string, std::string>> recover_records_from_page(
        const std::vector<uint8_t>& page_data, const catalog::TableSchema& schema);

    // 导出所有记录
    bool export_all_records();
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_DISASTER_RECOVERY_HPP
