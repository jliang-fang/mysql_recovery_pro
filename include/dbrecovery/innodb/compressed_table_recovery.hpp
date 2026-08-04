#ifndef DBRECOVERY_INNODB_COMPRESSED_TABLE_RECOVERY_HPP
#define DBRECOVERY_INNODB_COMPRESSED_TABLE_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <zlib.h>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/utils/lz4_helper.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief 压缩表恢复统计
 */
struct CompressedTableRecoveryStats {
    size_t total_pages = 0;
    size_t compressed_pages = 0;
    size_t decompressed_pages = 0;
    size_t recovered_records = 0;
    size_t failed_pages = 0;
    double decompress_time_ms = 0.0;
    double recovery_time_ms = 0.0;
    double total_time_ms = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "CompressedTableRecoveryStats{pages=" << total_pages
            << ", compressed=" << compressed_pages
            << ", decompressed=" << decompressed_pages
            << ", records=" << recovered_records
            << ", failed=" << failed_pages
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

/**
 * @brief 压缩表恢复器
 */
class CompressedTableRecovery {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     */
    explicit CompressedTableRecovery(size_t page_size = 16384);

    /**
     * @brief 设置文件路径
     */
    void set_file_path(const std::filesystem::path& file_path);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置压缩页大小
     */
    void set_compressed_page_size(size_t compressed_size);

    /**
     * @brief 执行压缩表恢复
     * @return 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover();

    /**
     * @brief 获取统计信息
     */
    const CompressedTableRecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief 获取压缩页列表
     */
    const std::vector<uint32_t>& get_compressed_pages() const { return compressed_pages_; }

    /**
     * @brief 清空数据
     */
    void clear();

private:
    size_t page_size_;
    size_t compressed_page_size_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;

    std::vector<uint32_t> compressed_pages_;
    std::vector<std::map<std::string, std::string>> records_;
    CompressedTableRecoveryStats stats_;

    // 扫描页并检测压缩
    std::vector<ScannedPage> scan_pages();

    // 检测页是否压缩
    bool is_page_compressed(const uint8_t* page_data, size_t page_size);

    // 解压缩页数据
    std::vector<uint8_t> decompress_page_data(const uint8_t* compressed_data,
                                              size_t compressed_size,
                                              size_t expected_size);

