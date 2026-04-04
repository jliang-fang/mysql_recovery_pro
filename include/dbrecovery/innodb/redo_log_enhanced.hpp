#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include <map>
#include <set>
#include <functional>
#include <sstream>
#include <cstring>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/redo_log.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== 增强的 Redo Log 类型 ====================
// MySQL 8.0+ Redo Log 类型扩展
constexpr uint8_t MLOG_DB_ALLOC = 53;           // 分配新页
constexpr uint8_t MLOG_FILE_CREATE = 54;        // 创建文件
constexpr uint8_t MLOG_FILE_RENAME = 55;        // 重命名文件
constexpr uint8_t MLOG_FILE_DELETE = 56;        // 删除文件
constexpr uint8_t MLOG_COMP_REC_MIN_MARK = 35;  // 最小记录标记
constexpr uint8_t MLOG_INDEX_CREATE = 57;       // 创建索引
// ✅ P3-1 修复：移除重复的 MLOG_INDEX_LOAD 定义（已在 redo_log.hpp 中定义为 52）
// constexpr uint8_t MLOG_INDEX_LOAD = 58;       // 加载索引
constexpr uint8_t MLOG_TABLE = 59;              // 表操作
constexpr uint8_t MLOG_TRUNCATE = 60;           // 截断表

// ==================== 增强的 Redo Log 记录结构 ====================
struct EnhancedRedoLogRecord : public RedoLogRecord {
    // 扩展字段
    uint8_t r_n_bits = 0;             // 位数（用于位图）
    std::vector<uint8_t> r_bitmap;    // 位图数据

    // 记录详细信息
    uint16_t r_rec_type = 0;          // 记录类型
    uint16_t r_n_fields = 0;          // 字段数量
    uint32_t r_trx_id_offset = 0;     // trx_id 在数据中的偏移

    // 页操作信息
    uint16_t r_free = 0;              // 空闲空间
    uint16_t r_garbage_end = 0;       // 垃圾结束位置

    // BLOB 信息
    uint32_t r_blob_page_no = 0;      // BLOB 页号
    uint32_t r_blob_offset = 0;       // BLOB 偏移

    // 事务信息
    uint64_t r_next_trx_id = 0;       // 下一个事务ID

    std::string to_string() const {
        std::ostringstream oss;
        oss << "EnhancedRedoLogRecord{type=" << r_type
            << ", lsn=" << r_lsn
            << ", space=" << r_space_id
            << ", page=" << r_page_no
            << ", trx_id=" << r_trx_id
            << ", data_len=" << r_data.size() << "}";
        return oss.str();
    }
};

// ==================== Redo Log 回放操作类型 ====================
enum class RedoOperationType {
    INSERT,           // 插入记录
    UPDATE,           // 更新记录
    DELETE,           // 删除记录
    PAGE_CREATE,      // 创建页
    PAGE_REINIT,      // 重初始化页
    PAGE_FREE,        // 释放页
    PAGE_ALLOC,       // 分配页
    BLOB_WRITE,       // BLOB 写入
    INDEX_CREATE,     // 创建索引
    INDEX_DROP,       // 删除索引
    TABLE_TRUNCATE,   // 截断表
    FILE_CREATE,      // 创建文件
    FILE_DELETE,      // 删除文件
    CHECKPOINT,       // 检查点
    UNKNOWN
};

// ==================== Redo Log 回放操作 ====================
struct RedoOperation {
    RedoOperationType type;
    uint64_t lsn;
    uint32_t space_id;
    uint32_t page_no;
    
    // 操作数据
    std::vector<uint8_t> data;
    std::map<uint16_t, std::vector<uint8_t>> field_updates;  // 字段更新
    
    // 事务信息
    uint64_t trx_id = 0;
    bool is_committed = false;
    
    // 页操作信息
    uint16_t new_page_no = 0;  // 新页号（用于 PAGE_ALLOC）
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "RedoOperation{type=" << static_cast<int>(type)
            << ", lsn=" << lsn
            << ", space=" << space_id
            << ", page=" << page_no;
        if (trx_id > 0) oss << ", trx=" << trx_id;
        oss << "}";
        return oss.str();
    }
};

// ==================== Redo Log 回放器 ====================
class RedoLogReplayer {
public:
    RedoLogReplayer();
    ~RedoLogReplayer();

    /**
     * @brief 设置页数据
     */
    void set_page_data(uint32_t page_no, std::vector<uint8_t> page_data);

    /**
     * @brief 添加 Redo Log 记录
     */
    void add_record(const EnhancedRedoLogRecord& record);

    /**
     * @brief 添加 Redo Log 记录（带 LSN）
     */
    void add_record_with_lsn(const EnhancedRedoLogRecord& record, uint64_t lsn);

    /**
     * @brief 按 LSN 顺序回放
     */
    bool replay_by_lsn();

    /**
     * @brief 按页号回放
     */
    bool replay_by_page();

    /**
     * @brief 获取回放操作
     */
    const std::vector<RedoOperation>& get_operations() const { return operations_; }

    /**
     * @brief 获取页数据
     */
    std::optional<std::vector<uint8_t>> get_page_data(uint32_t page_no) const;

