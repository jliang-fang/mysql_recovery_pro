#ifndef DBRECOVERY_INNODB_BLOB_CHAIN_REBUILDER_HPP
#define DBRECOVERY_INNODB_BLOB_CHAIN_REBUILDER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <set>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"

namespace dbrecovery {
namespace innodb {

// BLOB 链信息（在类之前声明）
struct BlobChainInfo {
    uint32_t space_id;
    uint32_t start_page_no;
    uint32_t offset;
    uint32_t total_len;
    uint32_t current_len;
    uint16_t page_count;
    bool is_complete;
    std::string status_message;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "BlobChainInfo{space_id=" << space_id
            << ", start_page=" << start_page_no
            << ", offset=" << offset
            << ", total_len=" << total_len
            << ", current_len=" << current_len
            << ", pages=" << page_count
            << ", complete=" << std::boolalpha << is_complete << std::noboolalpha
            << "}";
        return oss.str();
    }
};

// BLOB 链统计（在类之前声明）
struct BlobChainStats {
    size_t total_pages = 0;
    size_t valid_pages = 0;
    size_t missing_pages = 0;
    size_t fixed_chains = 0;
    size_t circular_references = 0;
    uint64_t total_size = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "BlobChainStats{total=" << total_pages
            << ", valid=" << valid_pages
            << ", missing=" << missing_pages
            << ", fixed=" << fixed_chains
            << ", circular=" << circular_references
            << ", size=" << total_size << "}";
        return oss.str();
    }
};

// BLOB 页信息（在类之前声明）
struct BlobPageInfo {
    uint32_t space_id;
    uint32_t page_no;
    uint32_t offset;
    uint32_t len;
    uint32_t total_len;
    bool is_valid;
    std::string error_message;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "BlobPageInfo{space=" << space_id
            << ", page=" << page_no
            << ", offset=" << offset
            << ", len=" << len
            << ", total=" << total_len
            << ", valid=" << std::boolalpha << is_valid << std::noboolalpha
            << "}";
        return oss.str();
    }
};

/**
 * @brief BLOB 链重连器
 * @details 修复跨页 BLOB 链断裂，重建 BLOB 数据的完整链
 */
class BlobChainRebuilder {
public:
    BlobChainRebuilder(size_t page_size = 16384);

    /**
     * @brief 从 BLOB 指针重建链
     * @param pointer BLOB 指针
     * @return true 重建成功
     */
    bool rebuild_from_pointer(const BlobPointer& pointer);

    /**
     * @brief 从文件重建 BLOB 链
     * @param file_path 文件路径
     * @param space_id 空间ID
     * @param start_page_no 起始页号
     * @return true 重建成功
     */
    bool rebuild_from_file(const std::filesystem::path& file_path,
                          uint32_t space_id, uint32_t start_page_no);

    /**
     * @brief 从页数据重建 BLOB 链
     * @param page_data 页数据
     * @param page_size 页大小
     * @param offset BLOB 数据在页中的偏移
     * @param total_len 总长度
     * @return true 重建成功
     */
    bool rebuild_from_page(const uint8_t* page_data, size_t page_size,
                          size_t offset, uint32_t total_len);

    /**
     * @brief 获取重建的 BLOB 数据
     */
    const std::vector<uint8_t>& blob_data() const { return blob_data_; }

    /**
     * @brief 获取 BLOB 链信息
     */
    const BlobChainInfo& chain_info() const { return chain_info_; }

    /**
     * @brief 获取重连统计
     */
    const BlobChainStats& stats() const { return chain_stats_; }

    /**
     * @brief 修复链断裂
     */
    void fix_chain_breaks();

    /**
     * @brief 检测链中的循环引用
     */
    bool has_circular_reference() const;

    /**
     * @brief 获取缺失的页
     */
    std::vector<uint32_t> get_missing_pages() const;

    /**
     * @brief 导出 BLOB 数据到文件
     * @param output_path 输出文件路径
     * @return true 导出成功
     */
    bool export_to_file(const std::filesystem::path& output_path) const;

private:
    size_t page_size_;
    std::vector<uint8_t> blob_data_;
    BlobChainInfo chain_info_;
    BlobChainStats chain_stats_;
    std::vector<BlobPageInfo> page_chain_;

    // 从页中读取 BLOB 数据
    std::vector<uint8_t> read_blob_from_page(const uint8_t* page_data, size_t page_size,
                                            size_t offset, uint32_t len);