    // 解析页中的记录
    std::vector<std::map<std::string, std::string>> parse_page_records(
        const std::vector<ScannedPage>& pages);
};

// ==================== CompressedTableRecovery 实现 ====================

inline CompressedTableRecovery::CompressedTableRecovery(size_t page_size)
    : page_size_(page_size), compressed_page_size_(16384) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void CompressedTableRecovery::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

inline void CompressedTableRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void CompressedTableRecovery::set_compressed_page_size(size_t compressed_size) {
    compressed_page_size_ = compressed_size;
}

inline void CompressedTableRecovery::clear() {
    compressed_pages_.clear();
    records_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

inline std::vector<ScannedPage> CompressedTableRecovery::scan_pages() {
    auto start = std::chrono::high_resolution_clock::now();

    compressed_pages_.clear();
    std::vector<ScannedPage> pages;

    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return pages;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    size_t total_pages = file_size / page_size_;
    stats_.total_pages = total_pages;

    if (total_pages == 0) {
        return pages;
    }

    std::vector<uint8_t> buffer(std::min(file_size, size_t(1024 * 1024)));

    size_t offset = 0;
    while (file.readsome(reinterpret_cast<char*>(buffer.data()), buffer.size())) {
        size_t read_size = file.gcount();

        for (size_t page_offset = 0; page_offset + page_size_ <= read_size; page_offset += page_size_) {
            const uint8_t* page_data = buffer.data() + page_offset;

            ScannedPage page{};
            page.offset = static_cast<uint32_t>(offset + page_offset);
            page.page_no = static_cast<uint32_t>(page_offset / page_size_);
            page.type = read_le<uint16_t>(page_data, 16);
            page.page_type = page.type;
            page.prev_page = read_le<uint32_t>(page_data, 0);
            page.next_page = read_le<uint32_t>(page_data, 4);
            page.lsn = read_le<uint64_t>(page_data, 8);
            page.space_id = read_le<uint32_t>(page_data, 24);
            page.is_valid = PageScanner::is_innodb_page(page_data, page_size_);

            // 检测是否压缩
            if (is_page_compressed(page_data, page_size_)) {
                page.is_compressed = true;
                compressed_pages_.push_back(page.page_no);
            }

            pages.push_back(page);
        }

        offset += read_size;
    }

    stats_.compressed_pages = compressed_pages_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.decompress_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return pages;
}

inline bool CompressedTableRecovery::is_page_compressed(const uint8_t* page_data, size_t page_size) {
    if (page_size < 38 + 32) {
        return false;  // Need at least FIL header (38 bytes) + ZSD header (32 bytes)
    }

    // Check page type is FIL_PAGE_COMPRESSED (0x45B5)
    uint16_t page_type = read_le<uint16_t>(page_data, 16);
    if (page_type != FIL_PAGE_COMPRESSED) {
        return false;
    }

    // Validate ZSD (Zero Space Descriptor) header at start of page data
    // ZSD layout (all little-endian):
    //   offset 0: zip_size (2 bytes) — compressed page size in 512-byte units
    //   offset 2: version (2 bytes) — compression version, 1 = InnoDB compression
    //   offset 8: zlen (2 bytes) — compressed data length
    //   offset 14: orig_size (4 bytes) — original uncompressed page size

    uint16_t version = read_le<uint16_t>(page_data, 2);
    if (version != 1) return false;

    uint16_t zip_size = read_le<uint16_t>(page_data, 0);
    // zip_size is in 512-byte units: 8=4KB, 16=8KB, 32=16KB
    if (zip_size != 8 && zip_size != 16 && zip_size != 32) return false;

    // zip_size must not exceed the page itself (can't be larger than the page)
    if (static_cast<uint32_t>(zip_size) * 512 > page_size) return false;

    uint32_t orig_size = read_le<uint32_t>(page_data, 14);
    // orig_size must be a valid InnoDB page size
    if (orig_size != 4096 && orig_size != 8192 && orig_size != 16384 &&
        orig_size != 32768 && orig_size != 65536) return false;

    uint16_t zlen = read_le<uint16_t>(page_data, 8);
    if (zlen == 0 || zlen > static_cast<uint16_t>(zip_size * 512)) return false;

    return true;
}

inline std::vector<uint8_t> CompressedTableRecovery::decompress_page_data(
    const uint8_t* compressed_data, size_t compressed_size, size_t expected_size) {

    auto start = std::chrono::high_resolution_clock::now();

    // ZSD header is 32 bytes at the start of the compressed page
    constexpr size_t ZSD_HEADER_SIZE = 32;
    if (compressed_size < ZSD_HEADER_SIZE) {
        return {};
    }

    // Read ZSD fields
    uint16_t zlen = read_le<uint16_t>(compressed_data, 8);
    uint16_t compression_type = read_le<uint16_t>(compressed_data, 18);

    // Compressed payload starts after ZSD header
    const uint8_t* payload = compressed_data + ZSD_HEADER_SIZE;
    size_t payload_size = std::min(static_cast<size_t>(zlen), compressed_size - ZSD_HEADER_SIZE);

    std::vector<uint8_t> decompressed(expected_size, 0);

    auto end_decompress = [&](bool success, size_t actual_size) {
        auto end = std::chrono::high_resolution_clock::now();
        stats_.decompress_time_ms += std::chrono::duration<double, std::milli>(end - start).count();
        if (success && actual_size == expected_size) {
            stats_.decompressed_pages++;
            return decompressed;
        }
        return std::vector<uint8_t>{};
    };

    // Try zlib first (compression_type 0 or unspecified)
    if (compression_type == 0) {
        uLong dest_len = static_cast<uLong>(expected_size);
        int result = uncompress(decompressed.data(), &dest_len,
                                payload, static_cast<uLong>(payload_size));
        if (result == Z_OK) {
            return end_decompress(true, dest_len);
        }
    }

    // Try LZ4 (compression_type 1 or fallback)
    // MySQL 8.0+ uses LZ4 by default for KEY_BLOCK_SIZE compression
    {
        auto lz4_result = dbrecovery::utils::LZ4Helper::decompress(
            payload, payload_size, expected_size);
        if (!lz4_result.empty()) {
            decompressed = std::move(lz4_result);
            return end_decompress(true, decompressed.size());
        }
    }

    // If zlib was not tried yet, try it as fallback
    if (compression_type != 0) {
        uLong dest_len = static_cast<uLong>(expected_size);
        int result = uncompress(decompressed.data(), &dest_len,
                                payload, static_cast<uLong>(payload_size));
        if (result == Z_OK) {
            return end_decompress(true, dest_len);
        }
    }

    return end_decompress(false, 0);
}

inline std::vector<std::map<std::string, std::string>> CompressedTableRecovery::parse_page_records(
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

        // 如果页压缩，尝试解压缩
        std::vector<uint8_t> working_data = page_data;
        if (page.is_compressed) {
            working_data = decompress_page_data(page_data.data(), page_data.size(), page_size_);
            if (working_data.empty()) {
                continue;
            }
        }

        // 解析页
        BPlusTreeParser parser(working_data);
        if (!parser.parse()) {
            continue;
        }

        // 解析记录
        auto page_records = parser.parse_records(schema_);
        records_.insert(records_.end(), page_records.begin(), page_records.end());
    }

    stats_.recovered_records = records_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_time_ms = stats_.decompress_time_ms + stats_.recovery_time_ms;

    return records_;
}

inline std::vector<std::map<std::string, std::string>> CompressedTableRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 扫描页
    auto all_pages = scan_pages();

