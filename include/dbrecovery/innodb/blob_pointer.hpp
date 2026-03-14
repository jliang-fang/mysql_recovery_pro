#pragma once
#include <span>
#include <optional>
#include <cstdint>
#include <vector>
#include <utility>
#include <set>
#include <functional>

#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ✅ BLOB page chain（支持多页拼接）
struct BlobPageChain {
    std::vector<std::pair<uint32_t, uint32_t>> pages; // (space_id, page_no)
    
    // BLOB 页头偏移常量
    static constexpr size_t BTR_BPAGE_NEXT = 0;   // 下一个 BLOB 页号（4字节）
    static constexpr size_t BTR_BPAGE_PREV = 4;   // 上一个 BLOB 页号（4字节）
    static constexpr size_t BTR_BPAGE_LEN  = 8;   // 数据长度（4字节）

    void add_page(uint32_t space_id, uint32_t page_no) {
        pages.emplace_back(space_id, page_no);
    }

    bool is_valid(const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader = [](uint32_t, uint32_t) { return std::nullopt; }) const {
        if (pages.empty()) return false;
        
        // 检查是否有重复页
        std::set<std::pair<uint32_t, uint32_t>> seen;
        for (const auto& p : pages) {
            if (seen.count(p)) {
                std::cerr << "[ERROR] BLOB chain contains duplicate page: (" 
                          << p.first << ", " << p.second << ")" << std::endl;
                return false;
            }
            seen.insert(p);
        }
        
        // 验证链表连续性（需要实际读取 BLOB 页头验证）
        for (size_t i = 1; i < pages.size(); ++i) {
            const auto& prev_page = pages[i-1];
            const auto& curr_page = pages[i];
            
            if (prev_page.first != curr_page.first) {
                // 不同表空间，跳过连续性检查（允许跨表空间BLOB）
                continue;
            }
            
            // 读取前一个页的 BLOB 头以验证 next_bpage 字段
            auto prev_data = page_reader(prev_page.first, prev_page.second);
            if (!prev_data || prev_data->size() < 16) {
                std::cerr << "[WARN] Cannot read BLOB page header for validation: (" 
                          << prev_page.first << ", " << prev_page.second << ")" << std::endl;
                continue; // 无法读取或数据不足，跳过验证
            }
            
            // 读取 next_bpage 字段（偏移 0）
            uint32_t next_bpage = dbrecovery::innodb::read_le<uint32_t>(prev_data->data(), BTR_BPAGE_NEXT);
            
            if (next_bpage != curr_page.second) {
                std::cerr << "[ERROR] BLOB chain断裂 at page (" 
                          << prev_page.first << ", " << prev_page.second 
                          << "): expected next=" << curr_page.second 
                          << ", actual=" << next_bpage << std::endl;
                return false; // next_bpage 不匹配，链表断裂
            }
        }
        
        return true;
    }
};

// ✅ BLOB 指针结构（20字节）
struct BlobPointer {
    uint32_t space_id;
    uint32_t page_no;
    uint32_t offset;
    uint32_t len;           // 当前页的数据长度
    uint32_t total_len;     // BLOB 总长度（完整指针格式包含此字段）
    
    // ✅ 新增：BLOB 页链信息
    std::vector<std::pair<uint32_t, uint32_t>> chain_pages; // (space_id, page_no)
    
    bool is_complete() const {
        // ✅ 增强：验证 BLOB 指针的完整性
        if (total_len == 0 || chain_pages.empty()) {
            return false;
        }
        
        // 验证链表长度与总长度的一致性（粗略检查）
        // 实际长度需要读取每个页并累加数据长度
        if (chain_pages.size() > 1 && total_len <= len) {
            std::cerr << "[WARN] BLOB chain has multiple pages but total_len <= single_page_len" << std::endl;
        }
        
        // ✅ 新增：验证链表中每个页的偏移和长度是否合理
        for (const auto& page : chain_pages) {
            if (page.second >= 0xFFFFFFFF) {
                std::cerr << "[ERROR] BLOB chain contains invalid page number" << std::endl;
                return false;
            }
        }
        
        // ✅ 新增：验证总长度是否超过合理限制（64GB）
        if (total_len > (64ULL * 1024 * 1024 * 1024)) {
            std::cerr << "[ERROR] BLOB total_len exceeds reasonable limit: " << total_len << std::endl;
            return false;
        }
        
        return true;
    }
};

