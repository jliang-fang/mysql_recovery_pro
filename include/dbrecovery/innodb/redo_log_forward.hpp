#ifndef DBRECOVERY_INNODB_REDO_LOG_FORWARD_HPP
#define DBRECOVERY_INNODB_REDO_LOG_FORWARD_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <deque>
#include <set>
#include <algorithm>
#include <functional>
#include <chrono>
#include <sstream>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/redo_log.hpp"
#include "dbrecovery/innodb/redo_log_enhanced.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Redo Log 前滚恢复统计 ====================
struct RedoLogRollforwardStats {
    size_t total_records = 0;
    size_t records_applied = 0;
    size_t records_skipped = 0;
    size_t pages_modified = 0;
    size_t conflicts_resolved = 0;
    double parse_time_ms = 0.0;
    double apply_time_ms = 0.0;
    double total_time_ms = 0.0;

    // LSN 范围
    uint64_t start_lsn = 0;
    uint64_t end_lsn = 0;
    uint32_t start_file_no = 0;
    uint32_t end_file_no = 0;

    std::string to_string() const;
};

// ==================== Redo Log 前滚恢复器 ====================
class RedoLogRollforward {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit RedoLogRollforward(size_t page_size = 16384);

    /**
     * @brief 析构函数
     */
    ~RedoLogRollforward();

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置 Redo Log 路径
     */
    void set_redo_log_path(const std::string& path);

    /**
     * @brief 设置恢复的 LSN 范围
     */
    void set_lsn_range(uint64_t start_lsn, uint64_t end_lsn);

    /**
     * @brief 设置检查点 LSN
     */
    void set_checkpoint_lsn(uint64_t checkpoint_lsn);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置是否应用恢复
     */
    void set_apply_recovery(bool apply);

    /**
     * @brief 执行 Redo Log 前滚恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 从 Redo Log 文件恢复
     * @param redo_log_path Redo Log 文件路径
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover_from_redo_log(const std::string& redo_log_path);

    /**
     * @brief 获取统计信息
     */
    const RedoLogRollforwardStats& get_stats() const { return stats_; }

    /**
     * @brief 获取应用的操作
     */
    const std::vector<RedoOperation>& get_operations() const { return operations_; }

    /**
     * @brief 获取修改的页
     */
    const std::set<uint32_t>& get_modified_pages() const { return modified_pages_; }

    /**
     * @brief 获取事务信息
     */
    const std::map<uint64_t, RedoLogAnalyzer::TransactionInfo>& get_transactions() const {
        return analyzer_.get_transactions();
    }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::string data_dir_;
    std::string redo_log_path_;
    uint64_t start_lsn_ = 0;
    uint64_t end_lsn_ = 0;
    uint64_t checkpoint_lsn_ = 0;
    catalog::TableSchema schema_;
    bool apply_recovery_ = true;

    RedoLogRollforwardStats stats_;
    std::vector<RedoOperation> operations_;
    std::set<uint32_t> modified_pages_;
    std::map<uint32_t, std::vector<uint8_t>> pages_;
    RedoLogAnalyzer analyzer_;

    // 扫描数据文件
    bool scan_data_files();

    // 解析 Redo Log 记录
    std::vector<RedoLogRecord> parse_redo_log();

    // 应用 Redo Log 操作
    bool apply_redo_operations(const std::vector<RedoLogRecord>& records);

    // 解析单个 Redo Log 记录
    std::optional<RedoOperation> parse_redo_record(const RedoLogRecord& record, uint64_t lsn);

    // 应用插入操作
    bool apply_insert(const RedoOperation& op);

    // 应用更新操作
    bool apply_update(const RedoOperation& op);

    // 应用删除操作
    bool apply_delete(const RedoOperation& op);

    // 获取页数据
    std::optional<std::vector<uint8_t>> get_page(uint32_t page_no);

    // 设置页数据
    void set_page(uint32_t page_no, std::vector<uint8_t> page_data);
};

// ==================== Redo Log 前滚扫描器 ====================
class RedoLogRollforwardScanner {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit RedoLogRollforwardScanner(size_t page_size = 16384);

    /**
     * @brief 析构函数
     */
    ~RedoLogRollforwardScanner();

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置 Redo Log 目录
     */
    void set_redo_log_directory(const std::string& dir);

    /**
     * @brief 扫描 Redo Log
     * @return Redo Log 文件列表
     */
    std::vector<std::string> scan_redo_logs();

    /**
     * @brief 获取检查点信息
     */
    struct CheckpointInfo {
        uint64_t lsn;
        uint32_t page_no;
        uint32_t file_no;
    };

    std::optional<CheckpointInfo> get_checkpoint(const std::string& redo_log_path);

    /**
     * @brief 获取统计信息
     */
    const RedoLogRollforwardStats& get_stats() const { return stats_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::string data_dir_;
    std::string redo_log_dir_;
    RedoLogRollforwardStats stats_;
    std::vector<std::string> redo_log_files_;

    void scan_directory(const std::string& dir);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_REDO_LOG_FORWARD_HPP
