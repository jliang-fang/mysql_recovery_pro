#ifndef DBRECOVERY_INNODB_MULTI_VERSION_RECOVERY_HPP
#define DBRECOVERY_INNODB_MULTI_VERSION_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <memory>
#include <algorithm>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_parser.hpp"
#include "dbrecovery/innodb/page_parser_v2.hpp"
#include "dbrecovery/innodb/page_parser_v3.hpp"
#include "dbrecovery/innodb/page_parser_v82.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief MySQL 版本
 */
enum class MySQLVersion {
    UNKNOWN = 0,
    MYSQL_55 = 55,
    MYSQL_56 = 56,
    MYSQL_57 = 57,
    MYSQL_80 = 80,
    MYSQL_81 = 81,
    MYSQL_82 = 82,
    MYSQL_83 = 83,
    MYSQL_84 = 84,
    MYSQL_85 = 85,
    MYSQL_90 = 90,
};

/**
 * @brief 版本检测结果
 */
struct VersionDetectionResult {
    MySQLVersion version;
    uint32_t page_size;
    uint8_t row_format;
    bool is_compressed;
    bool is_encrypted;
    std::string detected_from;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "VersionDetectionResult{version=" << static_cast<int>(version)
            << ", page_size=" << page_size
            << ", row_format=" << static_cast<int>(row_format)
            << ", compressed=" << (is_compressed ? "true" : "false")
            << ", encrypted=" << (is_encrypted ? "true" : "false")
            << "}";
        return oss.str();
    }
};

/**
 * @brief 多版本恢复统计
 */
struct MultiVersionRecoveryStats {
    size_t total_pages = 0;
    size_t pages_scanned = 0;
    size_t pages_parsed = 0;
    size_t recovered_records = 0;
    size_t failed_pages = 0;
    MySQLVersion detected_version = MySQLVersion::UNKNOWN;
    double detection_time_ms = 0.0;
    double recovery_time_ms = 0.0;
    double total_time_ms = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "MultiVersionRecoveryStats{pages=" << total_pages
            << ", scanned=" << pages_scanned
            << ", parsed=" << pages_parsed
            << ", records=" << recovered_records
            << ", failed=" << failed_pages
            << ", version=" << static_cast<int>(detected_version)
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

/**
 * @brief 多版本恢复器
 */
class MultiVersionRecovery {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit MultiVersionRecovery(size_t page_size = 16384);

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置是否自动检测版本
     */
    void set_auto_detect_version(bool auto_detect);

    /**
     * @brief 设置强制版本
     */
    void set_force_version(MySQLVersion version);

    /**
     * @brief 执行多版本恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 获取统计信息
     */
    const MultiVersionRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取检测到的版本
     */
    MySQLVersion get_detected_version() const { return stats_.detected_version; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;
    bool auto_detect_version_ = true;
    MySQLVersion force_version_ = MySQLVersion::UNKNOWN;

    MultiVersionRecoveryStats stats_;
    std::vector<std::map<std::string, std::string>> records_;

    // 检测 MySQL 版本
    VersionDetectionResult detect_version();

    // 使用指定版本解析页
    std::vector<std::map<std::string, std::string>> parse_with_version(
        const std::vector<ScannedPage>& pages, MySQLVersion version);

    // 从页数据解析记录
    std::vector<std::map<std::string, std::string>> parse_page_records(
        const std::vector<ScannedPage>& pages);
};

// ==================== MultiVersionRecovery 实现 ====================

inline MultiVersionRecovery::MultiVersionRecovery(size_t page_size)
    : page_size_(page_size), auto_detect_version_(true) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void MultiVersionRecovery::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

inline void MultiVersionRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void MultiVersionRecovery::set_auto_detect_version(bool auto_detect) {
    auto_detect_version_ = auto_detect;
}

inline void MultiVersionRecovery::set_force_version(MySQLVersion version) {
    force_version_ = version;
    auto_detect_version_ = false;
}

inline void MultiVersionRecovery::clear() {
    records_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline VersionDetectionResult MultiVersionRecovery::detect_version() {
    auto start = std::chrono::high_resolution_clock::now();

    VersionDetectionResult result;
    result.page_size = page_size_;
    result.row_format = 2;  // DYNAMIC
    result.is_compressed = false;
    result.is_encrypted = false;

    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        result.version = MySQLVersion::UNKNOWN;
        return result;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        result.version = MySQLVersion::UNKNOWN;
        return result;
    }

    std::vector<uint8_t> page_data(page_size_);
    file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

    // 检查页类型
    uint16_t page_type = read_le<uint16_t>(page_data.data(), 16);
    if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI) {
        result.version = MySQLVersion::UNKNOWN;
        return result;
    }