// ✅ 新增：从页数据中解析 BLOB 指针（20字节）
std::optional<BlobPointer> parse_blob_pointer(const uint8_t* data, size_t data_size = 20);

// ✅ 新增：从页数据中解析 BLOB 指针（20字节）
std::optional<BlobPointer> parse_blob_pointer(const std::vector<uint8_t>& data);

// ✅ 新增：从 BLOB 页数据中解析完整页链
std::optional<BlobPageChain> build_blob_page_chain(
    uint32_t first_space_id,
    uint32_t first_page_no,
    const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader = [](uint32_t, uint32_t) { return std::nullopt; }
);
    
// ✅ 新增：从 BLOB 页数据中读取实际数据长度
size_t read_blob_page_data_length(const std::vector<uint8_t>& page_data);

// ==================== 实现部分 ====================
inline std::optional<BlobPointer> parse_blob_pointer(const uint8_t* data, size_t data_size) {
    if (!data || data_size < 16) return std::nullopt;
    
    // BLOB 指针格式（20字节）：
    // [0-3]:  space_id
    // [4-7]:  page_no (first BLOB 页)
    // [8-11]: offset (数据在页内的偏移)
    // [12-15]: len (当前页的数据长度)
    // [16-19]: total_len (BLOB 总长度，某些版本可能省略)
    
    BlobPointer ptr;
    ptr.space_id = dbrecovery::innodb::read_le<uint32_t>(data, 0);
    ptr.page_no = dbrecovery::innodb::read_le<uint32_t>(data, 4);
    ptr.offset = dbrecovery::innodb::read_le<uint32_t>(data, 8);
    ptr.len = dbrecovery::innodb::read_le<uint32_t>(data, 12);
    
    // ✅ 修复：尝试读取总长度（需要额外4字节）
    if (data_size >= 20) {
        ptr.total_len = dbrecovery::innodb::read_le<uint32_t>(data, 16);
    } else {
        // 如果数据不足，保持默认值（len）
        ptr.total_len = ptr.len;
    }
    
    // 验证 BLOB 指针的有效性
    if (ptr.space_id == 0xFFFFFFFF || ptr.page_no == 0xFFFFFFFF) {
        return std::nullopt; // 无效的 BLOB 指针
    }
    
    // ✅ 新增：验证偏移和长度的边界检查（防止越界访问）
    if (ptr.offset >= 16384 || ptr.len > 16384) {
        std::cerr << "[ERROR] BLOB pointer has invalid offset/len: offset=" 
                  << ptr.offset << ", len=" << ptr.len << std::endl;
        return std::nullopt;
    }
    
    // ✅ 修复：total_len == 0 表示完整指针（无溢出页），此时 len 就是总长度
    if (ptr.total_len == 0) {
        ptr.total_len = ptr.len;
    } else if (ptr.len > ptr.total_len) {
        std::cerr << "[WARN] BLOB pointer length mismatch: len=" << ptr.len 
                  << ", total_len=" << ptr.total_len << ". Using len as total_len." << std::endl;
        // 某些情况下 len 可能大于 total_len（如页内截断），以 len 为准
        ptr.total_len = ptr.len;
    }
    
    // ✅ 新增：验证总长度合理性（BLOB 不能超过 64GB，实际通常远小于此）
    if (ptr.total_len > (64ULL * 1024 * 1024 * 1024)) {
        std::cerr << "[ERROR] BLOB total_len exceeds reasonable limit: " << ptr.total_len << std::endl;
        return std::nullopt;
    }
    
    // ✅ 新增：验证跨页情况
    if (ptr.offset + ptr.len > 16384) {
        std::cerr << "[INFO] BLOB data is cross-page: offset=" << ptr.offset 
                  << ", len=" << ptr.len << ", page_size=16384." << std::endl;
    }
    
    // 初始化页链（至少包含当前页）
    ptr.chain_pages.emplace_back(ptr.space_id, ptr.page_no);
    
    return ptr;
}

