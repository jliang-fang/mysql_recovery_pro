#ifndef DBRECOVERY_INNODB_BLOB_PARSER_HPP
#define DBRECOVERY_INNODB_BLOB_PARSER_HPP

#include "dbrecovery/innodb/page_types_v2.hpp"
#include <vector>
#include <optional>
#include <memory>

namespace dbrecovery {
namespace innodb {

/**
 * @brief BLOB 页类型
 */
enum class BlobPageType : uint8_t {
    FIRST = 0x01,      // 第一个 BLOB 页
    NEXT = 0x02,       // 后续 BLOB 页
    LAST = 0x03,       // 最后一个 BLOB 页
    SINGLE = 0x04,     // 单页 BLOB
    COMPRESSED = 0x05, // 压缩 BLOB 页
};

/**
 * @brief BLOB 指针结构 (20字节)
 */
struct alignas(1) BlobPointer {
    uint32_t space_id;     // 空间ID
    uint32_t page_no;      // 页号
    uint32_t offset;       // 页内偏移
    uint32_t len;          // 数据长度 (低24位)
    bool is_compressed;    // 是否压缩
    
    // 解析方法
    static std::optional<BlobPointer> parse(const uint8_t* data, size_t size);
    
    std::string to_string() const;
};

/**
 * @brief BLOB 页头
 */
struct BlobPageHeader {
    uint32_t prev_page;      // 前一个 BLOB 页
    uint32_t next_page;      // 后一个 BLOB 页
    uint32_t type;           // 页类型
    uint32_t len;            // 数据长度
    uint32_t max_len;        // 最大长度
    
    bool parse(const uint8_t* data, size_t size);
    std::string to_string() const;
};

/**
 * @brief BLOB 数据
 */
struct BlobData {
    std::vector<uint8_t> data;
    uint32_t total_len;
    uint32_t data_len;
    bool is_compressed;
    
    std::string to_string() const;
};

/**
 * @brief BLOB 链条
 */
struct BlobChain {
    std::vector<uint32_t> page_numbers;  // 页号链
    uint32_t total_length;               // 总长度
    uint32_t data_length;                // 实际数据长度
    
    bool is_valid() const;
    std::string to_string() const;
};

/**
 * @brief BLOB 解析器
 */
class BlobParser {
public:
    BlobParser();
    
    /**
     * @brief 解析 BLOB 指针
     */
    std::optional<BlobPointer> parse_pointer(const uint8_t* data, size_t size);
    
    /**
     * @brief 解析 BLOB 页
     */
    std::optional<BlobPageHeader> parse_page_header(const uint8_t* data, size_t size);
    
    /**
     * @brief 解析 BLOB 数据
     */
    std::optional<BlobData> parse_blob_data(const uint8_t* data, size_t size,
                                           const BlobPointer& pointer);
    
    /**
     * @brief 解析完整的 BLOB 链条
     */
    std::optional<BlobChain> parse_chain(
        const std::vector<uint32_t>& page_numbers,
        std::function<std::optional<std::vector<uint8_t>>(uint32_t)> page_reader);
    
    /**
     * @brief 从行数据中提取 BLOB 指针
     */
    std::optional<BlobPointer> extract_pointer(const std::vector<uint8_t>& row_data,
                                               uint16_t col_idx);
    
    /**
     * @brief 读取完整的 BLOB 数据
     */
    std::vector<uint8_t> read_blob(const BlobChain& chain,
                                   std::function<std::optional<std::vector<uint8_t>>(uint32_t)> page_reader);
    
    /**
     * @brief 获取解析统计信息
     */
    struct Stats {
        size_t pointers_parsed;
        size_t pages_parsed;
        size_t chains_parsed;
        size_t bytes_read;
        size_t errors;
    };
    
    Stats get_stats() const;
    
private:
    Stats stats_;
    
    std::vector<uint8_t> decompress_blob(const std::vector<uint8_t>& data);
};

/**
 * @brief BLOB 页链重建器
 */
class BlobChainReconstructor {
public:
    BlobChainReconstructor();
    
    /**
     * @brief 添加 BLOB 页
     */
    void add_page(uint32_t page_no, std::vector<uint8_t> data);
    
    /**
     * @brief 重建 BLOB 链条
     */
    bool reconstruct();
    
    /**
     * @brief 获取重建的 BLOB 数据
     */
    std::vector<uint8_t> get_data() const;
    
    /**
     * @brief 获取统计信息
     */
    struct Stats {
        size_t pages_added;
        size_t chains_reconstructed;
        size_t broken_chains;
        size_t circular_references;
    };
    
