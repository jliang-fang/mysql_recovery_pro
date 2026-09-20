#ifndef DBRECOVERY_DISK_ENHANCED_SCANNER_HPP
#define DBRECOVERY_DISK_ENHANCED_SCANNER_HPP

#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"
#include "dbrecovery/disk/ibd_signatures.hpp"
#include "dbrecovery/disk/raw_scanner.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dbrecovery {
namespace disk {

// ==================== 增强的磁盘扫描器 ====================
// 全分区扫描和 PhotoRec 文件雕刻结合

/// @brief 找到的 IBD 块信息
struct FoundIbdBlock {
    uint64_t disk_offset;       // 在磁盘/镜像中的偏移
    uint32_t page_size;         // 页大小
    uint32_t page_number;       // 页号
    uint32_t space_id;          // 空间 ID
    uint16_t page_type;         // 页类型
    std::vector<uint8_t> data;  // 页数据
};

/// @brief 找到的完整 IBD 文件
struct FoundIbdFile {
    uint64_t disk_offset;       // 在磁盘/镜像中的起始偏移
    uint64_t file_size;         // 文件大小
    uint32_t page_size;         // 页大小
    uint32_t space_id;          // 空间 ID
    uint32_t num_pages;         // 页数
    uint32_t index_pages;       // 索引页数
    uint32_t sdi_pages;         // SDI 页数
    std::vector<uint8_t> data;  // 文件数据
    std::vector<uint32_t> page_numbers; // 所有页号（排序后）
};

/// @brief 增强的磁盘扫描器
class EnhancedScanner {
public:
    EnhancedScanner();
    ~EnhancedScanner();

    // 扫描磁盘/镜像文件
    bool scan(const std::string& disk_path, uint64_t max_scan_size = 0);

    // 接管已授权的只读来源描述符
    bool adopt_fd(int owned_fd, uint64_t known_size, uint64_t max_scan_size = 0);

    // 使用 IBD 签名扫描
    std::vector<FoundIbdBlock> scan_for_ibd_blocks(uint32_t min_pages = 2);

    // PhotoRec 风格的文件雕刻
    std::vector<FoundIbdFile> carve_ibd_files(uint32_t min_pages = 2);

    // 使用已知表结构作为模板扫描已打开来源
    std::vector<FoundIbdBlock> scan_with_table_template(
        const std::vector<std::pair<std::string, uint8_t>>& column_definitions,
        uint32_t min_pages = 2);

    // 使用已知表结构作为模板扫描
    std::vector<FoundIbdBlock> scan_with_table_template(
        const std::string& disk_path,
        const std::vector<std::pair<std::string, uint8_t>>& column_definitions,
        uint32_t min_pages = 2);

    // 聚合找到的块为完整文件
    std::vector<FoundIbdFile> aggregate_blocks(
        const std::vector<FoundIbdBlock>& blocks,
        uint32_t min_pages = 2);

    // 获取扫描统计
    struct ScanStats {
        uint64_t total_bytes_scanned;
        uint32_t total_pages_found;
        uint32_t valid_ibd_pages;
        uint32_t index_pages;
        uint32_t sdi_pages;
        uint32_t undo_pages;
        uint32_t allocated_pages;
        uint32_t files_found;
        double scan_time_seconds;
    };

    const ScanStats& get_stats() const { return stats_; }

    // 获取找到的文件
    const std::vector<FoundIbdFile>& found_files() const { return found_files_; }

    // 回调函数类型
    using BlockFoundCallback = std::function<void(const FoundIbdBlock&)>;
    using FileFoundCallback = std::function<void(const FoundIbdFile&)>;

    // 设置回调
    void set_block_callback(BlockFoundCallback cb) { block_callback_ = cb; }
    void set_file_callback(FileFoundCallback cb) { file_callback_ = cb; }

    // 设置页大小（用于聚合时）
    void set_page_size(uint32_t page_size) { default_page_size_ = page_size; }

    // 扫描单个页大小
    std::vector<FoundIbdBlock> scan_page_size(uint32_t page_size, uint64_t max_pages);

    // 聚合同一个文件的块
    std::vector<std::vector<FoundIbdBlock>> group_blocks_by_space_id(
        const std::vector<FoundIbdBlock>& blocks);

