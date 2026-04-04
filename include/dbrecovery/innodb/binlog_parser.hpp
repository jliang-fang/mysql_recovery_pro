#ifndef DBRECOVERY_INNODB_BINLOG_PARSER_HPP
#define DBRECOVERY_INNODB_BINLOG_PARSER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <memory>
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Binlog 事件类型 ====================
enum class BinlogEventType : uint8_t {
    UNKNOWN = 0,
    QUERY_EVENT = 2,
    TABLE_MAP_EVENT = 33,
    WRITE_ROWS_EVENT = 30,
    UPDATE_ROWS_EVENT = 31,
    DELETE_ROWS_EVENT = 32,
    XID_EVENT = 16,
    ROTATE_EVENT = 4,
    FORMAT_DESCRIPTION_EVENT = 15,
    INTVAR_EVENT = 5,
    GTID_EVENT = 34,
    BEGIN_LOAD_QUERY_EVENT = 23,
    EXECUTE_LOAD_QUERY_EVENT = 24,
    TRANSACTION_CONTEXT_EVENT = 36,
    VIEW_CHANGE_EVENT = 37,
    XA_PREPARE_EVENT = 38,
    ANNOTATE_ROWS_EVENT = 35,
    BINLOG_CHECKPOINT_EVENT = 39,
    GTID_LIST_EVENT = 40,
    START_ENCRYPTION_EVENT = 41,
    ROWS_QUERY_EVENT = 25,
    PRE_GA_WRITE_ROWS_EVENT = 21,
    PRE_GA_UPDATE_ROWS_EVENT = 22,
    PRE_GA_DELETE_ROWS_EVENT = 23,
};

// ==================== Binlog 事件头 ====================
struct BinlogEventHeader {
    uint32_t timestamp;
    BinlogEventType type_code;
    uint32_t server_id;
    uint32_t event_length;
    uint32_t next_position;
    uint16_t flags;
    uint64_t log_pos;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "BinlogEventHeader{ts=" << timestamp
            << ", type=" << static_cast<int>(type_code)
            << ", server=" << server_id
            << ", len=" << event_length
            << ", pos=" << log_pos << "}";
        return oss.str();
    }
};

// ==================== 查询事件 ====================
struct QueryEvent {
    uint32_t thread_id;
    uint32_t exec_time;
    uint8_t db_len;
    uint16_t error_code;
    uint16_t status_vars_len;
    std::string database;
    std::string query;

    std::string to_string() const {
        return "QueryEvent{db=" + database + ", query=" + query + "}";
    }
};

// ==================== 表映射事件 ====================
struct TableMapEvent {
    uint64_t table_id;
    uint16_t flags;
    std::string database;
    std::string table;
    std::vector<uint8_t> column_types;
    std::vector<uint8_t> column_meta;

    std::string to_string() const {
        return "TableMapEvent{table=" + database + "." + table + "}";
    }
};

// ==================== 行事件 ====================
struct RowEvent {
    uint64_t table_id;
    uint16_t flags;
    std::vector<std::map<uint16_t, std::optional<std::vector<uint8_t>>>> before_rows;
    std::vector<std::map<uint16_t, std::optional<std::vector<uint8_t>>>> after_rows;

    std::string to_string() const {
        return "RowEvent{table_id=" + std::to_string(table_id) +
               ", before=" + std::to_string(before_rows.size()) +
               ", after=" + std::to_string(after_rows.size()) + "}";
    }
};

// ==================== XID 事件 ====================
struct XidEvent {
    uint64_t xid;

    std::string to_string() const {
        return "XidEvent{xid=" + std::to_string(xid) + "}";
    }
};

// ==================== Binlog 事件 ====================
struct BinlogEvent {
    BinlogEventHeader header;
    std::variant<
        QueryEvent,
        TableMapEvent,
        RowEvent,
        XidEvent
    > event;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "BinlogEvent{" << header.to_string();
        
        std::visit([&oss](auto&& e) {
            oss << ", " << e.to_string();
        }, event);
        
        oss << "}";
        return oss.str();
    }
};

// ==================== Binlog 解析器 ====================
class BinlogParser {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit BinlogParser(size_t page_size = 16384);

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 解析 Binlog 文件
     * @return 解析的事件
     */
    std::vector<BinlogEvent> parse();

    /**
     * @brief 解析单个事件
     */
    std::optional<BinlogEvent> parse_event(const uint8_t* data, size_t size);

    /**
     * @brief 获取表映射
     */
    const std::map<uint64_t, TableMapEvent>& get_table_map() const { return table_map_; }

    /**
     * @brief 获取统计信息
     */
    struct Stats {
        size_t total_events = 0;
        size_t query_events = 0;
        size_t table_map_events = 0;
        size_t row_events = 0;
        size_t xid_events = 0;
        size_t parse_errors = 0;
        double parse_time_ms = 0.0;