    /**
     * @brief 获取统计信息
     */
    struct Stats {
        size_t total_records = 0;
        size_t applied_records = 0;
        size_t failed_records = 0;
        size_t operations = 0;
        std::map<RedoOperationType, size_t> op_counts;
    };
    Stats get_stats() const { return stats_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    std::map<uint32_t, std::vector<uint8_t>> pages_;
    std::vector<EnhancedRedoLogRecord> records_;
    std::vector<RedoOperation> operations_;
    Stats stats_;

    // 解析 Redo Log 数据
    std::optional<RedoOperation> parse_operation(const EnhancedRedoLogRecord& record, uint64_t lsn);

    // 应用操作到页
    bool apply_operation(RedoOperation& op);

    // 解析记录插入操作
    std::optional<RedoOperation> parse_rec_insert(const EnhancedRedoLogRecord& record, uint64_t lsn);

    // 解析记录更新操作
    std::optional<RedoOperation> parse_rec_update(const EnhancedRedoLogRecord& record, uint64_t lsn);

    // 解析记录删除操作
    std::optional<RedoOperation> parse_rec_delete(const EnhancedRedoLogRecord& record, uint64_t lsn);

    // 解析页创建操作
    std::optional<RedoOperation> parse_page_create(const EnhancedRedoLogRecord& record, uint64_t lsn);

    // 解析页重初始化操作
    std::optional<RedoOperation> parse_page_reinit(const EnhancedRedoLogRecord& record, uint64_t lsn);

    // 解析 BLOB 写入操作
    std::optional<RedoOperation> parse_blob_write(const EnhancedRedoLogRecord& record, uint64_t lsn);

    // 解析检查点操作
    std::optional<RedoOperation> parse_checkpoint(const EnhancedRedoLogRecord& record, uint64_t lsn);
};

// ==================== Redo Log 分析器 ====================
class RedoLogAnalyzer {
public:
    RedoLogAnalyzer();
    ~RedoLogAnalyzer();

    /**
     * @brief 分析 Redo Log 文件
     */
    bool analyze_file(const std::string& path);

    /**
     * @brief 分析 Redo Log 记录
     */
    bool analyze_records(const std::vector<RedoLogRecord>& records);

    /**
     * @brief 获取事务信息
     */
    struct TransactionInfo {
        uint64_t trx_id;
        uint64_t start_lsn;
        uint64_t end_lsn;
        size_t record_count;
        bool is_committed;
        std::set<uint32_t> modified_spaces;
        std::set<uint32_t> modified_pages;
    };

    const std::map<uint64_t, TransactionInfo>& get_transactions() const { return transactions_; }

    /**
     * @brief 获取修改的表空间
     */
    const std::set<uint32_t>& get_modified_spaces() const { return modified_spaces_; }

    /**
     * @brief 获取修改的页
     */
    const std::set<uint32_t>& get_modified_pages() const { return modified_pages_; }

    /**
     * @brief 获取统计信息
     */
    struct AnalyzeStats {
        size_t total_records = 0;
        size_t unique_transactions = 0;
        size_t unique_spaces = 0;
        size_t unique_pages = 0;
        std::map<uint8_t, size_t> record_type_counts;
    };
    AnalyzeStats get_stats() const { return stats_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    std::map<uint64_t, TransactionInfo> transactions_;
    std::set<uint32_t> modified_spaces_;
    std::set<uint32_t> modified_pages_;
    AnalyzeStats stats_;

    void update_transaction_info(uint64_t trx_id, uint64_t lsn, uint32_t space_id, uint32_t page_no, bool is_commit);
};

// ==================== LSN 工具函数 ====================
inline uint64_t make_lsn(uint32_t file_no, uint32_t offset) {
    return (static_cast<uint64_t>(file_no) << 32) | offset;
}

inline uint32_t get_lsn_file_no(uint64_t lsn) {
    return static_cast<uint32_t>(lsn >> 32);
}

inline uint32_t get_lsn_offset(uint64_t lsn) {
    return static_cast<uint32_t>(lsn & 0xFFFFFFFF);
}

// ==================== Redo Log 回放器实现 ====================

inline RedoLogReplayer::RedoLogReplayer() {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline RedoLogReplayer::~RedoLogReplayer() = default;

inline void RedoLogReplayer::set_page_data(uint32_t page_no, std::vector<uint8_t> page_data) {
    pages_[page_no] = std::move(page_data);
}

inline void RedoLogReplayer::add_record(const EnhancedRedoLogRecord& record) {
    records_.push_back(record);
    stats_.total_records++;
}

inline void RedoLogReplayer::add_record_with_lsn(const EnhancedRedoLogRecord& record, uint64_t lsn) {
    EnhancedRedoLogRecord rec = record;
    rec.r_lsn = lsn;
    records_.push_back(rec);
    stats_.total_records++;
}

inline std::optional<std::vector<uint8_t>> RedoLogReplayer::get_page_data(uint32_t page_no) const {
    auto it = pages_.find(page_no);
    if (it == pages_.end()) {
        return std::nullopt;
    }
    return it->second;
}

inline void RedoLogReplayer::clear() {
    pages_.clear();
    records_.clear();
    operations_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

// ==================== Redo Log 分析器实现 ====================

inline RedoLogAnalyzer::RedoLogAnalyzer() {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline RedoLogAnalyzer::~RedoLogAnalyzer() = default;

inline void RedoLogAnalyzer::clear() {
    transactions_.clear();
    modified_spaces_.clear();
    modified_pages_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void RedoLogAnalyzer::update_transaction_info(uint64_t trx_id, uint64_t lsn, 
                                                      uint32_t space_id, uint32_t page_no, 
                                                      bool is_commit) {
    auto& trx = transactions_[trx_id];
    if (trx.start_lsn == 0 || lsn < trx.start_lsn) {
        trx.start_lsn = lsn;
    }
    if (lsn > trx.end_lsn) {
        trx.end_lsn = lsn;
    }
    trx.record_count++;
    trx.modified_spaces.insert(space_id);
    trx.modified_pages.insert(page_no);
    if (is_commit) {
        trx.is_committed = true;
    }
}

} // namespace innodb
} // namespace dbrecovery