    // 检查页头信息
    if (page_data.size() >= 80) {
        uint16_t flags = read_le<uint16_t>(page_data.data(), 80);

        // 检查压缩标志
        if (flags & 0x0080) {
            result.is_compressed = true;
        }

        // 从 flags 推断行格式
        uint8_t row_format = flags & 0x0F;
        result.row_format = row_format;
    }

    // 检查 LSN 值（MySQL 8.0+ LSN 更大）
    uint64_t lsn = read_le<uint64_t>(page_data.data(), 8);
    if (lsn > 10000000000ULL) {  // 大于 10B LSN，可能是 MySQL 8.0+
        result.version = MySQLVersion::MYSQL_80;
        result.detected_from = "LSN value";
    } else {
        // 检查页头版本信息
        if (page_data.size() >= 54 + 16) {
            // 检查 page header 的 version 字段
            uint8_t page_header_version = page_data[38 + 16];
            if (page_header_version >= 2) {
                result.version = MySQLVersion::MYSQL_80;
                result.detected_from = "page header version";
            } else {
                result.version = MySQLVersion::MYSQL_56;
                result.detected_from = "page header format";
            }
        } else {
            result.version = MySQLVersion::MYSQL_56;
            result.detected_from = "default assumption";
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.detection_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return result;
}

inline std::vector<std::map<std::string, std::string>> MultiVersionRecovery::parse_with_version(
    const std::vector<ScannedPage>& pages, MySQLVersion version) {

    std::vector<std::map<std::string, std::string>> records;

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
        file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

        if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
            continue;
        }

        // 根据版本选择解析器
        bool parsed = false;

        switch (version) {
            case MySQLVersion::MYSQL_55:
            case MySQLVersion::MYSQL_56:
            case MySQLVersion::MYSQL_57: {
                // 使用 v2 解析器
                dbrecovery::innodb::PageParserV2 parser(page_data);
                parsed = parser.parse();
                if (parsed) {
                    auto page_records = parser.parse_records(schema_);
                    records.insert(records.end(), page_records.begin(), page_records.end());
                }
                break;
            }
            case MySQLVersion::MYSQL_80:
            case MySQLVersion::MYSQL_81:
            case MySQLVersion::MYSQL_82:
            case MySQLVersion::MYSQL_83: {
                // 使用 v82 解析器
                dbrecovery::innodb::PageParserV82 parser(page_data);
                parsed = parser.parse();
                if (parsed) {
                    auto page_records = parser.parse_records(schema_);
                    records.insert(records.end(), page_records.begin(), page_records.end());
                }
                break;
            }
            case MySQLVersion::MYSQL_84:
            case MySQLVersion::MYSQL_85:
            case MySQLVersion::MYSQL_90: {
                // 使用 v3 解析器
                dbrecovery::innodb::PageParserV3 parser(page_data);
                parsed = parser.parse();
                if (parsed) {
                    auto page_records = parser.parse_records(schema_);
                    records.insert(records.end(), page_records.begin(), page_records.end());
                }
                break;
            }
            default: {
                // 使用默认解析器
                BPlusTreeParser parser(page_data);
                parsed = parser.parse();
                if (parsed) {
                    auto page_records = parser.parse_records(schema_);
                    records.insert(records.end(), page_records.begin(), page_records.end());
                }
                break;
            }
        }
    }

