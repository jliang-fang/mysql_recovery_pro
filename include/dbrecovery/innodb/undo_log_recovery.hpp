#ifndef DBRECOVERY_INNODB_UNDO_LOG_RECOVERY_HPP
#define DBRECOVERY_INNODB_UNDO_LOG_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <deque>
#include <set>
#include <algorithm>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief Undo Log 记录类型
 */
enum class UndoLogType : uint8_t {
    INSERT = 0x01,      // INSERT 记录
    UPDATE = 0x02,      // UPDATE 记录
    DELETE = 0x03,      // DELETE 记录
    TRUNCATE = 0x04,    // TRUNCATE 记录
    XA = 0x10,          // XA 事务记录
};

/**
 * @brief Undo Log 记录
 */
struct UndoLogRecord {
    uint64_t lsn;
    uint32_t space_id;
    uint32_t page_no;
    uint32_t offset;
    UndoLogType type;
    uint32_t trx_id;
    uint32_t roll_ptr;
    
    // 数据
    std::vector<uint8_t> data;
    
    // 事务信息
    uint32_t next_record;
    uint32_t prev_record;
    
    // 解析后的信息
    std::map<uint32_t, std::vector<uint8_t>> field_values;
    bool is_deleted;
    bool is_insert;
    bool is_update;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "UndoLogRecord{lsn=" << lsn
            << ", type=" << static_cast<int>(type)
            << ", trx=" << trx_id
            << ", page=" << page_no << ":" << offset;
        if (is_deleted) oss << ", deleted";
        if (is_insert) oss << ", insert";
        if (is_update) oss << ", update";
        oss << "}";
        return oss.str();
    }
};

/**
 * @brief Undo Log 页
 */
struct UndoLogPage {
    uint32_t page_no;
    uint32_t space_id;
    uint64_t lsn;
    uint32_t first_record;
    uint32_t last_record;
    uint32_t next_page;
    uint32_t prev_page;
    bool is_header;
    bool is_full;
    
    std::vector<UndoLogRecord> records;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "UndoLogPage{page=" << page_no
            << ", records=" << records.size()
            << ", next=" << next_page
            << ", prev=" << prev_page << "}";
        return oss.str();
    }
};

/**
 * @brief Undo Log 链
 */
struct UndoLogChain {
    uint32_t space_id;
    uint32_t start_page_no;
    uint64_t start_lsn;
    uint64_t end_lsn;
    std::vector<uint32_t> page_nos;
    std::vector<UndoLogRecord> records;
    uint32_t trx_id;
    bool is_active;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "UndoLogChain{trx=" << trx_id
            << ", pages=" << page_nos.size()
            << ", records=" << records.size()
            << ", lsn=" << start_lsn << "-" << end_lsn;
        if (is_active) oss << ", active";
        oss << "}";
        return oss.str();
    }
};

/**
 * @brief Undo Log 恢复统计
 */
struct UndoLogRecoveryStats {
    size_t total_pages = 0;
    size_t undo_pages = 0;
    size_t records_parsed = 0;
    size_t chains_found = 0;
    size_t records_applied = 0;
    size_t conflicts = 0;
    double scan_time_ms = 0.0;
    double recovery_time_ms = 0.0;
    double total_time_ms = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "UndoLogRecoveryStats{pages=" << total_pages
            << ", undo=" << undo_pages
            << ", records=" << records_parsed
            << ", chains=" << chains_found
            << ", applied=" << records_applied
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

/**
 * @brief Undo Log 恢复器
 */
class UndoLogRecovery {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit UndoLogRecovery(size_t page_size = 16384);

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置恢复的 LSN 范围
     */
    void set_lsn_range(uint64_t start_lsn, uint64_t end_lsn);

    /**
     * @brief 设置是否应用回滚
     */
    void set_apply_rollback(bool apply);

    /**
     * @brief 执行 Undo Log 恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 获取统计信息
     */
    const UndoLogRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取 Undo Log 页
     */
    const std::vector<UndoLogPage>& get_pages() const { return pages_; }

    /**
     * @brief 获取 Undo Log 链
     */
    const std::vector<UndoLogChain>& get_chains() const { return chains_; }

