#ifndef DBRECOVERY_INNODB_PARTITION_TABLE_RECOVERY_HPP
#define DBRECOVERY_INNODB_PARTITION_TABLE_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <set>
#include <algorithm>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief 分区信息
 */
struct PartitionInfo {
    std::string partition_name;
    uint32_t partition_id;
    uint32_t tablespace_id;
    std::string ibd_path;
    uint64_t row_count;
    uint64_t data_length;
    uint32_t first_page_no;
    uint32_t last_page_no;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "PartitionInfo{name=" << partition_name
            << ", id=" << partition_id
            << ", tablespace=" << tablespace_id
            << ", pages=" << first_page_no << "-" << last_page_no
            << ", rows=" << row_count << "}";
        return oss.str();
    }
};

/**
 * @brief 分区表恢复统计
 */
struct PartitionTableRecoveryStats {
    size_t total_partitions = 0;
    size_t recovered_partitions = 0;
    size_t failed_partitions = 0;
    size_t total_records = 0;
    double recovery_time_ms = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "PartitionTableRecoveryStats{partitions=" << total_partitions
            << ", recovered=" << recovered_partitions
            << ", failed=" << failed_partitions
            << ", records=" << total_records
            << ", time=" << std::fixed << std::setprecision(2) << recovery_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

/**
 * @brief 分区表恢复器
 */
class PartitionTableRecovery {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit PartitionTableRecovery(size_t page_size = 16384);

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置表名
     */
    void set_table_name(const std::string& table_name);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 扫描分区表
     * @return 是否找到分区
     */
    bool scan_partitions();

    /**
     * @brief 添加分区
     */
    void add_partition(const PartitionInfo& partition);

    /**
     * @brief 执行分区表恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 获取统计信息
     */
    const PartitionTableRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取分区列表
     */
    const std::vector<PartitionInfo>& get_partitions() const { return partitions_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::string data_dir_;
    std::string table_name_;
    catalog::TableSchema schema_;

    std::vector<PartitionInfo> partitions_;
    std::vector<std::map<std::string, std::string>> records_;
    PartitionTableRecoveryStats stats_;

    // 扫描目录查找分区文件
    void scan_directory_for_partitions(const std::string& dir, int depth = 0);

    // 从文件名解析分区名
    std::optional<PartitionInfo> parse_partition_from_path(const std::string& path);

    // 恢复单个分区
    std::vector<std::map<std::string, std::string>> recover_partition(
        const PartitionInfo& partition);
};

// ==================== PartitionTableRecovery 实现 ====================

inline PartitionTableRecovery::PartitionTableRecovery(size_t page_size)
    : page_size_(page_size) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void PartitionTableRecovery::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

inline void PartitionTableRecovery::set_table_name(const std::string& table_name) {
    table_name_ = table_name;
}

inline void PartitionTableRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void PartitionTableRecovery::clear() {
    partitions_.clear();
    records_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void PartitionTableRecovery::scan_directory_for_partitions(const std::string& dir, int depth) {
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

                // 检查是否是分区表文件
                // 分区表文件名格式: table_name#P#partition_name.ibd
                if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
                    // 移除 .ibd 扩展名
                    std::string base_name = filename.substr(0, filename.size() - 4);

                    // 检查是否包含分区标记
                    if (base_name.find("#P#") != std::string::npos) {
                        auto partition = parse_partition_from_path(entry.path().string());
                        if (partition.has_value()) {
                            partitions_.push_back(partition.value());
                        }
                    }
                }
            } else if (entry.is_directory()) {
                std::string dirname = entry.path().filename().string();
                if (dirname != "." && dirname != ".." &&
                    dirname != "mysql" && dirname != "sys" &&
                    dirname != "performance_schema" && dirname != "information_schema") {
                    scan_directory_for_partitions(entry.path().string(), depth + 1);
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // 忽略错误
    }
}

inline std::optional<PartitionInfo> PartitionTableRecovery::parse_partition_from_path(
    const std::string& path) {
    std::filesystem::path p(path);
    std::string filename = p.filename().string();

    if (filename.size() <= 4) {
        return std::nullopt;
    }

    std::string base_name = filename.substr(0, filename.size() - 4);

    // 解析分区名
    // 格式: table_name#P#partition_name
    size_t pos = base_name.find("#P#");
    if (pos == std::string::npos) {
        return std::nullopt;
    }

    std::string table_part = base_name.substr(0, pos);
    std::string partition_part = base_name.substr(pos + 3);

    PartitionInfo info;
    info.partition_name = partition_part;
    info.ibd_path = path;
    info.first_page_no = 0;
    info.last_page_no = 0;

    // 尝试从文件大小估算页数
    std::error_code ec;
    uint64_t file_size = std::filesystem::file_size(path, ec);
    if (!ec && file_size >= page_size_) {
        info.last_page_no = static_cast<uint32_t>(file_size / page_size_) - 1;
    }

    return info;
}

inline void PartitionTableRecovery::add_partition(const PartitionInfo& partition) {
    partitions_.push_back(partition);
}

inline std::vector<std::map<std::string, std::string>> PartitionTableRecovery::recover_partition(
    const PartitionInfo& partition) {
    std::vector<std::map<std::string, std::string>> records;

    std::ifstream file(partition.ibd_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return records;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        return records;
    }

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
    std::vector<uint8_t> page_data(page_size_);

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        uint16_t page_type = read_le<uint16_t>(page_data.data(), 16);
        if (page_type != FIL_PAGE_INDEX) {
            continue;
        }

        BPlusTreeParser parser(page_data);
        if (!parser.parse()) {
            continue;
        }

        auto page_records = parser.parse_records(schema_);
        records.insert(records.end(), page_records.begin(), page_records.end());
    }