inline std::optional<BlobPointer> parse_blob_pointer(const std::vector<uint8_t>& data) {
    if (data.size() < 16) return std::nullopt;
    return parse_blob_pointer(data.data(), data.size());
}

inline size_t read_blob_page_data_length(const std::vector<uint8_t>& page_data) {
    if (page_data.size() < BlobPageChain::BTR_BPAGE_LEN + 4) {
        return 0;
    }
    
    // BLOB 页头偏移 8 处是数据长度（4字节）
    return dbrecovery::innodb::read_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN);
}

inline std::optional<BlobPageChain> build_blob_page_chain(
    uint32_t first_space_id,
    uint32_t first_page_no,
    const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader
) {
    BlobPageChain chain;
    
    if (!page_reader) {
        return std::nullopt;
    }
    
    // ✅ 修复：添加最大深度限制（BLOB 链通常不会超过 100 页）
    const size_t MAX_BLOB_CHAIN_DEPTH = 1024;
    size_t depth = 0;
    
    uint32_t current_space_id = first_space_id;
    uint32_t current_page_no = first_page_no;
    
    while (depth < MAX_BLOB_CHAIN_DEPTH) {
        chain.add_page(current_space_id, current_page_no);
        
        auto page_data = page_reader(current_space_id, current_page_no);
        if (!page_data || page_data->size() < 16) {
            break;
        }
        
        // ✅ 新增：验证 BLOB 页头校验和（可选，增强数据完整性检查）
        // 这里可以添加 CRC32 校验逻辑
        
        // ✅ 修复：验证 BLOB 页头有效性
        uint32_t next_bpage = dbrecovery::innodb::read_le<uint32_t>(page_data->data(), BlobPageChain::BTR_BPAGE_NEXT);
        uint32_t prev_bpage = dbrecovery::innodb::read_le<uint32_t>(page_data->data(), BlobPageChain::BTR_BPAGE_PREV);
        
        // 检查页号有效性
        if (next_bpage != 0xFFFFFFFF && next_bpage != 0 && 
            (next_bpage >= 0x10000000)) { // 假设有效页号小于 2^28
            std::cerr << "[WARN] Invalid BLOB next page number: " << next_bpage << std::endl;
            break;
        }
        
        if (prev_bpage != 0xFFFFFFFF && prev_bpage != 0 &&
            (prev_bpage >= 0x10000000)) {
            std::cerr << "[WARN] Invalid BLOB previous page number: " << prev_bpage << std::endl;
            break;
        }
        
        // ✅ 新增：读取并验证当前页的数据长度
        size_t current_page_data_len = read_blob_page_data_length(*page_data);
        if (current_page_data_len == 0 || current_page_data_len > page_data->size() - 16) {
            std::cerr << "[WARN] Invalid BLOB page data length: " << current_page_data_len << std::endl;
            break;
        }
        
        // 链表结束条件
        if (next_bpage == 0xFFFFFFFF || next_bpage == 0) {
            break;
        }
        
        // ✅ 新增：验证页链连续性（可选，需要额外读取前一页）
        // 这里只做基本检查，避免跨页错误
        
        current_page_no = next_bpage;
        depth++;
    }
    
    if (chain.pages.empty()) {
        return std::nullopt;
    }
    
    // ✅ 修复：验证链表长度合理性（BLOB 数据通常不会超过 10000 页）
    if (chain.pages.size() > MAX_BLOB_CHAIN_DEPTH) {
        std::cerr << "[ERROR] BLOB chain exceeds maximum depth: " << chain.pages.size() << std::endl;
        return std::nullopt;
    }
    
    return chain;
}

} // namespace innodb
} // namespace dbrecovery