    /**
     * @brief 获取解析的记录
     */
    const std::vector<UndoLogRecord>& get_records() const { return records_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;
    uint64_t start_lsn_ = 0;
    uint64_t end_lsn_ = 0;
    bool apply_rollback_ = true;

    std::vector<UndoLogPage> pages_;
    std::vector<UndoLogChain> chains_;
    std::vector<UndoLogRecord> records_;
    std::vector<std::map<std::string, std::string>> recovered_records_;
    UndoLogRecoveryStats stats_;

    // 扫描 Undo Log 页
    std::vector<UndoLogPage> scan_undo_pages();

    // 解析 Undo Log 记录
    std::vector<UndoLogRecord> parse_records(const UndoLogPage& page);

    // 构建 Undo Log 链
    std::vector<UndoLogChain> build_chains();

    // 应用回滚
    std::vector<std::map<std::string, std::string>> apply_rollback(
        const std::vector<UndoLogChain>& chains);

    // 解析单个 Undo Log 记录
    std::optional<UndoLogRecord> parse_record(const uint8_t* data, size_t size,
                                              uint64_t lsn, uint32_t page_no);
};

// ==================== UndoLogRecovery 实现 ====================

inline UndoLogRecovery::UndoLogRecovery(size_t page_size)
    : page_size_(page_size), apply_rollback_(true) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void UndoLogRecovery::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

inline void UndoLogRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void UndoLogRecovery::set_lsn_range(uint64_t start_lsn, uint64_t end_lsn) {
    start_lsn_ = start_lsn;
    end_lsn_ = end_lsn;
}

inline void UndoLogRecovery::set_apply_rollback(bool apply) {
    apply_rollback_ = apply;
}

inline void UndoLogRecovery::clear() {
    pages_.clear();
    chains_.clear();
    records_.clear();
    recovered_records_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline std::vector<UndoLogPage> UndoLogRecovery::scan_undo_pages() {
    auto start = std::chrono::high_resolution_clock::now();

    pages_.clear();

    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return pages_;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        return pages_;
    }

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
    std::vector<uint8_t> page_data(page_size_);

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

        // 检查页类型 - 使用 FIL_PAGE_TYPE_ABS (24) 而不是偏移 16
        // InnoDB FIL header fields are stored in BIG-ENDIAN format
        uint16_t page_type = read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);
        if (page_type != FIL_PAGE_UNDO_LOG) {
            continue;
        }

        UndoLogPage page;
        page.page_no = page_no;
        page.space_id = read_be<uint32_t>(page_data.data(), 0);  // FIL_PAGE_SPACE_ID = 0
        page.lsn = read_be<uint64_t>(page_data.data(), 16);       // FIL_PAGE_LSN = 16
        page.first_record = read_le<uint32_t>(page_data.data(), 38 + 8);
        page.last_record = read_le<uint32_t>(page_data.data(), 38 + 12);
        page.next_page = read_le<uint32_t>(page_data.data(), 38 + 16);
        page.prev_page = read_le<uint32_t>(page_data.data(), 38 + 20);
        page.is_header = (page_no == 0);
        page.is_full = false;

        // 解析记录
        page.records = parse_records(page);

        pages_.push_back(page);
    }

    stats_.undo_pages = pages_.size();
    stats_.total_pages = total_pages;

    auto end = std::chrono::high_resolution_clock::now();
    stats_.scan_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return pages_;
}

inline std::vector<UndoLogRecord> UndoLogRecovery::parse_records(const UndoLogPage& page) {
    std::vector<UndoLogRecord> records;

    if (page.page_data.empty()) {
        return records;
    }

    // 从页中读取记录
    // Undo Log 记录格式:
    // - 2 bytes: next record offset
    // - 2 bytes: type
    // - 4 bytes: trx id
    // - 8 bytes: roll ptr
    // - variable: data

    size_t offset = 38 + 16;  // 跳过页头
    while (offset < page.page_data.size()) {
        if (offset + 14 > page.page_data.size()) {
            break;
        }

        uint16_t next_offset = read_le<uint16_t>(page.page_data.data(), offset);
        uint16_t type = read_le<uint16_t>(page.page_data.data(), offset + 2);
        uint32_t trx_id = read_le<uint32_t>(page.page_data.data(), offset + 4);
        uint64_t roll_ptr = read_le<uint64_t>(page.page_data.data(), offset + 8);

        UndoLogRecord record;
        record.lsn = page.lsn;
        record.space_id = page.space_id;
        record.page_no = page.page_no;
        record.offset = static_cast<uint32_t>(offset);
        record.type = static_cast<UndoLogType>(type);
        record.trx_id = trx_id;
        record.roll_ptr = static_cast<uint32_t>(roll_ptr & 0xFFFFFFFF);
        record.next_record = next_offset;
        record.prev_record = 0;
        record.is_deleted = (type == 0x03);
        record.is_insert = (type == 0x01);
        record.is_update = (type == 0x02);

        // 读取数据
        size_t data_size = next_offset - offset - 14;
        if (data_size > 0 && offset + 14 + data_size <= page.page_data.size()) {
            record.data.assign(page.page_data.begin() + offset + 14,
                              page.page_data.begin() + offset + 14 + data_size);
        }

        records.push_back(record);

        if (next_offset == 0 || next_offset <= offset || next_offset >= page.page_data.size()) {
            break;
        }

        offset = next_offset;
    }

    return records;
}