    // 检查页数据是否与表模板匹配
    bool page_matches_table_template(const std::vector<uint8_t>& page_data,
                                     const std::vector<std::pair<std::string, uint8_t>>& columns);

private:
    std::unique_ptr<RawScanner> scanner_;
    std::vector<FoundIbdFile> found_files_;
    ScanStats stats_;
    BlockFoundCallback block_callback_;
    FileFoundCallback file_callback_;

    // 预定义的页大小
    static constexpr std::array<uint32_t, 5> PAGE_SIZES{4096, 8192, 16384, 32768, 65536};

    uint32_t default_page_size_ = 16384;
    uint64_t scan_limit_ = 0;
};

// ==================== 实现 ====================

inline EnhancedScanner::EnhancedScanner() : scanner_(std::make_unique<RawScanner>()) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline EnhancedScanner::~EnhancedScanner() = default;

inline bool EnhancedScanner::scan(const std::string& disk_path, uint64_t max_scan_size) {
    if (!scanner_->open(disk_path)) {
        return false;
    }

    uint64_t disk_size = scanner_->get_size();
    scan_limit_ = max_scan_size > 0 ? std::min(max_scan_size, disk_size) : disk_size;
    stats_.total_bytes_scanned = scan_limit_;

    return true;
}

inline bool EnhancedScanner::adopt_fd(int owned_fd, uint64_t known_size, uint64_t max_scan_size) {
    if (!scanner_->adopt_fd(owned_fd, known_size)) {
        return false;
    }

    scan_limit_ = max_scan_size > 0 ? std::min(max_scan_size, known_size) : known_size;
    stats_.total_bytes_scanned = scan_limit_;
    return true;
}

inline std::vector<FoundIbdBlock> EnhancedScanner::scan_page_size(uint32_t page_size, uint64_t max_pages) {
    std::vector<FoundIbdBlock> blocks;

    if (!scanner_ || !scanner_->is_open()) {
        return blocks;
    }

    uint64_t disk_size = std::min(scanner_->get_size(), scan_limit_);
    if (page_size == 0) {
        return blocks;
    }
    if (max_pages == 0) {
        max_pages = disk_size / page_size;
    }

    std::vector<uint8_t> page_data(page_size);

    uint64_t num_pages = std::min(max_pages, disk_size / page_size);

    for (uint64_t page_no = 0; page_no < num_pages; ++page_no) {
        uint64_t offset = page_no * page_size;

        if (offset + page_size > disk_size) {
            break;
        }

        size_t read_size = scanner_->read(offset, page_data.data(), page_size);
        if (read_size != page_size) {
            continue;
        }

        // 检查页类型
        uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), 24);

        if (!innodb::is_valid_page_type(page_type)) {
            continue;
        }

        // 提取元数据
        uint32_t space_id = innodb::read_be<uint32_t>(page_data.data(), 34);
        uint32_t page_number = innodb::read_be<uint32_t>(page_data.data(), 4);

        // 跳过无效的页号
        if (page_number > 1000000) {
            continue;
        }

        // 记录统计
        stats_.total_pages_found++;
        stats_.valid_ibd_pages++;

        switch (page_type) {
            case innodb::FIL_PAGE_INDEX:
                stats_.index_pages++;
                break;
            case innodb::FIL_PAGE_SDI:
            case innodb::FIL_PAGE_SDI_ZIP:
                stats_.sdi_pages++;
                break;
            case innodb::FIL_PAGE_UNDO_LOG:
                stats_.undo_pages++;
                break;
            case innodb::FIL_PAGE_TYPE_ALLOCATED:
                stats_.allocated_pages++;
                break;
        }

        // 回调
        FoundIbdBlock block;
        block.disk_offset = offset;
        block.page_size = page_size;
        block.page_number = page_number;
        block.space_id = space_id;
        block.page_type = page_type;
        block.data = page_data;

        if (block_callback_) {
            block_callback_(block);
        }

        blocks.push_back(std::move(block));
    }

    return blocks;
}

inline std::vector<FoundIbdBlock> EnhancedScanner::scan_for_ibd_blocks(uint32_t min_pages) {
    std::vector<FoundIbdBlock> all_blocks;

    // 扫描所有可能的页大小
    for (uint32_t page_size : PAGE_SIZES) {
        auto blocks = scan_page_size(page_size, 0);
        all_blocks.insert(all_blocks.end(), blocks.begin(), blocks.end());
    }

    // 按空间 ID 和页号排序
    std::sort(all_blocks.begin(), all_blocks.end(),
              [](const FoundIbdBlock& a, const FoundIbdBlock& b) {
                  if (a.space_id != b.space_id) {
                      return a.space_id < b.space_id;
                  }
                  return a.page_number < b.page_number;
              });

    return all_blocks;
}

