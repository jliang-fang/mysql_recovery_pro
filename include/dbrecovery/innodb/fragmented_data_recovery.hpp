#ifndef DBRECOVERY_INNODB_FRAGMENTED_DATA_RECOVERY_HPP
#define DBRECOVERY_INNODB_FRAGMENTED_DATA_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <set>
#include <unordered_map>
#include <algorithm>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief 片段信息
 */
struct FragmentInfo {
    uint32_t page_no;
    uint32_t space_id;
    uint64_t lsn;
    uint16_t page_type;
    uint32_t prev_page;
    uint32_t next_page;
    bool is_valid;
    bool is_orphaned;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "FragmentInfo{page=" << page_no
            << ", space=" << space_id
            << ", lsn=" << lsn
            << ", type=" << page_type
            << ", orphaned=" << (is_orphaned ? "true" : "false")
            << "}";
        return oss.str();
    }
};

/**
 * @brief 碎片化数据恢复统计
 */
struct FragmentedDataRecoveryStats {
    size_t total_fragments = 0;
    size_t valid_fragments = 0;
    size_t orphaned_fragments = 0;
    size_t recovered_records = 0;
    size_t reconstructed_chains = 0;
    double scan_time_ms = 0.0;
    double recovery_time_ms = 0.0;
    double total_time_ms = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "FragmentedDataRecoveryStats{fragments=" << total_fragments
            << ", valid=" << valid_fragments
            << ", orphaned=" << orphaned_fragments
            << ", records=" << recovered_records
            << ", chains=" << reconstructed_chains
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

/**
 * @brief 碎片化数据恢复器
 */
class FragmentedDataRecovery {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit FragmentedDataRecovery(size_t page_size = 16384);

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置扫描深度
     */
    void set_max_depth(int depth);

    /**
     * @brief 设置是否尝试重建链
     */
    void set_try_reconstruct_chains(bool try_reconstruct);

    /**
     * @brief 执行碎片化数据恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 获取统计信息
     */
    const FragmentedDataRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取片段列表
     */
    const std::vector<FragmentInfo>& get_fragments() const { return fragments_; }

    /**
     * @brief 获取重建的链
     */
    const std::vector<std::vector<uint32_t>>& get_reconstructed_chains() const {
        return reconstructed_chains_;
    }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;
    int max_depth_ = 10;
    bool try_reconstruct_chains_ = true;

    std::vector<FragmentInfo> fragments_;
    std::vector<std::vector<uint32_t>> reconstructed_chains_;
    std::vector<std::map<std::string, std::string>> records_;
    FragmentedDataRecoveryStats stats_;

    // 扫描文件获取所有页
    std::vector<FragmentInfo> scan_fragments();

    // 分析页并检测碎片化
    void analyze_fragments();

    // 重建页链
    void reconstruct_chains();