inline std::vector<UndoLogChain> UndoLogRecovery::build_chains() {
    auto start = std::chrono::high_resolution_clock::now();

    chains_.clear();

    // 按事务ID分组记录
    std::map<uint32_t, std::vector<UndoLogRecord>> records_by_trx;

    for (const auto& page : pages_) {
        for (const auto& record : page.records) {
            records_by_trx[record.trx_id].push_back(record);
        }
    }

    // 构建链
    for (auto& [trx_id, trx_records] : records_by_trx) {
        // 按 LSN 排序
        std::sort(trx_records.begin(), trx_records.end(),
            [](const UndoLogRecord& a, const UndoLogRecord& b) {
                return a.lsn < b.lsn;
            });

        UndoLogChain chain;
        chain.trx_id = trx_id;
        chain.space_id = trx_records.front().space_id;
        chain.start_lsn = trx_records.front().lsn;
        chain.end_lsn = trx_records.back().lsn;
        chain.is_active = false;

        // 收集页号
        std::set<uint32_t> page_nos;
        for (const auto& record : trx_records) {
            page_nos.insert(record.page_no);
        }
        chain.page_nos.assign(page_nos.begin(), page_nos.end());

        chain.records = std::move(trx_records);
        chains_.push_back(chain);
    }

    stats_.chains_found = chains_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return chains_;
}

inline std::vector<std::map<std::string, std::string>> UndoLogRecovery::apply_rollback(
    const std::vector<UndoLogChain>& chains) {

    auto start = std::chrono::high_resolution_clock::now();

    recovered_records_.clear();

    for (const auto& chain : chains) {
        // Walk records in reverse order (newest-first = rollback direction)
        for (auto it = chain.records.rbegin(); it != chain.records.rend(); ++it) {
            const auto& record = *it;

            switch (record.type) {
                case UndoLogType::INSERT: {
                    // INSERT rollback = DELETE — record the PK so we know what was removed
                    std::map<std::string, std::string> row;
                    row["_undo_action"] = "DELETE";
                    row["_trx_id"] = std::to_string(record.trx_id);
                    row["_page_no"] = std::to_string(record.page_no);
                    for (const auto& [field_no, value] : record.field_values) {
                        row["col_" + std::to_string(field_no)] =
                            std::string(value.begin(), value.end());
                    }
                    recovered_records_.push_back(row);
                    break;
                }
                case UndoLogType::UPDATE: {
                    // UPDATE rollback — restore old field values
                    std::map<std::string, std::string> row;
                    row["_undo_action"] = "RESTORE";
                    row["_trx_id"] = std::to_string(record.trx_id);
                    row["_page_no"] = std::to_string(record.page_no);
                    for (const auto& [field_no, value] : record.field_values) {
                        row["col_" + std::to_string(field_no)] =
                            std::string(value.begin(), value.end());
                    }
                    recovered_records_.push_back(row);
                    break;
                }
                case UndoLogType::DELETE: {
                    // DELETE rollback = INSERT — restore the deleted row
                    std::map<std::string, std::string> row;
                    row["_undo_action"] = "INSERT";
                    row["_trx_id"] = std::to_string(record.trx_id);
                    row["_page_no"] = std::to_string(record.page_no);
                    for (const auto& [field_no, value] : record.field_values) {
                        row["col_" + std::to_string(field_no)] =
                            std::string(value.begin(), value.end());
                    }
                    recovered_records_.push_back(row);
                    break;
                }
                case UndoLogType::TRUNCATE: {
                    // TRUNCATE rollback — restore all pre-truncate data from undo
                    std::map<std::string, std::string> row;
                    row["_undo_action"] = "TRUNCATE_ROLLBACK";
                    row["_trx_id"] = std::to_string(record.trx_id);
                    row["_page_no"] = std::to_string(record.page_no);
                    row["_space_id"] = std::to_string(record.space_id);
                    for (const auto& [field_no, value] : record.field_values) {
                        row["col_" + std::to_string(field_no)] =
                            std::string(value.begin(), value.end());
                    }
                    recovered_records_.push_back(row);
                    break;
                }
                default:
                    break;
            }
        }
    }

    stats_.records_applied = recovered_records_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms += std::chrono::duration<double, std::milli>(end - start).count();

    return recovered_records_;
}

