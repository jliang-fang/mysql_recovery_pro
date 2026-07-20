#ifndef DBRECOVERY_INNODB_MMAP_RECOVERY_HPP
#define DBRECOVERY_INNODB_MMAP_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief 内存映射恢复统计
 */
struct MMapRecoveryStats {
    size_t total_pages = 0;
    size_t pages_scanned = 0;
    size_t pages_parsed = 0;
    size_t recovered_records = 0;
    size_t failed_pages = 0;
    double mmap_time_ms = 0.0;
    double recovery_time_ms = 0.0;
    double total_time_ms = 0.0;
    size_t mapped_size = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "MMapRecoveryStats{pages=" << total_pages
            << ", scanned=" << pages_scanned
            << ", parsed=" << pages_parsed
            << ", records=" << recovered_records
            << ", failed=" << failed_pages
            << ", mapped=" << mapped_size << " bytes"
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

/**
 * @brief 内存映射恢复器
 */
class MMapRecovery {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit MMapRecovery(size_t page_size = 16384);

    /**
     * @brief 析构函数
     */
    ~MMapRecovery();

    // 禁止拷贝
    MMapRecovery(const MMapRecovery&) = delete;
    MMapRecovery& operator=(const MMapRecovery&) = delete;

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 执行内存映射恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 获取统计信息
     */
    const MMapRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取映射的内存
     */
    const uint8_t* get_mapped_data() const { return mapped_data_; }

    /**
     * @brief 获取映射大小
     */
    size_t get_mapped_size() const { return mapped_size_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;

    int fd_ = -1;
    uint8_t* mapped_data_ = nullptr;
    size_t mapped_size_ = 0;

    std::vector<std::map<std::string, std::string>> records_;
    MMapRecoveryStats stats_;

    // 映射文件到内存
    bool map_file();

    // 解除映射
    void unmap_file();

    // 解析记录
    std::vector<std::map<std::string, std::string>> parse_records();
};

// ==================== MMapRecovery 实现 ====================

inline MMapRecovery::MMapRecovery(size_t page_size)
    : page_size_(page_size), fd_(-1), mapped_data_(nullptr), mapped_size_(0) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline MMapRecovery::~MMapRecovery() {
    unmap_file();
}

inline void MMapRecovery::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

inline void MMapRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void MMapRecovery::clear() {
    records_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline bool MMapRecovery::map_file() {
    auto start = std::chrono::high_resolution_clock::now();

    unmap_file();

    // 打开文件
    fd_ = open(file_path_.string().c_str(), O_RDONLY);
    if (fd_ == -1) {
        std::cerr << "[ERROR] Failed to open file: " << file_path_.string() << std::endl;
        return false;
    }

    // 获取文件大小
    struct stat st;
    if (fstat(fd_, &st) == -1) {
        std::cerr << "[ERROR] Failed to get file size" << std::endl;
        close(fd_);
        fd_ = -1;
        return false;
    }

    mapped_size_ = st.st_size;

    // 映射文件到内存
    mapped_data_ = static_cast<uint8_t*>(mmap(nullptr, mapped_size_, PROT_READ, MAP_PRIVATE, fd_, 0));
    if (mapped_data_ == MAP_FAILED) {
        std::cerr << "[ERROR] Failed to mmap file: " << file_path_.string() << std::endl;
        close(fd_);
        fd_ = -1;
        return false;
    }

    // 建议内核预读
    madvise(mapped_data_, mapped_size_, MADV_SEQUENTIAL);

    auto end = std::chrono::high_resolution_clock::now();
    stats_.mmap_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return true;
}

inline void MMapRecovery::unmap_file() {
    if (mapped_data_ != nullptr && mapped_data_ != MAP_FAILED) {
        munmap(mapped_data_, mapped_size_);
        mapped_data_ = nullptr;
        mapped_size_ = 0;
    }

    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
}

inline std::vector<std::map<std::string, std::string>> MMapRecovery::parse_records() {
    auto start = std::chrono::high_resolution_clock::now();

    records_.clear();

    if (mapped_data_ == nullptr || mapped_data_ == MAP_FAILED) {
        return records_;
    }

    if (mapped_size_ < page_size_) {
        return records_;
    }

    uint32_t total_pages = static_cast<uint32_t>(mapped_size_ / page_size_);
    stats_.total_pages = total_pages;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        uint8_t* page_data = mapped_data_ + page_no * page_size_;

        // 检查页是否有效
        if (!PageScanner::is_innodb_page(page_data, page_size_)) {
            continue;
        }

        // 读取页类型
        uint16_t page_type = read_be<uint16_t>(page_data, 24);
        if (page_type != FIL_PAGE_INDEX) {
            continue;
        }

        // 解析页
        BPlusTreeParser parser(std::vector<uint8_t>(page_data, page_data + page_size_));
        if (!parser.parse()) {
            stats_.failed_pages++;
            continue;
        }

        // 解析记录
        auto page_records = parser.parse_records(schema_);
        records_.insert(records_.end(), page_records.begin(), page_records.end());
        stats_.pages_parsed++;
    }

    stats_.recovered_records = records_.size();
    stats_.pages_scanned = total_pages;

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_time_ms = stats_.mmap_time_ms + stats_.recovery_time_ms;

    return records_;
}

inline std::vector<std::map<std::string, std::string>> MMapRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 映射文件
    if (!map_file()) {
        return records_;
    }

