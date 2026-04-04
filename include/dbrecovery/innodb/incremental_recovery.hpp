#ifndef DBRECOVERY_INNODB_INCREMENTAL_RECOVERY_HPP
#define DBRECOVERY_INNODB_INCREMENTAL_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <chrono>
#include <set>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/redo_log.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== 增量恢复范围 ====================
struct RecoveryRange {
    uint64_t start_lsn;
    uint64_t end_lsn;
    uint32_t start_page_no;
    uint32_t end_page_no;
    bool is_complete;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "RecoveryRange{lsn=" << start_lsn << "-" << end_lsn
            << ", pages=" << start_page_no << "-" << end_page_no << "}";
        return oss.str();
    }
};

// ==================== 增量恢复统计 ====================
struct IncrementalRecoveryStats {
    size_t total_pages = 0;
    size_t pages_in_range = 0;
    size_t records_recovered = 0;
    size_t records_skipped = 0;
    double scan_time_ms = 0.0;
    double recovery_time_ms = 0.0;
    double total_time_ms = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "IncrementalRecoveryStats{pages=" << total_pages
            << ", in_range=" << pages_in_range
            << ", records=" << records_recovered
            << ", skipped=" << records_skipped
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

// ==================== 增量恢复器 ====================
class IncrementalRecovery {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit IncrementalRecovery(size_t page_size = 16384);

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置恢复范围
     */
    void set_recovery_range(uint64_t start_lsn, uint64_t end_lsn);

    /**
     * @brief 设置页范围
     */
    void set_page_range(uint32_t start_page_no, uint32_t end_page_no);

    /**
     * @brief 执行增量恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 从 Redo Log 恢复
     * @param redo_log_path Redo Log 文件路径
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover_from_redo_log(
        const std::filesystem::path& redo_log_path);

    /**
     * @brief 获取恢复范围
     */
    const RecoveryRange& get_range() const { return range_; }

    /**
     * @brief 获取统计信息
     */
    const IncrementalRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取扫描的页
     */
    const std::vector<ScannedPage>& get_pages() const { return pages_; }