inline std::vector<std::vector<FoundIbdBlock>> EnhancedScanner::group_blocks_by_space_id(
    const std::vector<FoundIbdBlock>& blocks) {
    std::unordered_map<uint32_t, std::vector<FoundIbdBlock>> groups;

    for (const auto& block : blocks) {
        groups[block.space_id].push_back(block);
    }

    std::vector<std::vector<FoundIbdBlock>> result;
    result.reserve(groups.size());

    for (auto& [space_id, group_blocks] : groups) {
        // 按页号排序
        std::sort(group_blocks.begin(), group_blocks.end(),
                  [](const FoundIbdBlock& a, const FoundIbdBlock& b) {
                      return a.page_number < b.page_number;
                  });
        result.push_back(std::move(group_blocks));
    }

    return result;
}

inline std::vector<FoundIbdFile> EnhancedScanner::aggregate_blocks(
    const std::vector<FoundIbdBlock>& blocks,
    uint32_t min_pages) {
    std::vector<FoundIbdFile> files;

    auto grouped = group_blocks_by_space_id(blocks);

    for (auto& group : grouped) {
        if (group.size() < min_pages) {
            continue;
        }

        FoundIbdFile file;
        file.disk_offset = group[0].disk_offset;
        file.page_size = group[0].page_size;
        file.space_id = group[0].space_id;
        file.num_pages = group.size();

        // 计算文件大小
        file.file_size = file.num_pages * file.page_size;
        file.data.resize(file.file_size);
        std::fill(file.data.begin(), file.data.end(), 0);

        // 统计页类型
        for (const auto& block : group) {
            file.page_numbers.push_back(block.page_number);

            switch (block.page_type) {
                case innodb::FIL_PAGE_INDEX:
                    file.index_pages++;
                    break;
                case innodb::FIL_PAGE_SDI:
                case innodb::FIL_PAGE_SDI_ZIP:
                    file.sdi_pages++;
                    break;
            }

            // 填充文件数据
            size_t offset = block.page_number * file.page_size;
            if (offset + block.data.size() <= file.data.size()) {
                std::memcpy(file.data.data() + offset, block.data.data(), block.data.size());
            }
        }

        // 按页号排序
        std::sort(file.page_numbers.begin(), file.page_numbers.end());

        if (file_callback_) {
            file_callback_(file);
        }

        files.push_back(std::move(file));
    }

    stats_.files_found = files.size();

    return files;
}

inline std::vector<FoundIbdFile> EnhancedScanner::carve_ibd_files(uint32_t min_pages) {
    auto blocks = scan_for_ibd_blocks(min_pages);
    return aggregate_blocks(blocks, min_pages);
}

inline bool EnhancedScanner::page_matches_table_template(
    const std::vector<uint8_t>& page_data,
    const std::vector<std::pair<std::string, uint8_t>>& columns) {
    // 这是一个简化的模板匹配
    // 实际实现需要根据表结构解析页中的记录

    // 基本检查：页类型必须是索引页
    uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), 24);
    if (!innodb::is_valid_index_page_type(page_type)) {
        return false;
    }

    // 如果提供了列定义，可以进一步检查记录格式
    // 例如：检查是否有预期的字段数量

    return true;
}

inline std::vector<FoundIbdBlock> EnhancedScanner::scan_with_table_template(
    const std::vector<std::pair<std::string, uint8_t>>& column_definitions,
    uint32_t min_pages) {
    if (!scanner_ || !scanner_->is_open()) {
        return {};
    }

    auto blocks = scan_for_ibd_blocks(min_pages);

    std::vector<FoundIbdBlock> filtered;
    for (const auto& block : blocks) {
        if (page_matches_table_template(block.data, column_definitions)) {
            filtered.push_back(block);
        }
    }

    return filtered;
}

inline std::vector<FoundIbdBlock> EnhancedScanner::scan_with_table_template(
    const std::string& disk_path,
    const std::vector<std::pair<std::string, uint8_t>>& column_definitions,
    uint32_t min_pages) {
    if (!scan(disk_path)) {
        return {};
    }

    return scan_with_table_template(column_definitions, min_pages);
}

} // namespace disk
} // namespace dbrecovery

#endif // DBRECOVERY_DISK_ENHANCED_SCANNER_HPP