inline std::optional<UndoLogRecord> UndoLogRecovery::parse_record(const uint8_t* data,
                                                                   size_t size,
                                                                   uint64_t lsn,
                                                                   uint32_t page_no) {
    if (size < 14) {
        return std::nullopt;
    }

    UndoLogRecord record;
    record.lsn = lsn;
    record.page_no = page_no;
    record.offset = 0;
    record.type = static_cast<UndoLogType>(read_le<uint16_t>(data, 2));
    record.trx_id = read_le<uint32_t>(data, 4);
    record.roll_ptr = read_le<uint32_t>(data, 8);

    // 读取数据
    if (size > 14) {
        record.data.assign(data + 14, data + size);
    }

    record.is_deleted = (record.type == UndoLogType::DELETE);
    record.is_insert = (record.type == UndoLogType::INSERT);
    record.is_update = (record.type == UndoLogType::UPDATE);

    return record;
}

inline std::vector<std::map<std::string, std::string>> UndoLogRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 扫描 Undo Log 页
    auto undo_pages = scan_undo_pages();
    stats_.records_parsed = 0;

    // 解析所有记录
    for (const auto& page : undo_pages) {
        records_.insert(records_.end(), page.records.begin(), page.records.end());
        stats_.records_parsed += page.records.size();
    }

    // 构建链
    auto chains = build_chains();

    // 应用回滚
    if (apply_rollback_) {
        recovered_records_ = apply_rollback(chains);
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return recovered_records_;
}

// ==================== Undo Log 扫描器 ====================
class UndoLogScanner {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit UndoLogScanner(size_t page_size = 16384);

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 扫描 Undo Log
     * @return Undo Log 页列表
     */
    std::vector<UndoLogPage> scan();

    /**
     * @brief 获取统计信息
     */
    const UndoLogRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::string data_dir_;
    UndoLogRecoveryStats stats_;
    std::vector<UndoLogPage> pages_;

    void scan_directory(const std::string& dir, int depth = 0);
};

// ==================== UndoLogScanner 实现 ====================

inline UndoLogScanner::UndoLogScanner(size_t page_size)
    : page_size_(page_size) {}

inline void UndoLogScanner::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

inline void UndoLogScanner::clear() {
    pages_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void UndoLogScanner::scan_directory(const std::string& dir, int depth) {
    if (depth > 10) {
        return;
    }

    std::filesystem::path directory(dir);

    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        return;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
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
                        if (page_type != FIL_PAGE_UNDO_LOG) {
                            continue;
                        }

                        UndoLogPage page;
                        page.page_no = page_no;
                        page.space_id = read_le<uint32_t>(page_data.data(), 24);
                        page.lsn = read_le<uint64_t>(page_data.data(), 8);
                        page.next_page = read_le<uint32_t>(page_data.data(), 38 + 16);
                        page.prev_page = read_le<uint32_t>(page_data.data(), 38 + 20);
                        page.is_header = (page_no == 0);

                        pages_.push_back(page);
                    }
                }
            } else if (entry.is_directory()) {
                std::string dirname = entry.path().filename().string();
                if (dirname != "." && dirname != ".." &&
                    dirname != "mysql" && dirname != "sys" &&
                    dirname != "performance_schema" && dirname != "information_schema") {
                    scan_directory(entry.path().string(), depth + 1);
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // 忽略错误
    }
}

inline std::vector<UndoLogPage> UndoLogScanner::scan() {
    clear();

    if (data_dir_.empty()) {
        return pages_;
    }

    scan_directory(data_dir_);

    stats_.undo_pages = pages_.size();
    stats_.total_pages = pages_.size();

    return pages_;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_UNDO_LOG_RECOVERY_HPP