    return records;
}

inline std::vector<std::map<std::string, std::string>> MultiVersionRecovery::parse_page_records(
    const std::vector<ScannedPage>& pages) {

    auto start = std::chrono::high_resolution_clock::now();

    records_.clear();

    // 如果设置了强制版本，使用该版本
    if (!auto_detect_version_ && force_version_ != MySQLVersion::UNKNOWN) {
        records_ = parse_with_version(pages, force_version_);
    } else {
        // 自动检测版本
        auto version_result = detect_version();
        stats_.detected_version = version_result.version;
        records_ = parse_with_version(pages, version_result.version);
    }

    stats_.recovered_records = records_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_time_ms = stats_.detection_time_ms + stats_.recovery_time_ms;

    return records_;
}

inline std::vector<std::map<std::string, std::string>> MultiVersionRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 扫描页
    std::vector<ScannedPage> pages;

    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return records_;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        return records_;
    }

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
    std::vector<uint8_t> page_data(page_size_);

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

        ScannedPage page{};
        page.page_no = page_no;
        page.page_type = read_le<uint16_t>(page_data.data(), 16);
        page.prev_page = read_le<uint32_t>(page_data.data(), 0);
        page.next_page = read_le<uint32_t>(page_data.data(), 4);
        page.lsn = read_le<uint64_t>(page_data.data(), 8);
        page.space_id = read_le<uint32_t>(page_data.data(), 24);
        page.is_valid = PageScanner::is_innodb_page(page_data.data(), page_size_);

        pages.push_back(page);
    }

    stats_.total_pages = total_pages;
    stats_.pages_scanned = pages.size();

    // 解析记录
    records_ = parse_page_records(pages);
    stats_.pages_parsed = stats_.recovered_records > 0 ? pages.size() : 0;

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

// ==================== 多版本扫描器 ====================
class MultiVersionScanner {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit MultiVersionScanner(size_t page_size = 16384);

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置最大深度
     */
    void set_max_depth(int depth);

    /**
     * @brief 扫描多版本数据
     * @return 扫描结果
     */
    std::map<MySQLVersion, std::vector<std::string>> scan();

    /**
     * @brief 获取版本检测结果
     */
    std::map<std::string, VersionDetectionResult> get_version_results() const {
        return version_results_;
    }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::string data_dir_;
    int max_depth_ = 10;

    std::map<MySQLVersion, std::vector<std::string>> version_map_;
    std::map<std::string, VersionDetectionResult> version_results_;

    void scan_directory(const std::string& dir, int depth = 0);
    VersionDetectionResult detect_version_from_file(const std::string& path);
};

// ==================== MultiVersionScanner 实现 ====================

inline MultiVersionScanner::MultiVersionScanner(size_t page_size)
    : page_size_(page_size) {}

inline void MultiVersionScanner::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

inline void MultiVersionScanner::set_max_depth(int depth) {
    max_depth_ = depth > 0 ? depth : 10;
}

inline void MultiVersionScanner::clear() {
    version_map_.clear();
    version_results_.clear();
}

inline VersionDetectionResult MultiVersionScanner::detect_version_from_file(const std::string& path) {
    VersionDetectionResult result;
    result.page_size = page_size_;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        result.version = MySQLVersion::UNKNOWN;
        return result;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        result.version = MySQLVersion::UNKNOWN;
        return result;
    }

    std::vector<uint8_t> page_data(page_size_);
    file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

    // 检查页类型
    uint16_t page_type = read_le<uint16_t>(page_data.data(), 16);
    if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI) {
        result.version = MySQLVersion::UNKNOWN;
        return result;
    }

    // 检查 LSN
    uint64_t lsn = read_le<uint64_t>(page_data.data(), 8);
    if (lsn > 10000000000ULL) {
        result.version = MySQLVersion::MYSQL_80;
        result.detected_from = "LSN";
    } else {
        result.version = MySQLVersion::MYSQL_56;
        result.detected_from = "LSN";
    }

    // 检查 flags
    if (page_data.size() >= 80) {
        uint16_t flags = read_le<uint16_t>(page_data.data(), 80);
        result.is_compressed = (flags & 0x0080) != 0;
        result.row_format = flags & 0x0F;
    }

    return result;
}

inline void MultiVersionScanner::scan_directory(const std::string& dir, int depth) {
    if (depth > max_depth_) {
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
                    auto result = detect_version_from_file(entry.path().string());
                    version_results_[filename] = result;

                    if (result.version != MySQLVersion::UNKNOWN) {
                        version_map_[result.version].push_back(filename);
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

inline std::map<MySQLVersion, std::vector<std::string>> MultiVersionScanner::scan() {
    clear();

    if (data_dir_.empty()) {
        return version_map_;
    }

    scan_directory(data_dir_);

    return version_map_;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_MULTI_VERSION_RECOVERY_HPP