        std::string to_string() const {
            std::ostringstream oss;
            oss << "BinlogParserStats{events=" << total_events
                << ", query=" << query_events
                << ", table_map=" << table_map_events
                << ", rows=" << row_events
                << ", xid=" << xid_events << "}";
            return oss.str();
        }
    };

    Stats get_stats() const { return stats_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::filesystem::path file_path_;
    std::vector<BinlogEvent> events_;
    std::map<uint64_t, TableMapEvent> table_map_;
    Stats stats_;

    // 解析事件头
    std::optional<BinlogEventHeader> parse_header(const uint8_t* data, size_t size);

    // 解析查询事件
    std::optional<QueryEvent> parse_query_event(const uint8_t* data, size_t size);

    // 解析表映射事件
    std::optional<TableMapEvent> parse_table_map_event(const uint8_t* data, size_t size);

    // 解析行事件
    std::optional<RowEvent> parse_row_event(const uint8_t* data, size_t size, BinlogEventType type);

    // 解析 XID 事件
    std::optional<XidEvent> parse_xid_event(const uint8_t* data, size_t size);
};

// ==================== Binlog 分析器 ====================
class BinlogAnalyzer {
public:
    /**
     * @brief 构造函数
     */
    BinlogAnalyzer();

    /**
     * @brief 设置 Binlog 文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 分析 Binlog
     * @return 分析结果
     */
    struct AnalysisResult {
        uint64_t start_lsn = 0;
        uint64_t end_lsn = 0;
        size_t total_events = 0;
        size_t transactions = 0;
        size_t inserts = 0;
        size_t updates = 0;
        size_t deletes = 0;
        std::map<std::string, size_t> table_stats;
        std::vector<uint64_t> xid_list;

        std::string to_string() const {
            std::ostringstream oss;
            oss << "BinlogAnalysis{events=" << total_events
                << ", txns=" << transactions
                << ", inserts=" << inserts
                << ", updates=" << updates
                << ", deletes=" << deletes << "}";
            return oss.str();
        }
    };

    AnalysisResult analyze();

    /**
     * @brief 获取事务列表
     */
    const std::vector<uint64_t>& get_transactions() const { return xid_list_; }

    /**
     * @brief 获取表统计
     */
    const std::map<std::string, size_t>& get_table_stats() const { return table_stats_; }

private:
    std::filesystem::path file_path_;
    std::vector<uint64_t> xid_list_;
    std::map<std::string, size_t> table_stats_;
    AnalysisResult result_;

    void update_stats(const BinlogEvent& event);
};

// ==================== 增量恢复信息 ====================
struct IncrementalRecoveryInfo {
    uint64_t start_lsn;
    uint64_t end_lsn;
    std::vector<uint64_t> xid_list;
    std::map<uint64_t, std::vector<std::map<std::string, std::string>>> table_data;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "IncrementalRecoveryInfo{lsn=" << start_lsn << "-" << end_lsn
            << ", txns=" << xid_list.size()
            << ", tables=" << table_data.size() << "}";
        return oss.str();
    }
};

// ==================== Binlog 恢复器 ====================
class BinlogRecovery {
public:
    /**
     * @brief 构造函数
     */
    BinlogRecovery();

    /**
     * @brief 设置 Binlog 文件路径
     */
    void set_binlog_path(const std::filesystem::path& file_path);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置恢复范围
     */
    void set_recovery_range(uint64_t start_lsn, uint64_t end_lsn);

    /**
     * @brief 执行恢复
     * @return 恢复的数据
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 获取增量恢复信息
     */
    const IncrementalRecoveryInfo& get_info() const { return info_; }

    /**
     * @brief 获取统计信息
     */
    const BinlogParser::Stats& get_stats() const { return parser_.get_stats(); }

private:
    std::filesystem::path binlog_path_;
    catalog::TableSchema schema_;
    uint64_t start_lsn_ = 0;
    uint64_t end_lsn_ = 0;
    BinlogParser parser_;
    IncrementalRecoveryInfo info_;
    std::map<uint64_t, std::vector<std::map<std::string, std::string>>> table_data_;

    void process_event(const BinlogEvent& event);
};

// ==================== Binlog 解析器实现 ====================

inline BinlogParser::BinlogParser(size_t page_size)
    : page_size_(page_size) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void BinlogParser::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

inline void BinlogParser::clear() {
    events_.clear();
    table_map_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

// ==================== Binlog 分析器实现 ====================

inline BinlogAnalyzer::BinlogAnalyzer() {
    std::memset(&result_, 0, sizeof(result_));
}

inline void BinlogAnalyzer::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

// ==================== Binlog 恢复器实现 ====================

inline BinlogRecovery::BinlogRecovery() {
}

inline void BinlogRecovery::set_binlog_path(const std::filesystem::path& file_path) {
    binlog_path_ = file_path;
    parser_.set_file_path(file_path);
}

inline void BinlogRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void BinlogRecovery::set_recovery_range(uint64_t start_lsn, uint64_t end_lsn) {
    start_lsn_ = start_lsn;
    end_lsn_ = end_lsn;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BINLOG_PARSER_HPP
