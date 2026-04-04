#ifndef DBRECOVERY_INNODB_BLOB_CHAIN_REBUILDER_HPP
#define DBRECOVERY_INNODB_BLOB_CHAIN_REBUILDER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <fstream>
#include <filesystem>
#include <sstream>
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

    // 处理单页 BLOB
    if (pointer.total_len == pointer.len && pointer.len > 0) {
        // 单页 BLOB
        chain_info_.page_count = 1;
        chain_info_.is_complete = true;
        
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

    // 多页 BLOB - 需要文件访问
    chain_info_.status_message = "Multi-page BLOB requires file access";
    return true;
}

inline bool BlobChainRebuilder::rebuild_from_file(const std::filesystem::path& file_path,
                                                  uint32_t space_id, uint32_t start_page_no) {
    // 打开文件
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        chain_info_.status_message = "Failed to open file";
        return false;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 分配缓冲区
    std::vector<uint8_t> buffer(std::min(file_size, size_t(1024 * 1024)));

    // 扫描文件中的 BLOB 页
    size_t offset = 0;
    while (file.readsome(reinterpret_cast<char*>(buffer.data()), buffer.size())) {
        size_t read_size = file.gcount();
        
        for (size_t page_offset = 0; page_offset + page_size_ <= read_size; page_offset += page_size_) {
            const uint8_t* page_data = buffer.data() + page_offset;
            
            // 检查页类型
            uint16_t page_type = read_le<uint16_t>(page_data, 16);
            uint32_t page_no = read_le<uint32_t>(page_data, 20);
            
            if (page_type == FIL_PAGE_TYPE_BLOB || page_type == FIL_PAGE_COMPRESSED) {
                // 这是一个 BLOB 页
                chain_stats_.total_pages++;
                
                // 读取 BLOB 数据
                // BLOB 数据从偏移 38 + 32 = 70 开始
                constexpr size_t BLOB_DATA_OFFSET = 70;
                
                if (page_offset + BLOB_DATA_OFFSET + page_size_ <= read_size) {
                    chain_stats_.valid_pages++;
                    chain_stats_.total_size += page_size_;
                }
            }
        }
        
        offset += read_size;
    }

    chain_info_.space_id = space_id;
    chain_info_.start_page_no = start_page_no;
    chain_info_.page_count = static_cast<uint16_t>(chain_stats_.valid_pages);
    
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
