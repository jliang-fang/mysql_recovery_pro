#ifndef DBRECOVERY_INNODB_PAGE_SCANNER_HPP
#define DBRECOVERY_INNODB_PAGE_SCANNER_HPP

#include <vector>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <fstream>
#include <sstream>
#include <iomanip>
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

// 扫描结果
struct ScannedPage {
    uint32_t offset;        // 页在文件中的偏移
    uint16_t type;          // 页类型
    uint16_t level;         // 页级别（B+Tree层数）
    uint32_t page_no;       // 页号
    uint32_t prev_page;     // 前一个页
    uint32_t next_page;     // 后一个页
    bool is_valid;          // 是否有效页
    uint64_t lsn;           // LSN
    uint32_t space_id;      // 空间ID

    std::string to_string() const {
        std::ostringstream oss;
        oss << "ScannedPage{offset=" << offset
            << ", type=0x" << std::hex << type << std::dec
            << ", level=" << level
            << ", page_no=" << page_no
            << ", prev=" << prev_page
            << ", next=" << next_page
            << ", valid=" << is_valid << "}";
        return oss.str();
    }
};

// 扫描统计
struct ScanStats {
    size_t total_pages = 0;
    size_t valid_pages = 0;
    size_t index_pages = 0;
    size_t sdi_pages = 0;
    size_t undo_pages = 0;
    size_t blob_pages = 0;
    size_t allocated_pages = 0;
    size_t invalid_pages = 0;
    uint64_t total_size = 0;
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "ScanStats{total=" << total_pages
            << ", valid=" << valid_pages
            << ", index=" << index_pages
            << ", sdi=" << sdi_pages
            << ", undo=" << undo_pages
            << ", blob=" << blob_pages
            << ", allocated=" << allocated_pages
            << ", invalid=" << invalid_pages << "}";
        return oss.str();
    }
};

/**
 * @brief InnoDB 页扫描器
 * @details 批量扫描文件中的所有 InnoDB 页，构建页链关系
 */
class PageScanner {
public:
    explicit PageScanner(size_t page_size = 16384);
    
    /**
     * @brief 扫描整个文件
     */
    std::vector<ScannedPage> scan_file(const std::filesystem::path& file_path);
    
    /**
     * @brief 扫描内存缓冲区
     */
    std::vector<ScannedPage> scan_buffer(const uint8_t* data, size_t size);
    
    /**
     * @brief 获取扫描统计
     */
    const ScanStats& stats() const { return stats_; }
    
    /**
     * @brief 检查是否为 InnoDB 页
     */
    static bool is_innodb_page(const uint8_t* data, size_t page_size = 16384);
    
    /**
     * @brief 检查页类型是否有效
     */
    static bool is_valid_page_type(uint16_t type);

private:
    size_t page_size_;
    ScanStats stats_;
    
    // 扫描单个页
    std::optional<ScannedPage> scan_single_page(const uint8_t* data, size_t file_offset);

    // 跳过非 InnoDB 页
    size_t skip_non_innodb_pages(const uint8_t* data, size_t size);
};

// ==================== 实现 ====================

inline PageScanner::PageScanner(size_t page_size)
    : page_size_(page_size) {
    // 验证页大小
    if (page_size_ != 4096 && page_size_ != 8192 && 
        page_size_ != 16384 && page_size_ != 32768 && page_size_ != 65536) {
        page_size_ = 16384;  // 默认 16KB
    }
}

inline bool PageScanner::is_valid_page_type(uint16_t type) {
    // MySQL 5.6 和 8.0 的页类型兼容性
    switch (type) {
        case FIL_PAGE_TYPE_ALLOCATED:
        case FIL_PAGE_INDEX:
        case 17852:  // MySQL 5.6 索引页 (0x45BC)
        case FIL_PAGE_UNDO_LOG:
        case FIL_PAGE_SDI:
        case FIL_PAGE_TYPE_BLOB:
        case FIL_PAGE_COMPRESSED:
            return true;
        default:
            return false;
    }
}

inline bool PageScanner::is_innodb_page(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 38) {
        return false;
    }

    // 读取页类型 - 根据 page_defs.hpp，FIL_PAGE_TYPE 在偏移 24
    // InnoDB FIL header fields are stored in BIG-ENDIAN format
    uint16_t type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    return is_valid_page_type(type);
}

