#ifndef DBRECOVERY_INNODB_REDO_LOG_ROLLFORWARD_HPP
#define DBRECOVERY_INNODB_REDO_LOG_ROLLFORWARD_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <deque>
#include <set>
#include <algorithm>
#include <functional>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/redo_log.hpp"
#include "dbrecovery/innodb/redo_log_enhanced.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief Redo Log 前滚恢复统计
 */
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

    std::string to_string() const {
        std::ostringstream oss;
        oss << "RedoLogRollforwardStats{records=" << total_records
            << ", applied=" << records_applied
            << ", skipped=" << records_skipped
            << ", pages=" << pages_modified
            << ", lsn=" << start_lsn << "-" << end_lsn
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

/**
 * @brief Redo Log 前滚恢复器
 */
class RedoLogRollforward {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit RedoLogRollforward(size_t page_size = 16384);

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

// ==================== RedoLogRollforward 实现 ====================

inline RedoLogRollforward::RedoLogRollforward(size_t page_size)
    : page_size_(page_size), apply_recovery_(true) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void RedoLogRollforward::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

inline void RedoLogRollforward::set_redo_log_path(const std::string& path) {
    redo_log_path_ = path;
}

inline void RedoLogRollforward::set_lsn_range(uint64_t start_lsn, uint64_t end_lsn) {
    start_lsn_ = start_lsn;
    end_lsn_ = end_lsn;
}

inline void RedoLogRollforward::set_checkpoint_lsn(uint64_t checkpoint_lsn) {
    checkpoint_lsn_ = checkpoint_lsn;
}

inline void RedoLogRollforward::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void RedoLogRollforward::set_apply_recovery(bool apply) {
    apply_recovery_ = apply;
}

inline void RedoLogRollforward::clear() {
    operations_.clear();
    modified_pages_.clear();
    pages_.clear();
    analyzer_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline bool RedoLogRollforward::scan_data_files() {
    if (data_dir_.empty()) {
        return false;
    }

    std::filesystem::path directory(data_dir_);

    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        return false;
    }

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();

                if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
                    std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
                    if (!file) continue;

                    uint64_t file_size = file.tellg();
                    file.seekg(0, std::ios::beg);

                    if (file_size < page_size_) {
                        continue;
                    }

                    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
                    std::vector<uint8_t> page_data(page_size_);

                    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
                        file.seekg(page_no * page_size_, std::ios::beg);
                        file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

                        uint16_t page_type = read_le<uint16_t>(page_data.data(), 16);
                        if (page_type != FIL_PAGE_INDEX) {
                            continue;
                        }

                        pages_[page_no] = page_data;
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // 忽略错误
    }

    return !pages_.empty();
}

inline std::vector<RedoLogRecord> RedoLogRollforward::parse_redo_log() {
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<RedoLogRecord> records;

    if (redo_log_path_.empty()) {
        return records;
    }

    std::ifstream file(redo_log_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return records;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 5) {
        return records;
    }

    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);

    // 解析 Redo Log 记录
    size_t pos = 0;
    uint64_t lsn = 0;

    while (pos + 5 <= buffer.size()) {
        RedoLogRecord record;
        record.r_type = read_le<uint8_t>(buffer.data(), pos);
        pos += sizeof(uint8_t);

        if (pos + 8 <= buffer.size()) {
            record.r_space_id = read_le<uint32_t>(buffer.data(), pos);
            pos += sizeof(uint32_t);
            record.r_page_no = read_le<uint32_t>(buffer.data(), pos);
            pos += sizeof(uint32_t);
        }

        if (pos < buffer.size()) {
            record.r_data.assign(buffer.begin() + pos, buffer.end());
        }

        record.r_lsn = lsn;
        records.push_back(record);

        // 简化：假设每个记录至少 5 字节
        pos += record.r_data.size() + 1;
        lsn += record.r_data.size() + 1;
    }

    stats_.total_records = records.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.parse_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return records;
}