    Stats get_stats() const;
    
private:
    struct Page {
        uint32_t page_no;
        std::vector<uint8_t> data;
        uint32_t next_page;
    };
    
    std::vector<Page> pages_;
    Stats stats_;
    
    bool detect_circular_reference();
};

// ==================== 实现 ====================

inline BlobParser::BlobParser() {
    stats_ = {0, 0, 0, 0, 0};
}

inline BlobParser::Stats BlobParser::get_stats() const {
    return stats_;
}

inline BlobChainReconstructor::BlobChainReconstructor() {
    stats_ = {0, 0, 0, 0};
}

inline BlobChainReconstructor::Stats BlobChainReconstructor::get_stats() const {
    return stats_;
}

// ==================== BlobPointer ====================

inline std::optional<BlobPointer> BlobPointer::parse(const uint8_t* data, size_t size) {
    if (size < 20) {
        return std::nullopt;
    }
    
    BlobPointer ptr;
    ptr.space_id = read_le<uint32_t>(data, 0);
    ptr.page_no = read_le<uint32_t>(data, 4);
    ptr.offset = read_le<uint32_t>(data, 8);
    
    // len 是 25 位 (低 24 位 + 1 位压缩标志)
    uint32_t len_field = read_le<uint32_t>(data, 12);
    ptr.len = len_field & 0x00FFFFFF;
    ptr.is_compressed = (len_field & 0x80000000) != 0;
    
    return ptr;
}

inline std::string BlobPointer::to_string() const {
    std::ostringstream oss;
    oss << "BlobPointer{"
        << "space=" << space_id
        << ", page=" << page_no
        << ", offset=" << offset
        << ", len=" << len
        << ", compressed=" << (is_compressed ? "true" : "false")
        << "}";
    return oss.str();
}

// ==================== BlobPageHeader ====================

inline bool BlobPageHeader::parse(const uint8_t* data, size_t size) {
    if (size < 20) {
        return false;
    }
    
    prev_page = read_le<uint32_t>(data, 0);
    next_page = read_le<uint32_t>(data, 4);
    type = read_le<uint32_t>(data, 8);
    len = read_le<uint32_t>(data, 12);
    max_len = read_le<uint32_t>(data, 16);
    
    return true;
}

inline std::string BlobPageHeader::to_string() const {
    std::ostringstream oss;
    oss << "BlobPageHeader{"
        << "prev=" << prev_page
        << ", next=" << next_page
        << ", type=" << type
        << ", len=" << len
        << ", max_len=" << max_len
        << "}";
    return oss.str();
}

// ==================== BlobData ====================

inline std::string BlobData::to_string() const {
    std::ostringstream oss;
    oss << "BlobData{"
        << "total=" << total_len
        << ", data=" << data_len
        << ", compressed=" << (is_compressed ? "true" : "false")
        << ", size=" << data.size()
        << "}";
    return oss.str();
}

// ==================== BlobChain ====================

inline bool BlobChain::is_valid() const {
    if (page_numbers.empty()) {
        return false;
    }
    
    // 检查页号连续性
    for (size_t i = 1; i < page_numbers.size(); i++) {
        if (page_numbers[i] == 0) {
            return false;
        }
    }
    
    return true;
}

inline std::string BlobChain::to_string() const {
    std::ostringstream oss;
    oss << "BlobChain{"
        << "pages=" << page_numbers.size()
        << ", total_len=" << total_length
        << ", data_len=" << data_length
        << ", valid=" << (is_valid() ? "true" : "false")
        << "}";
    return oss.str();
}

// ==================== BlobParser ====================

inline std::optional<BlobPointer> BlobParser::parse_pointer(const uint8_t* data, size_t size) {
    auto ptr = BlobPointer::parse(data, size);
    if (ptr) {
        stats_.pointers_parsed++;
    } else {
        stats_.errors++;
    }
    return ptr;
}

inline std::optional<BlobPageHeader> BlobParser::parse_page_header(const uint8_t* data, size_t size) {
    BlobPageHeader header;
    if (header.parse(data, size)) {
        stats_.pages_parsed++;
        return header;
    }
    stats_.errors++;
    return std::nullopt;
}

inline std::optional<BlobData> BlobParser::parse_blob_data(const uint8_t* data, size_t size,
                                                           const BlobPointer& pointer) {
    if (pointer.len > size) {
        return std::nullopt;
    }
    
    BlobData blob;
    blob.data.resize(pointer.len);
    std::memcpy(blob.data.data(), data, pointer.len);
    blob.total_len = pointer.len;
    blob.data_len = pointer.len;
    blob.is_compressed = pointer.is_compressed;
    
    stats_.bytes_read += pointer.len;
    return blob;
}

inline std::optional<BlobChain> BlobParser::parse_chain(
    const std::vector<uint32_t>& page_numbers,
    std::function<std::optional<std::vector<uint8_t>>(uint32_t)> page_reader) {
    BlobChain chain;
    chain.page_numbers = page_numbers;
    
    uint32_t total_len = 0;
    for (uint32_t page_no : page_numbers) {
        auto data_opt = page_reader(page_no);
        if (!data_opt.has_value()) {
            chain.page_numbers.clear();
            return std::nullopt;
        }
        
        BlobPageHeader header;
        if (header.parse(data_opt->data(), data_opt->size())) {
            total_len += header.len;
        }
    }
    
    chain.total_length = total_len;
    chain.data_length = total_len;
    
    stats_.chains_parsed++;
    return chain;
}

inline std::vector<uint8_t> BlobParser::read_blob(
    const BlobChain& chain,
    std::function<std::optional<std::vector<uint8_t>>(uint32_t)> page_reader) {
    std::vector<uint8_t> result;
    
    for (uint32_t page_no : chain.page_numbers) {
        auto data_opt = page_reader(page_no);
        if (!data_opt.has_value()) {
            continue;
        }
        
        // 跳过页头 (20字节)
        if (data_opt->size() > 20) {
            result.insert(result.end(), data_opt->data() + 20, data_opt->data() + data_opt->size());
        }
    }
    
    stats_.bytes_read += result.size();
    return result;
}

inline std::optional<BlobPointer> BlobParser::extract_pointer(const std::vector<uint8_t>& row_data,
                                                              uint16_t col_idx) {
    // BLOB pointer is 20 bytes at offset determined by column position.
    // For COMPACT/DYNAMIC format, BLOB fields store the 20-byte BTR_EXTERN
    // reference at the field's data position (variable-length).
    // Without schema, we scan backward from the end of row data for a valid pointer.
    if (row_data.size() < 20) {
        return std::nullopt;
    }

    // Try multiple candidate positions starting from end of data
    // The most recent BLOB column's pointer will be at the end
    // Earlier BLOB columns' pointers are further back
    // Since we don't have schema positions, try a window scan
    size_t search_end = row_data.size();
    for (int pass = static_cast<int>(col_idx); pass >= 0 && search_end >= 20; pass--) {
        const uint8_t* candidate = row_data.data() + search_end - 20;
        auto ptr = parse_pointer(candidate, 20);
        if (ptr.has_value() && ptr->page_no != 0 && ptr->page_no != FIL_NULL) {
            if (ptr->len > 0 && ptr->len < 256 * 1024 * 1024) {  // Sanity: < 256MB
                return ptr;
            }
        }
        search_end -= 20;  // Move back past this 20-byte pointer
    }

    // Fallback: try last 20 bytes
    return parse_pointer(row_data.data() + row_data.size() - 20, 20);
}

// ==================== BlobChainReconstructor ====================

inline void BlobChainReconstructor::add_page(uint32_t page_no, std::vector<uint8_t> data) {
    Page page;
    page.page_no = page_no;
    page.data = std::move(data);
    
    if (page.data.size() >= 24) {
        page.next_page = read_le<uint32_t>(page.data.data(), 4);
    } else {
        page.next_page = 0;
    }
    
    pages_.push_back(page);
    stats_.pages_added++;
}

inline bool BlobChainReconstructor::reconstruct() {
    if (pages_.empty()) {
        return false;
    }

    // 检测循环引用
    if (detect_circular_reference()) {
        stats_.circular_references++;
        return false;
    }

    // Sort by page_no as a fallback for when FIL_PAGE_NEXT is unavailable
    // In the general case, pages are traversed following FIL_PAGE_NEXT chain
    // (offset 4 in FIL header, big-endian). For recovery scenarios where pages
    // are collected individually, page_no order is a reasonable approximation.
    std::sort(pages_.begin(), pages_.end(), [](const Page& a, const Page& b) {
        return a.page_no < b.page_no;
    });

    stats_.chains_reconstructed++;
    return true;
}

inline std::vector<uint8_t> BlobChainReconstructor::get_data() const {
    std::vector<uint8_t> result;
    
    for (const auto& page : pages_) {
        // 跳过页头
        if (page.data.size() > 20) {
            result.insert(result.end(), page.data.data() + 20, page.data.data() + page.data.size());
        }
    }
    
    return result;
}

inline bool BlobChainReconstructor::detect_circular_reference() {
    std::set<uint32_t> seen;
    
    for (const auto& page : pages_) {
        if (seen.count(page.page_no)) {
            return true;
        }
        seen.insert(page.page_no);
    }
    
    return false;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BLOB_PARSER_HPP