inline std::optional<ScannedPage> PageScanner::scan_single_page(const uint8_t* data, size_t file_offset = 0) {
    if (!data || page_size_ < 38) {
        return std::nullopt;
    }

    ScannedPage page{};
    page.offset = static_cast<uint32_t>(file_offset);  // 文件中的绝对偏移
    page.is_valid = false;

    // 读取 FIL Header - 偏移量参考 page_defs.hpp
    // InnoDB FIL header fields are stored in BIG-ENDIAN format
    // 0-3:   space_id (FIL_PAGE_SPACE_ID = 0)
    // 4-7:   page_no (FIL_PAGE_OFFSET = 4)
    // 8-15:  prev_page (4 bytes) + next_page (4 bytes) (FIL_PAGE_PREV = 8, FIL_PAGE_NEXT = 12)
    // 16-23: lsn (FIL_PAGE_LSN = 16, 8 bytes)
    // 24-25: type (FIL_PAGE_TYPE = 24)
    // 26-27: level (FIL_PAGE_LEVEL = 26)
    page.space_id = read_be<uint32_t>(data, 0);
    page.page_no = read_be<uint32_t>(data, 4);
    page.prev_page = read_be<uint32_t>(data, 8);
    page.next_page = read_be<uint32_t>(data, 12);
    page.lsn = read_be<uint64_t>(data, 16);
    page.type = read_be<uint16_t>(data, 24);
    page.level = read_be<uint16_t>(data, 26);  // 页级别

    // 验证页类型
    if (!is_valid_page_type(page.type)) {
        return page;  // 标记为无效
    }

    // 验证页号范围（允许 0xFFFFFFFF 作为特殊值）
    if (page.page_no >= 0xFFFFFFF0 && page.page_no != 0xFFFFFFFF) {
        return page;
    }

    // 验证页链关系（仅当 prev/next 有效时）
    // prev_page >= page_no 表示链表方向错误
    if (page.prev_page != 0xFFFFFFFF && page.prev_page >= page.page_no) {
        // 这可能是合理的（如循环链表），仅警告不拒绝
    }

    // next_page <= page_no 表示链表方向错误
    if (page.next_page != 0xFFFFFFFF && page.next_page <= page.page_no) {
        // 这可能是合理的（如循环链表），仅警告不拒绝
    }

    page.is_valid = true;
    return page;
}

inline std::vector<ScannedPage> PageScanner::scan_buffer(const uint8_t* data, size_t size) {
    std::vector<ScannedPage> pages;
    stats_ = ScanStats{};
    
    if (!data || size < page_size_) {
        return pages;
    }
    
    stats_.total_size = size;
    
    // 扫描每个页
    for (size_t offset = 0; offset + page_size_ <= size; offset += page_size_) {
        auto page_opt = scan_single_page(data + offset);

        if (page_opt.has_value()) {
            auto& page = page_opt.value();
            pages.push_back(page);
            stats_.total_pages++;

            if (page.is_valid) {
                stats_.valid_pages++;

                // 统计页类型（支持 MySQL 5.6 和 8.0）
                switch (page.type) {
                    case 17852:  // MySQL 5.6 索引页 (0x45BC)
                    case FIL_PAGE_INDEX:
                        stats_.index_pages++;
                        break;
                    case FIL_PAGE_SDI:
                        stats_.sdi_pages++;
                        break;
                    case FIL_PAGE_UNDO_LOG:
                        stats_.undo_pages++;
                        break;
                    case FIL_PAGE_TYPE_BLOB:
                        stats_.blob_pages++;
                        break;
                    case FIL_PAGE_TYPE_ALLOCATED:
                        stats_.allocated_pages++;
                        break;
                    default:
                        break;
                }
            } else {
                stats_.invalid_pages++;
            }
        }
    }

    return pages;
}

inline std::vector<ScannedPage> PageScanner::scan_file(const std::filesystem::path& file_path) {
    std::vector<ScannedPage> pages;

    // 打开文件
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return pages;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 分配缓冲区
    std::vector<uint8_t> buffer(file_size);
    
    // 读取整个文件
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    size_t actual_read = file.gcount();
    
    if (actual_read == 0) {
        return pages;
    }

    // 扫描
    size_t offset = 0;
    while (offset + page_size_ <= actual_read) {
        auto page_opt = scan_single_page(buffer.data() + offset, offset);

        if (page_opt.has_value()) {
            auto& page = page_opt.value();
            pages.push_back(page);
            stats_.total_pages++;

            if (page.is_valid) {
                stats_.valid_pages++;

                // 统计页类型
                switch (page.type) {
                    case FIL_PAGE_INDEX:
                        stats_.index_pages++;
                        break;
                    case FIL_PAGE_SDI:
                        stats_.sdi_pages++;
                        break;
                    case FIL_PAGE_UNDO_LOG:
                        stats_.undo_pages++;
                        break;
                    case FIL_PAGE_TYPE_BLOB:
                        stats_.blob_pages++;
                        break;
                    case FIL_PAGE_TYPE_ALLOCATED:
                        stats_.allocated_pages++;
                        break;
                    default:
                        break;
                }
            } else {
                stats_.invalid_pages++;
            }
        }
        
        offset += page_size_;
    }

    stats_.total_size = file_size;
    return pages;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_SCANNER_HPP