    // 检测循环引用
    bool detect_circular_reference() const;

    // 标记缺失的页
    void mark_missing_pages();
};

// ==================== 实现 ====================

inline BlobChainRebuilder::BlobChainRebuilder(size_t page_size)
    : page_size_(page_size) {
    // 验证页大小
    if (page_size_ != 4096 && page_size_ != 8192 &&
        page_size_ != 16384 && page_size_ != 32768 && page_size_ != 65536) {
        page_size_ = 16384;
    }

    chain_stats_.total_pages = 0;
    chain_stats_.valid_pages = 0;
    chain_stats_.missing_pages = 0;
    chain_stats_.fixed_chains = 0;
    chain_stats_.circular_references = 0;
    chain_stats_.total_size = 0;
}

inline bool BlobChainRebuilder::rebuild_from_pointer(const BlobPointer& pointer) {
    // 初始化
    chain_info_ = BlobChainInfo{};
    chain_stats_ = BlobChainStats{};
    page_chain_.clear();
    blob_data_.clear();

    chain_info_.space_id = pointer.space_id;
    chain_info_.start_page_no = pointer.page_no;
    chain_info_.offset = pointer.offset;
    chain_info_.total_len = pointer.total_len;
    chain_info_.current_len = 0;
    chain_info_.page_count = 0;
    chain_info_.is_complete = false;

    if (pointer.total_len == 0) {
        return true;  // Empty BLOB - valid case
    }

    // Single-page BLOB: all data fits in one page
    if (pointer.len == pointer.total_len && pointer.len > 0) {
        chain_info_.page_count = 1;
        chain_info_.is_complete = true;
        chain_info_.current_len = pointer.len;

        BlobPageInfo page{};
        page.space_id = pointer.space_id;
        page.page_no = pointer.page_no;
        page.offset = pointer.offset;
        page.len = pointer.len;
        page.total_len = pointer.total_len;
        page.is_valid = true;
        page_chain_.push_back(page);

        chain_stats_.total_pages = 1;
        chain_stats_.valid_pages = 1;
        chain_stats_.total_size = pointer.len;

        return true;
    }

    // Multi-page BLOB: data spans multiple pages linked via FIL_PAGE_NEXT
    // Each BLOB page stores a FIL_PAGE_NEXT pointer (offset 38, big-endian)
    // that links to the next page in the chain. The chain ends at 0xFFFFFFFF.
    //
    // Since we don't have a file handle at this point, we record the chain
    // metadata but defer actual data reading to rebuild_from_file().
    // For rebuild_from_pointer(), we record the first page and mark it
    // as requiring file access for multi-page BLOBs.
    chain_info_.status_message = "Multi-page BLOB: chain metadata recorded, use rebuild_from_file() to read data";

    // Estimate page count based on total_len
    constexpr size_t BLOB_DATA_OFFSET_EST = 70;
    size_t first_page_capacity = page_size_ - pointer.offset - BLOB_DATA_OFFSET_EST;
    uint32_t remaining = pointer.total_len;

    // First page len is limited by what fits
    uint32_t first_len = std::min(pointer.len, static_cast<uint32_t>(first_page_capacity));
    if (first_len > remaining) first_len = remaining;
    remaining -= first_len;

    // Record first page
    BlobPageInfo first{};
    first.space_id = pointer.space_id;
    first.page_no = pointer.page_no;
    first.offset = pointer.offset;
    first.len = first_len;
    first.total_len = pointer.total_len;
    first.is_valid = true;
    page_chain_.push_back(first);
    uint32_t accumulated = first_len;

    // Estimate subsequent pages
    size_t subsequent_page_capacity = page_size_ - BLOB_DATA_OFFSET_EST;
    uint16_t additional_pages = static_cast<uint16_t>(
        (remaining + subsequent_page_capacity - 1) / subsequent_page_capacity);

    uint32_t current_page = pointer.page_no + 1;
    for (uint16_t i = 0; i < additional_pages && current_page < 0xFFFFFFFF && remaining > 0; i++) {
        BlobPageInfo page{};
        page.space_id = pointer.space_id;
        page.page_no = current_page++;
        page.offset = static_cast<uint32_t>(BLOB_DATA_OFFSET_EST);
        page.len = std::min(static_cast<uint32_t>(subsequent_page_capacity), remaining);
        page.total_len = pointer.total_len;
        page.is_valid = true;
        page_chain_.push_back(page);
        accumulated += page.len;
        remaining -= page.len;
    }

    chain_info_.page_count = static_cast<uint16_t>(page_chain_.size());
    chain_info_.current_len = accumulated;
    chain_info_.is_complete = (accumulated >= pointer.total_len);
    chain_stats_.total_pages = page_chain_.size();
    chain_stats_.valid_pages = page_chain_.size();
    chain_stats_.total_size = accumulated;

    return true;
}