inline std::optional<RedoOperation> RedoLogRollforward::parse_redo_record(const RedoLogRecord& record, uint64_t lsn) {
    RedoOperation op;
    op.lsn = lsn;
    op.space_id = record.r_space_id;
    op.page_no = record.r_page_no;
    op.data = record.r_data;

    // 根据类型解析
    switch (record.r_type) {
        case MLOG_1BYTE:
        case MLOG_2BYTES:
        case MLOG_4BYTES:
        case MLOG_8BYTES:
            op.type = RedoOperationType::UPDATE;
            break;
        case MLOG_REC_INSERT:
            op.type = RedoOperationType::INSERT;
            break;
        case MLOG_REC_DELETE:
            op.type = RedoOperationType::DELETE;
            break;
        case MLOG_PAGE_CREATE:
            op.type = RedoOperationType::PAGE_CREATE;
            break;
        case MLOG_PAGE_REINIT:
            op.type = RedoOperationType::PAGE_REINIT;
            break;
        default:
            op.type = RedoOperationType::UNKNOWN;
            break;
    }

    return op;
}

inline bool RedoLogRollforward::apply_redo_operations(const std::vector<RedoLogRecord>& records) {
    auto start = std::chrono::high_resolution_clock::now();

    operations_.clear();
    modified_pages_.clear();

    for (const auto& record : records) {
        auto op_opt = parse_redo_record(record, record.r_lsn);
        if (!op_opt.has_value()) {
            stats_.records_skipped++;
            continue;
        }

        auto op = op_opt.value();
        operations_.push_back(op);

        if (apply_recovery_) {
            bool success = false;
            switch (op.type) {
                case RedoOperationType::INSERT:
                    success = apply_insert(op);
                    break;
                case RedoOperationType::UPDATE:
                    success = apply_update(op);
                    break;
                case RedoOperationType::DELETE:
                    success = apply_delete(op);
                    break;
                default:
                    success = true;
                    break;
            }

            if (success) {
                stats_.records_applied++;
                modified_pages_.insert(op.page_no);
            } else {
                stats_.records_skipped++;
            }
        } else {
            stats_.records_applied++;
        }
    }

    stats_.pages_modified = modified_pages_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.apply_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_time_ms = stats_.parse_time_ms + stats_.apply_time_ms;

    return !operations_.empty();
}

inline bool RedoLogRollforward::apply_insert(const RedoOperation& op) {
    // 解析插入记录
    if (op.data.size() < 5) {
        return false;
    }

    // 简化实现：记录插入操作
    // 实际需要解析记录格式并插入到页中
    return true;
}

inline bool RedoLogRollforward::apply_update(const RedoOperation& op) {
    // 解析更新记录
    if (op.data.size() < 5) {
        return false;
    }

    // 简化实现：记录更新操作
    // 实际需要解析更新字段并应用到页中
    return true;
}

inline bool RedoLogRollforward::apply_delete(const RedoOperation& op) {
    // 解析删除记录
    if (op.data.size() < 2) {
        return false;
    }

    // 简化实现：记录删除操作
    // 实际需要解析删除的记录偏移并标记为删除
    return true;
}

inline std::optional<std::vector<uint8_t>> RedoLogRollforward::get_page(uint32_t page_no) {
    auto it = pages_.find(page_no);
    if (it == pages_.end()) {
        return std::nullopt;
    }
    return it->second;
}

inline void RedoLogRollforward::set_page(uint32_t page_no, std::vector<uint8_t> page_data) {
    pages_[page_no] = std::move(page_data);
}

inline std::vector<std::map<std::string, std::string>> RedoLogRollforward::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 扫描数据文件
    scan_data_files();

    // 解析 Redo Log
    auto records = parse_redo_log();

    // 应用 Redo Log 操作
    apply_redo_operations(records);

    // 返回恢复的记录（从修改的页中解析）
    std::vector<std::map<std::string, std::string>> recovered_records;

    for (uint32_t page_no : modified_pages_) {
        auto page_opt = get_page(page_no);
        if (!page_opt.has_value()) {
            continue;
        }

        BPlusTreeParser parser(page_opt.value());
        if (!parser.parse()) {
            continue;
        }

        auto page_records = parser.parse_records(schema_);
        recovered_records.insert(recovered_records.end(), page_records.begin(), page_records.end());
    }

    stats_.end_lsn = end_lsn_;
    if (end_lsn_ == 0 && !records.empty()) {
        stats_.end_lsn = records.back().r_lsn;
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return recovered_records;
}