    // 解析记录
    std::vector<std::map<std::string, std::string>> parse_records();
};

// ==================== FragmentedDataRecovery 实现 ====================

inline FragmentedDataRecovery::FragmentedDataRecovery(size_t page_size)
    : page_size_(page_size), try_reconstruct_chains_() {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void FragmentedDataRecovery::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

inline void FragmentedDataRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void FragmentedDataRecovery::set_max_depth(int depth) {
    max_depth_ = depth > 0 ? depth : 10;
}

inline void FragmentedDataRecovery::set_try_reconstruct_chains(bool try_reconstruct) {
    try_reconstruct_chains_ = try_reconstruct;
}

inline void FragmentedDataRecovery::clear() {
    fragments_.clear();
    reconstructed_chains_.clear();
    records_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline std::vector<FragmentInfo> FragmentedDataRecovery::scan_fragments() {
    auto start = std::chrono::high_resolution_clock::now();

    fragments_.clear();

    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return fragments_;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        return fragments_;
    }

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
    std::vector<uint8_t> page_data(page_size_);

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        FragmentInfo fragment;
        fragment.page_no = page_no;
        fragment.space_id = read_le<uint32_t>(page_data.data(), 24);
        fragment.lsn = read_le<uint64_t>(page_data.data(), 8);
        fragment.page_type = read_le<uint16_t>(page_data.data(), 16);
        fragment.prev_page = read_le<uint32_t>(page_data.data(), 0);
        fragment.next_page = read_le<uint32_t>(page_data.data(), 4);
        fragment.is_valid = PageScanner::is_innodb_page(page_data.data(), page_size_);
        fragment.is_orphaned = false;

        fragments_.push_back(fragment);
    }

    stats_.total_fragments = fragments_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.scan_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return fragments_;
}

inline void FragmentedDataRecovery::analyze_fragments() {
    auto start = std::chrono::high_resolution_clock::now();

    // 统计页类型
    std::map<uint16_t, size_t> page_type_counts;
    for (const auto& fragment : fragments_) {
        page_type_counts[fragment.page_type]++;
    }

    // 检测 orphaned 页
    std::set<uint32_t> all_pages;
    for (const auto& fragment : fragments_) {
        all_pages.insert(fragment.page_no);
    }

    for (auto& fragment : fragments_) {
        // 检查 prev_page 是否在文件中
        if (fragment.prev_page != 0xFFFFFFFF && all_pages.find(fragment.prev_page) == all_pages.end()) {
            fragment.is_orphaned = true;
        }

        // 检查 next_page 是否在文件中
        if (fragment.next_page != 0xFFFFFFFF && all_pages.find(fragment.next_page) == all_pages.end()) {
            fragment.is_orphaned = true;
        }
    }

    // 统计有效页和 orphaned 页
    stats_.valid_fragments = 0;
    stats_.orphaned_fragments = 0;
    for (const auto& fragment : fragments_) {
        if (fragment.is_valid) {
            stats_.valid_fragments++;
            if (fragment.is_orphaned) {
                stats_.orphaned_fragments++;
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.scan_time_ms += std::chrono::duration<double, std::milli>(end - start).count();
}

inline void FragmentedDataRecovery::reconstruct_chains() {
    if (!try_reconstruct_chains_) {
        return;
    }

    auto start = std::chrono::high_resolution_clock::now();

    reconstructed_chains_.clear();

    // 按页类型分组
    std::map<uint16_t, std::vector<FragmentInfo*>> pages_by_type;
    for (auto& fragment : fragments_) {
        if (fragment.is_valid && fragment.page_type == FIL_PAGE_INDEX) {
            pages_by_type[fragment.page_type].push_back(&fragment);
        }
    }

    // 重建页链
    std::set<uint32_t> used_pages;

    for (auto& [page_type, pages] : pages_by_type) {
        std::vector<uint32_t> chain;

        // 找到链的起始页（prev_page 为 0xFFFFFFFF）
        for (auto* fragment : pages) {
            if (fragment->prev_page == 0xFFFFFFFF && used_pages.find(fragment->page_no) == used_pages.end()) {
                chain.clear();
                uint32_t current_page = fragment->page_no;

                while (current_page != 0xFFFFFFFF && used_pages.find(current_page) == used_pages.end()) {
                    chain.push_back(current_page);
                    used_pages.insert(current_page);

                    // 找到下一个页
                    bool found = false;
                    for (auto* f : pages) {
                        if (f->page_no == current_page) {
                            current_page = f->next_page;
                            found = true;
                            break;
                        }
                    }

                    if (!found) {
                        break;
                    }
                }

                if (chain.size() > 1) {
                    reconstructed_chains_.push_back(chain);
                }
            }
        }
    }

    stats_.reconstructed_chains = reconstructed_chains_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.scan_time_ms += std::chrono::duration<double, std::milli>(end - start).count();
}

inline std::vector<std::map<std::string, std::string>> FragmentedDataRecovery::parse_records() {
    auto start = std::chrono::high_resolution_clock::now();

    records_.clear();

    // 优先使用重建的链
    std::vector<uint32_t> pages_to_process;

    if (!reconstructed_chains_.empty()) {
        for (const auto& chain : reconstructed_chains_) {
            pages_to_process.insert(pages_to_process.end(), chain.begin(), chain.end());
        }
    } else {
        for (const auto& fragment : fragments_) {
            if (fragment.is_valid) {
                pages_to_process.push_back(fragment.page_no);
            }
        }
    }

    // 去重
    std::set<uint32_t> unique_pages(pages_to_process.begin(), pages_to_process.end());
    pages_to_process.assign(unique_pages.begin(), unique_pages.end());

    std::vector<uint8_t> page_data(page_size_);

    for (uint32_t page_no : pages_to_process) {
        std::ifstream file(file_path_, std::ios::binary);
        if (!file) continue;

        uint64_t offset = static_cast<uint64_t>(page_no) * page_size_;
        file.seekg(offset, std::ios::beg);

        file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

        if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
            continue;
        }

        BPlusTreeParser parser(page_data);
        if (!parser.parse()) {
            continue;
        }

        auto page_records = parser.parse_records(schema_);
        records_.insert(records_.end(), page_records.begin(), page_records.end());
    }

    stats_.recovered_records = records_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_time_ms = stats_.scan_time_ms + stats_.recovery_time_ms;

    return records_;
}

inline std::vector<std::map<std::string, std::string>> FragmentedDataRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 扫描片段
    auto all_fragments = scan_fragments();

    // 分析片段
    analyze_fragments();

    // 重建链
    reconstruct_chains();

    // 解析记录
    records_ = parse_records();

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

// ==================== 碎片化数据扫描器 ====================
class FragmentedDataScanner {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit FragmentedDataScanner(size_t page_size = 16384);

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置最大深度
     */
    void set_max_depth(int depth);

    /**
     * @brief 扫描碎片化数据
     * @return 扫描结果
     */
    std::vector<FragmentInfo> scan();

    /**
     * @brief 获取统计信息
     */
    const FragmentedDataRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::string data_dir_;
    int max_depth_ = 10;

    std::vector<FragmentInfo> fragments_;
    FragmentedDataRecoveryStats stats_;

    void scan_directory(const std::string& dir, int depth = 0);
};

// ==================== FragmentedDataScanner 实现 ====================

inline FragmentedDataScanner::FragmentedDataScanner(size_t page_size)
    : page_size_(page_size) {}

inline void FragmentedDataScanner::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

inline void FragmentedDataScanner::set_max_depth(int depth) {
    max_depth_ = depth > 0 ? depth : 10;
}

inline void FragmentedDataScanner::clear() {
    fragments_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void FragmentedDataScanner::scan_directory(const std::string& dir, int depth) {
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
                        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

                        FragmentInfo fragment;
                        fragment.page_no = page_no;
                        fragment.space_id = read_le<uint32_t>(page_data.data(), 24);
                        fragment.lsn = read_le<uint64_t>(page_data.data(), 8);
                        fragment.page_type = read_le<uint16_t>(page_data.data(), 16);
                        fragment.prev_page = read_le<uint32_t>(page_data.data(), 0);
                        fragment.next_page = read_le<uint32_t>(page_data.data(), 4);
                        fragment.is_valid = PageScanner::is_innodb_page(page_data.data(), page_size_);

                        fragments_.push_back(fragment);
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

inline std::vector<FragmentInfo> FragmentedDataScanner::scan() {
    clear();

    if (data_dir_.empty()) {
        return fragments_;
    }

    scan_directory(data_dir_);

    stats_.total_fragments = fragments_.size();

    // 分析片段
    std::set<uint32_t> all_pages;
    for (const auto& fragment : fragments_) {
        all_pages.insert(fragment.page_no);
    }

    for (auto& fragment : fragments_) {
        if (fragment.prev_page != 0xFFFFFFFF && all_pages.find(fragment.prev_page) == all_pages.end()) {
            fragment.is_orphaned = true;
        }
        if (fragment.next_page != 0xFFFFFFFF && all_pages.find(fragment.next_page) == all_pages.end()) {
            fragment.is_orphaned = true;
        }
    }

    stats_.valid_fragments = 0;
    stats_.orphaned_fragments = 0;
    for (const auto& fragment : fragments_) {
        if (fragment.is_valid) {
            stats_.valid_fragments++;
            if (fragment.is_orphaned) {
                stats_.orphaned_fragments++;
            }
        }
    }

    return fragments_;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_FRAGMENTED_DATA_RECOVERY_HPP