    /**
     * @brief 获取修改的页
     */
    const std::set<uint32_t>& get_modified_pages() const { return modified_pages_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;
    RecoveryRange range_;
    std::vector<ScannedPage> pages_;
    std::vector<std::map<std::string, std::string>> records_;
    std::set<uint32_t> modified_pages_;
    IncrementalRecoveryStats stats_;

    // 扫描页
    std::vector<ScannedPage> scan_pages();

    // 过滤页范围
    std::vector<ScannedPage> filter_pages_by_range(const std::vector<ScannedPage>& pages);

    // 解析页中的记录
    std::vector<std::map<std::string, std::string>> parse_page_records(
        const std::vector<ScannedPage>& pages);

    // 从 Redo Log 解析修改的页
    std::set<uint32_t> parse_modified_pages_from_redo_log(
        const std::filesystem::path& redo_log_path);
};

// ==================== 增量恢复器实现 ====================

inline IncrementalRecovery::IncrementalRecovery(size_t page_size)
    : page_size_(page_size) {
    range_.start_lsn = 0;
    range_.end_lsn = 0;
    range_.start_page_no = 0;
    range_.end_page_no = 0;
    range_.is_complete = false;
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void IncrementalRecovery::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

inline void IncrementalRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void IncrementalRecovery::set_recovery_range(uint64_t start_lsn, uint64_t end_lsn) {
    range_.start_lsn = start_lsn;
    range_.end_lsn = end_lsn;
}

inline void IncrementalRecovery::set_page_range(uint32_t start_page_no, uint32_t end_page_no) {
    range_.start_page_no = start_page_no;
    range_.end_page_no = end_page_no;
}

inline void IncrementalRecovery::clear() {
    pages_.clear();
    records_.clear();
    modified_pages_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

// ==================== 增量恢复器实现 ====================

inline std::vector<ScannedPage> IncrementalRecovery::scan_pages() {
    auto start = std::chrono::high_resolution_clock::now();

    pages_.clear();

    // 打开文件
    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return pages_;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 计算页数
    size_t total_pages = file_size / page_size_;
    stats_.total_pages = total_pages;

    if (total_pages == 0) {
        return pages_;
    }

    // 分配缓冲区
    std::vector<uint8_t> buffer(std::min(file_size, size_t(1024 * 1024)));

    // 扫描页
    size_t offset = 0;
    while (file.readsome(reinterpret_cast<char*>(buffer.data()), buffer.size())) {
        size_t read_size = file.gcount();

        for (size_t page_offset = 0; page_offset + page_size_ <= read_size; page_offset += page_size_) {
            const uint8_t* page_data = buffer.data() + page_offset;

            // 扫描单个页
            ScannedPage page{};
            page.offset = static_cast<uint32_t>(offset + page_offset);
            page.page_no = static_cast<uint32_t>(page_offset / page_size_);
            page.page_type = read_le<uint16_t>(page_data, 16);
            page.prev_page = read_le<uint32_t>(page_data, 0);
            page.next_page = read_le<uint32_t>(page_data, 4);
            page.lsn = read_le<uint64_t>(page_data, 8);
            page.space_id = read_le<uint32_t>(page_data, 24);
            page.is_valid = PageScanner::is_innodb_page(page_data, page_size_);

            pages_.push_back(page);
        }

        offset += read_size;
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.scan_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return pages_;
}

inline std::vector<ScannedPage> IncrementalRecovery::filter_pages_by_range(
    const std::vector<ScannedPage>& pages) {
    
    std::vector<ScannedPage> filtered;

    for (const auto& page : pages) {
        // 检查 LSN 范围
        if (range_.start_lsn > 0 && page.lsn < range_.start_lsn) {
            continue;
        }
        if (range_.end_lsn > 0 && page.lsn > range_.end_lsn) {
            continue;
        }

        // 检查页号范围
        if (range_.start_page_no > 0 && page.page_no < range_.start_page_no) {
            continue;
        }
        if (range_.end_page_no > 0 && page.page_no > range_.end_page_no) {
            continue;
        }

        filtered.push_back(page);
    }

    stats_.pages_in_range = filtered.size();
    return filtered;
}

inline std::vector<std::map<std::string, std::string>> IncrementalRecovery::parse_page_records(
    const std::vector<ScannedPage>& pages) {
    
    auto start = std::chrono::high_resolution_clock::now();

    records_.clear();

    for (const auto& page : pages) {
        if (!page.is_valid) {
            continue;
        }

        // 读取页数据
        std::ifstream file(file_path_, std::ios::binary);
        if (!file) continue;

        size_t offset = static_cast<size_t>(page.page_no) * page_size_;
        file.seekg(offset, std::ios::beg);

        std::vector<uint8_t> page_data(page_size_);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
            continue;
        }

        // 解析页
        BPlusTreeParser parser(page_data);
        if (!parser.parse()) {
            continue;
        }

        // 解析记录
        auto page_records = parser.parse_records(schema_);
        records_.insert(records_.end(), page_records.begin(), page_records.end());
    }

    stats_.records_recovered = records_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_time_ms = stats_.scan_time_ms + stats_.recovery_time_ms;

    return records_;
}

inline std::set<uint32_t> IncrementalRecovery::parse_modified_pages_from_redo_log(
    const std::filesystem::path& redo_log_path) {
    
    modified_pages_.clear();

    std::ifstream file(redo_log_path, std::ios::binary);
    if (!file) {
        return modified_pages_;
    }

    // 读取文件
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);

    // 解析 Redo Log 记录
    size_t pos = 0;
    while (pos + 9 <= buffer.size()) {
        uint8_t type = read_le<uint8_t>(buffer.data(), pos);
        pos += sizeof(uint8_t);

        if (pos + 8 <= buffer.size()) {
            uint32_t space_id = read_le<uint32_t>(buffer.data(), pos);
            pos += sizeof(uint32_t);
            uint32_t page_no = read_le<uint32_t>(buffer.data(), pos);
            pos += sizeof(uint32_t);

            // 记录修改的页
            modified_pages_.insert(page_no);
        }
    }

    return modified_pages_;
}

inline std::vector<std::map<std::string, std::string>> IncrementalRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 扫描页
    auto all_pages = scan_pages();

    // 过滤页范围
    auto filtered_pages = filter_pages_by_range(all_pages);

    // 解析记录
    records_ = parse_page_records(filtered_pages);

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

inline std::vector<std::map<std::string, std::string>> IncrementalRecovery::recover_from_redo_log(
    const std::filesystem::path& redo_log_path) {
    
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 从 Redo Log 解析修改的页
    modified_pages_ = parse_modified_pages_from_redo_log(redo_log_path);

    // 只恢复修改的页
    if (modified_pages_.empty()) {
        return records_;
    }

    // 读取修改的页
    std::vector<ScannedPage> pages;
    for (uint32_t page_no : modified_pages_) {
        std::ifstream file(file_path_, std::ios::binary);
        if (!file) continue;

        size_t offset = static_cast<size_t>(page_no) * page_size_;
        file.seekg(offset, std::ios::beg);

        std::vector<uint8_t> page_data(page_size_);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
            continue;
        }

        ScannedPage page{};
        page.page_no = page_no;
        page.page_type = read_le<uint16_t>(page_data.data(), 16);
        page.is_valid = PageScanner::is_innodb_page(page_data.data(), page_size_);

        pages.push_back(page);
    }

    // 解析记录
    records_ = parse_page_records(pages);

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

// ==================== 增量恢复管理器 ====================
class IncrementalRecoveryManager {
public:
    /**
     * @brief 构造函数
     */
    IncrementalRecoveryManager();

    /**
     * @brief 添加恢复点
     */
    void add_checkpoint(uint64_t lsn, uint32_t page_no);

    /**
     * @brief 设置当前 LSN
     */
    void set_current_lsn(uint64_t lsn);

    /**
     * @brief 获取需要恢复的范围
     */
    std::vector<RecoveryRange> get_recovery_ranges() const;

    /**
     * @brief 获取检查点列表
     */
    const std::map<uint64_t, uint32_t>& get_checkpoints() const { return checkpoints_; }

private:
    std::map<uint64_t, uint32_t> checkpoints_;
    uint64_t current_lsn_ = 0;
};

// ==================== 增量恢复管理器实现 ====================

inline IncrementalRecoveryManager::IncrementalRecoveryManager() {
    current_lsn_ = 0;
}

inline void IncrementalRecoveryManager::add_checkpoint(uint64_t lsn, uint32_t page_no) {
    checkpoints_[lsn] = page_no;
}

inline void IncrementalRecoveryManager::set_current_lsn(uint64_t lsn) {
    current_lsn_ = lsn;
}

inline std::vector<RecoveryRange> IncrementalRecoveryManager::get_recovery_ranges() const {
    std::vector<RecoveryRange> ranges;

    if (checkpoints_.empty()) {
        return ranges;
    }

    // 获取所有检查点
    std::vector<std::pair<uint64_t, uint32_t>> sorted_checkpoints(checkpoints_.begin(), checkpoints_.end());

    // 生成恢复范围
    for (size_t i = 0; i < sorted_checkpoints.size() - 1; ++i) {
        RecoveryRange range{};
        range.start_lsn = sorted_checkpoints[i].first;
        range.end_lsn = sorted_checkpoints[i + 1].first;
        range.start_page_no = sorted_checkpoints[i].second;
        range.end_page_no = sorted_checkpoints[i + 1].second;
        range.is_complete = true;
        ranges.push_back(range);
    }

    return ranges;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_INCREMENTAL_RECOVERY_HPP