    // 解析记录
    records_ = parse_records();

    // 解除映射
    unmap_file();

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

// ==================== 内存映射扫描器 ====================
class MMapScanner {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit MMapScanner(size_t page_size = 16384);

    /**
     * @brief 析构函数
     */
    ~MMapScanner();

    // 禁止拷贝
    MMapScanner(const MMapScanner&) = delete;
    MMapScanner& operator=(const MMapScanner&) = delete;

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 扫描文件
     * @return 扫描的页
     */
    std::vector<ScannedPage> scan();

    /**
     * @brief 获取统计信息
     */
    const MMapRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取映射的内存
     */
    const uint8_t* get_mapped_data() const { return mapped_data_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    std::filesystem::path file_path_;

    int fd_ = -1;
    uint8_t* mapped_data_ = nullptr;
    size_t mapped_size_ = 0;

    std::vector<ScannedPage> pages_;
    MMapRecoveryStats stats_;

    bool map_file();
    void unmap_file();
};

// ==================== MMapScanner 实现 ====================

inline MMapScanner::MMapScanner(size_t page_size)
    : page_size_(page_size), fd_(-1), mapped_data_(nullptr), mapped_size_(0) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline MMapScanner::~MMapScanner() {
    unmap_file();
}

inline void MMapScanner::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

inline void MMapScanner::clear() {
    pages_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline bool MMapScanner::map_file() {
    unmap_file();

    fd_ = open(file_path_.string().c_str(), O_RDONLY);
    if (fd_ == -1) {
        return false;
    }

    struct stat st;
    if (fstat(fd_, &st) == -1) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    mapped_size_ = st.st_size;

    mapped_data_ = static_cast<uint8_t*>(mmap(nullptr, mapped_size_, PROT_READ, MAP_PRIVATE, fd_, 0));
    if (mapped_data_ == MAP_FAILED) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    madvise(mapped_data_, mapped_size_, MADV_SEQUENTIAL);

    return true;
}

inline void MMapScanner::unmap_file() {
    if (mapped_data_ != nullptr && mapped_data_ != MAP_FAILED) {
        munmap(mapped_data_, mapped_size_);
        mapped_data_ = nullptr;
        mapped_size_ = 0;
    }

    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
}

inline std::vector<ScannedPage> MMapScanner::scan() {
    auto start = std::chrono::high_resolution_clock::now();

    clear();

    if (!map_file()) {
        return pages_;
    }

    if (mapped_size_ < page_size_) {
        unmap_file();
        return pages_;
    }

    uint32_t total_pages = static_cast<uint32_t>(mapped_size_ / page_size_);
    stats_.total_pages = total_pages;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        uint8_t* page_data = mapped_data_ + page_no * page_size_;

        ScannedPage page{};
        page.offset = static_cast<uint32_t>(page_no * page_size_);
        page.page_no = page_no;
        page.page_type = read_be<uint16_t>(page_data, 24);
        page.prev_page = read_be<uint32_t>(page_data, 8);
        page.next_page = read_be<uint32_t>(page_data, 12);
        page.lsn = read_be<uint64_t>(page_data, 16);
        page.space_id = read_be<uint32_t>(page_data, 34);
        page.is_valid = PageScanner::is_innodb_page(page_data, page_size_);

        pages_.push_back(page);
    }

    stats_.pages_scanned = pages_.size();
    stats_.mapped_size = mapped_size_;

    unmap_file();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.scan_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return pages_;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_MMAP_RECOVERY_HPP