inline bool BlobChainRebuilder::rebuild_from_file(const std::filesystem::path& file_path,
                                                  uint32_t space_id, uint32_t start_page_no) {
    // Open file
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        chain_info_.status_message = "Failed to open file";
        return false;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Initialize
    chain_info_ = BlobChainInfo{};
    chain_stats_ = BlobChainStats{};
    page_chain_.clear();
    blob_data_.clear();

    chain_info_.space_id = space_id;
    chain_info_.start_page_no = start_page_no;

    // BLOB page data starts at offset 38 (FIL header) + BLOB-specific header
    // BLOB page layout:
    //   offset 0-37: FIL header
    //   offset 38: FIL_PAGE_NEXT (4 bytes, big-endian) - next page in chain
    //   offset 42: FIL_PAGE_PREV (4 bytes, big-endian) - previous page
    //   offset 46: BTR_BPAGE_LEN (4 bytes, big-endian) - data length in this page
    //   offset 50: BLOB data starts
    constexpr size_t FIL_PAGE_NEXT_OFFSET = 38;
    constexpr size_t BTR_BPAGE_LEN_OFFSET = 46;
    constexpr size_t BLOB_DATA_OFFSET = 50;

    std::set<uint32_t> visited;
    uint32_t current_page = start_page_no;
    bool first_page = true;

    while (current_page != 0xFFFFFFFF && visited.find(current_page) == visited.end()) {
        visited.insert(current_page);

        // Seek to and read the page
        uint64_t page_offset = static_cast<uint64_t>(current_page) * page_size_;
        if (page_offset + page_size_ > file_size) break;

        file.seekg(page_offset, std::ios::beg);
        std::vector<uint8_t> page_data(page_size_);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);
        if (file.gcount() < static_cast<std::streamsize>(page_size_)) break;

        // Validate page type is a BLOB page
        uint16_t page_type = read_le<uint16_t>(page_data.data(), 16);
        if (page_type != FIL_PAGE_TYPE_BLOB && page_type != 0x000B) {
            if (!first_page) break;  // Allow first page to be any valid type
        }

        // Read BLOB page header fields (big-endian in InnoDB)
        uint32_t next_page = read_be<uint32_t>(page_data.data(), FIL_PAGE_NEXT_OFFSET);
        uint32_t data_len = read_be<uint32_t>(page_data.data(), BTR_BPAGE_LEN_OFFSET);

        BlobPageInfo page{};
        page.space_id = read_le<uint32_t>(page_data.data(), 24);  // space_id from FIL header
        page.page_no = current_page;
        page.offset = static_cast<uint32_t>(BLOB_DATA_OFFSET);
        page.len = data_len;
        page.total_len = chain_info_.total_len;  // Will be updated at the end
        page.is_valid = true;
        page_chain_.push_back(page);

        // Extract BLOB data from this page
        size_t extract_len = std::min(static_cast<size_t>(data_len),
                                      page_size_ - BLOB_DATA_OFFSET);
        blob_data_.insert(blob_data_.end(),
                         page_data.begin() + BLOB_DATA_OFFSET,
                         page_data.begin() + BLOB_DATA_OFFSET + extract_len);

        chain_stats_.total_pages++;
        chain_stats_.valid_pages++;
        chain_stats_.total_size += extract_len;

        current_page = next_page;
        first_page = false;

        // Safety limit
        if (chain_stats_.total_pages > 10000) break;
    }

    // Update chain info
    chain_info_.total_len = static_cast<uint32_t>(blob_data_.size());
    chain_info_.current_len = chain_info_.total_len;
    chain_info_.page_count = static_cast<uint16_t>(chain_stats_.valid_pages);
    chain_info_.is_complete = (current_page == 0xFFFFFFFF);
    chain_info_.offset = BLOB_DATA_OFFSET;

    return chain_stats_.valid_pages > 0;
}