    return records;
}

inline bool PartitionTableRecovery::scan_partitions() {
    partitions_.clear();

    if (data_dir_.empty()) {
        return false;
    }

    scan_directory_for_partitions(data_dir_);

    return !partitions_.empty();
}

inline std::vector<std::map<std::string, std::string>> PartitionTableRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    if (partitions_.empty()) {
        return records_;
    }

    stats_.total_partitions = partitions_.size();

    for (const auto& partition : partitions_) {
        auto partition_records = recover_partition(partition);
        records_.insert(records_.end(), partition_records.begin(), partition_records.end());

        if (!partition_records.empty()) {
            stats_.recovered_partitions++;
        } else {
            stats_.failed_partitions++;
        }
    }

    stats_.total_records = records_.size();

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

// ==================== 分区表扫描器 ====================
class PartitionTableScanner {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit PartitionTableScanner(size_t page_size = 16384);

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 扫描分区表
     * @return 分区表列表
     */
    std::vector<std::string> scan();

    /**
     * @brief 获取分区信息
     */
    std::vector<PartitionInfo> get_partitions(const std::string& table_name);

private:
    size_t page_size_;
    std::string data_dir_;
    std::set<std::string> tables_;

    void scan_directory(const std::string& dir, int depth = 0);
};

// ==================== PartitionTableScanner 实现 ====================

inline PartitionTableScanner::PartitionTableScanner(size_t page_size)
    : page_size_(page_size) {}

inline void PartitionTableScanner::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

inline void PartitionTableScanner::scan_directory(const std::string& dir, int depth) {
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
                    std::string base_name = filename.substr(0, filename.size() - 4);

                    // 检查是否是分区表文件
                    if (base_name.find("#P#") != std::string::npos) {
                        // 提取表名
                        size_t pos = base_name.find("#P#");
                        if (pos != std::string::npos) {
                            std::string table_name = base_name.substr(0, pos);
                            tables_.insert(table_name);
                        }
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

inline std::vector<std::string> PartitionTableScanner::scan() {
    tables_.clear();

    if (data_dir_.empty()) {
        return {};
    }

    scan_directory(data_dir_);

    return std::vector<std::string>(tables_.begin(), tables_.end());
}

inline std::vector<PartitionInfo> PartitionTableScanner::get_partitions(
    const std::string& table_name) {
    std::vector<PartitionInfo> partitions;

    if (data_dir_.empty()) {
        return partitions;
    }

    // 扫描目录查找该表的分区
    std::filesystem::path directory(data_dir_);

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();

                if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
                    std::string base_name = filename.substr(0, filename.size() - 4);

                    // 检查是否是该表的分区
                    std::string expected_prefix = table_name + "#P#";
                    if (base_name.find(expected_prefix) == 0) {
                        PartitionInfo info;
                        info.partition_name = base_name.substr(expected_prefix.size());
                        info.ibd_path = entry.path().string();

                        // 估算页数
                        std::error_code ec;
                        uint64_t file_size = std::filesystem::file_size(entry.path(), ec);
                        if (!ec && file_size >= page_size_) {
                            info.last_page_no = static_cast<uint32_t>(file_size / page_size_) - 1;
                        }

                        partitions.push_back(info);
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // 忽略错误
    }

    return partitions;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PARTITION_TABLE_RECOVERY_HPP