    // 解析记录
    records_ = parse_page_records(all_pages);

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

// ==================== 压缩表恢复管理器 ====================
class CompressedTableRecoveryManager {
public:
    /**
     * @brief 构造函数
     */
    CompressedTableRecoveryManager();

    /**
     * @brief 添加压缩表
     */
    void add_compressed_table(const std::string& table_name,
                              uint32_t compressed_page_size);

    /**
     * @brief 设置表的压缩页大小
     */
    void set_compressed_page_size(const std::string& table_name, size_t size);

    /**
     * @brief 获取表的压缩页大小
     */
    std::optional<size_t> get_compressed_page_size(const std::string& table_name) const;

    /**
     * @brief 从 ibd 文件头解析压缩信息
     */
    std::optional<size_t> parse_compression_info(const std::string& ibd_path);

    /**
     * @brief 获取所有压缩表
     */
    const std::map<std::string, size_t>& get_tables() const { return tables_; }

private:
    std::map<std::string, size_t> tables_;
};

// ==================== CompressedTableRecoveryManager 实现 ====================

inline CompressedTableRecoveryManager::CompressedTableRecoveryManager() {}

inline void CompressedTableRecoveryManager::add_compressed_table(const std::string& table_name,
                                                                  uint32_t compressed_page_size) {
    tables_[table_name] = compressed_page_size;
}

inline void CompressedTableRecoveryManager::set_compressed_page_size(const std::string& table_name,
                                                                      size_t size) {
    tables_[table_name] = size;
}

inline std::optional<size_t> CompressedTableRecoveryManager::get_compressed_page_size(
    const std::string& table_name) const {
    auto it = tables_.find(table_name);
    if (it != tables_.end()) {
        return it->second;
    }
    return std::nullopt;
}

inline std::optional<size_t> CompressedTableRecoveryManager::parse_compression_info(
    const std::string& ibd_path) {
    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 16384) {
        return std::nullopt;
    }

    std::vector<uint8_t> page_data(16384);
    file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

    // 检查页 0 的 FSP_FLAGS (offset 80, 4 bytes, little-endian)
    if (page_data.size() >= 84) {  // need 4 bytes at offset 80
        uint32_t fsp_flags = read_le<uint32_t>(page_data.data(), 80);
        // FSP_FLAGS zip_ssize is at bits 5-7
        // zip_ssize = (fsp_flags >> 5) & 0x07
        // Actual compressed page size = UNIV_PAGE_SIZE / (2 ^ zip_ssize)
        // UNIV_PAGE_SIZE is typically 16384 (16KB)
        // zip_ssize=0 -> 16KB (no compression), zip_ssize=1 -> 8KB, zip_ssize=2 -> 4KB
        uint32_t zip_ssize = (fsp_flags >> 5) & 0x07;
        if (zip_ssize > 0) {
            return static_cast<size_t>(16384 / (1 << zip_ssize));
        }
    }

    return std::nullopt;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_COMPRESSED_TABLE_RECOVERY_HPP