inline std::vector<std::map<std::string, std::string>> RedoLogRollforward::recover_from_redo_log(
    const std::string& redo_log_path) {
    set_redo_log_path(redo_log_path);
    return recover();
}

// ==================== Redo Log 前滚扫描器 ====================
class RedoLogRollforwardScanner {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit RedoLogRollforwardScanner(size_t page_size = 16384);

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

// ==================== RedoLogRollforwardScanner 实现 ====================

inline RedoLogRollforwardScanner::RedoLogRollforwardScanner(size_t page_size)
    : page_size_(page_size) {}

inline void RedoLogRollforwardScanner::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

inline void RedoLogRollforwardScanner::set_redo_log_directory(const std::string& dir) {
    redo_log_dir_ = dir;
}

inline void RedoLogRollforwardScanner::clear() {
    redo_log_files_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void RedoLogRollforwardScanner::scan_directory(const std::string& dir) {
    std::filesystem::path directory(dir);

    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        return;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();

                // 检查是否是 Redo Log 文件
                // MySQL 8.0: ib_logfile0, ib_logfile1, etc.
                // 或 ib_logfileN
                if (filename.find("ib_logfile") == 0) {
                    redo_log_files_.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // 忽略错误
    }
}

inline std::vector<std::string> RedoLogRollforwardScanner::scan_redo_logs() {
    clear();

    if (redo_log_dir_.empty()) {
        return redo_log_files_;
    }

    scan_directory(redo_log_dir_);

    // 排序文件
    std::sort(redo_log_files_.begin(), redo_log_files_.end());

    stats_.total_records = redo_log_files_.size();

    return redo_log_files_;
}

inline std::optional<RedoLogRollforwardScanner::CheckpointInfo> 
RedoLogRollforwardScanner::get_checkpoint(const std::string& redo_log_path) {
    std::ifstream file(redo_log_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 16384) {
        return std::nullopt;
    }

    std::vector<uint8_t> page_data(16384);
    file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

    // 检查点 1 位于偏移 76-115
    constexpr size_t CHECKPOINT1_OFFSET = 76;
    constexpr size_t CHECKPOINT2_OFFSET = 136;

    uint32_t cp1_lsn_low = read_le<uint32_t>(page_data.data(), CHECKPOINT1_OFFSET);
    uint32_t cp1_lsn_high = read_le<uint32_t>(page_data.data(), CHECKPOINT1_OFFSET + 4);
    uint32_t cp1_page_no = read_le<uint32_t>(page_data.data(), CHECKPOINT1_OFFSET + 8);

    uint32_t cp2_lsn_low = read_le<uint32_t>(page_data.data(), CHECKPOINT2_OFFSET);
    uint32_t cp2_lsn_high = read_le<uint32_t>(page_data.data(), CHECKPOINT2_OFFSET + 4);
    uint32_t cp2_page_no = read_le<uint32_t>(page_data.data(), CHECKPOINT2_OFFSET + 8);

    // 选择较新的检查点
    uint64_t lsn1 = (static_cast<uint64_t>(cp1_lsn_high) << 32) | cp1_lsn_low;
    uint64_t lsn2 = (static_cast<uint64_t>(cp2_lsn_high) << 32) | cp2_lsn_low;

    CheckpointInfo checkpoint;
    if (lsn1 >= lsn2) {
        checkpoint.lsn = lsn1;
        checkpoint.page_no = cp1_page_no;
    } else {
        checkpoint.lsn = lsn2;
        checkpoint.page_no = cp2_page_no;
    }

    return checkpoint;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_REDO_LOG_ROLLFORWARD_HPP