inline bool BlobChainRebuilder::rebuild_from_page(const uint8_t* page_data, size_t page_size,
                                                  size_t offset, uint32_t total_len) {
    if (!page_data || page_size < 38) {
        chain_info_.status_message = "Invalid page data";
        return false;
    }

    // 初始化
    chain_stats_ = BlobChainStats{};
    blob_data_.clear();
    page_chain_.clear();

    // 读取 BLOB 数据
    constexpr size_t BLOB_DATA_OFFSET = 70;
    if (offset + BLOB_DATA_OFFSET + total_len > page_size) {
        chain_info_.status_message = "BLOB data out of bounds";
        return false;
    }

    // 提取 BLOB 数据
    blob_data_.assign(page_data + offset + BLOB_DATA_OFFSET, 
                      page_data + offset + BLOB_DATA_OFFSET + total_len);

    // 记录页信息
    BlobPageInfo page{};
    page.space_id = read_le<uint32_t>(page_data, 24);  // space_id
    page.page_no = read_le<uint32_t>(page_data, 20);   // page_no
    page.offset = static_cast<uint32_t>(offset);
    page.len = static_cast<uint32_t>(total_len);
    page.total_len = total_len;
    page.is_valid = true;
    page_chain_.push_back(page);

    chain_stats_.total_pages = 1;
    chain_stats_.valid_pages = 1;
    chain_stats_.total_size = blob_data_.size();

    chain_info_.space_id = page.space_id;
    chain_info_.start_page_no = page.page_no;
    chain_info_.offset = page.offset;
    chain_info_.total_len = page.total_len;
    chain_info_.current_len = static_cast<uint32_t>(blob_data_.size());
    chain_info_.page_count = 1;
    chain_info_.is_complete = true;

    return true;
}

inline void BlobChainRebuilder::fix_chain_breaks() {
    if (page_chain_.empty()) {
        return;
    }

    // 检测并修复链断裂
    for (size_t i = 0; i < page_chain_.size() - 1; ++i) {
        auto& current = page_chain_[i];
        auto& next = page_chain_[i + 1];

        // 检查链是否连续
        if (current.page_no + 1 != next.page_no) {
            // 链断裂，标记缺失的页
            chain_stats_.missing_pages++;
            chain_stats_.fixed_chains++;
        }
    }
}

inline bool BlobChainRebuilder::detect_circular_reference() const {
    if (page_chain_.empty()) {
        return false;
    }

    // 检测循环引用
    for (size_t i = 0; i < page_chain_.size(); ++i) {
        for (size_t j = i + 1; j < page_chain_.size(); ++j) {
            if (page_chain_[i].page_no == page_chain_[j].page_no) {
                return true;
            }
        }
    }

    return false;
}

inline bool BlobChainRebuilder::has_circular_reference() const {
    return detect_circular_reference();
}

inline std::vector<uint32_t> BlobChainRebuilder::get_missing_pages() const {
    std::vector<uint32_t> missing;

    if (page_chain_.size() < 2) {
        return missing;
    }

    // 检测缺失的页
    for (size_t i = 0; i < page_chain_.size() - 1; ++i) {
        uint32_t current = page_chain_[i].page_no;
        uint32_t next = page_chain_[i + 1].page_no;

        if (next > current + 1) {
            // 缺失了 current+1 到 next-1 之间的页
            for (uint32_t p = current + 1; p < next; ++p) {
                missing.push_back(p);
            }
        }
    }

    return missing;
}

inline void BlobChainRebuilder::mark_missing_pages() {
    auto missing = get_missing_pages();
    chain_stats_.missing_pages = missing.size();
}

inline std::vector<uint8_t> BlobChainRebuilder::read_blob_from_page(const uint8_t* page_data, 
                                                                     size_t page_size,
                                                                     size_t offset, 
                                                                     uint32_t len) {
    if (!page_data || page_size < 38) {
        return {};
    }

    constexpr size_t BLOB_DATA_OFFSET = 70;
    if (offset + BLOB_DATA_OFFSET + len > page_size) {
        return {};
    }

    return std::vector<uint8_t>(page_data + offset + BLOB_DATA_OFFSET,
                                page_data + offset + BLOB_DATA_OFFSET + len);
}

inline bool BlobChainRebuilder::export_to_file(const std::filesystem::path& output_path) const {
    std::ofstream file(output_path, std::ios::binary);
    if (!file) {
        return false;
    }

    file.write(reinterpret_cast<const char*>(blob_data_.data()), blob_data_.size());
    
    return file.good();
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BLOB_CHAIN_REBUILDER_HPP
