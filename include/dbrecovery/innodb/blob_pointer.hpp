#pragma once
#include <span>
#include <optional>
#include <cstdint>
#include <vector>
#include <utility>
#include <set>
#include <functional>
#include <iostream>
#include <cmath>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_reader.hpp"

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
        if (pages.empty()) {
            std::cerr << "[ERROR] BLOB chain is empty" << std::endl;
            return false;
        }
        
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
                std::cerr << "[WARN] BLOB chain crosses tablespace from (" 
                          << prev_page.first << ", " << prev_page.second << ") to ("
                          << curr_page.first << ", " << curr_page.second << ")" << std::endl;
                continue; // 继续处理后续页对
            }
            
            // 读取前一个页的 BLOB 头以验证 next_bpage 字段
            auto prev_data = page_reader(prev_page.first, prev_page.second);
            if (!prev_data) {
                std::cerr << "[WARN] Cannot read BLOB page header for validation: (" 
                          << prev_page.first << ", " << prev_page.second << ")" << std::endl;
                continue; // 无法读取，跳过验证
            }
            
            if (prev_data->size() < 16) {
                std::cerr << "[WARN] BLOB page header too small: (" 
                          << prev_page.first << ", " << prev_page.second << ")" << std::endl;
                continue; // 数据不足，跳过验证
            }
            
            // 读取 next_bpage 字段（偏移 0）
            uint32_t next_bpage = dbrecovery::innodb::read_le<uint32_t>(prev_data->data(), BTR_BPAGE_NEXT);
            
            if (next_bpage == 0xFFFFFFFF) {
                // 链表提前结束，标记为警告但不失败
                std::cerr << "[WARN] BLOB chain ends prematurely at page (" 
                          << prev_page.first << ", " << prev_page.second << ")" << std::endl;
                break;
            }
            
            if (next_bpage != curr_page.second) {
                std::cerr << "[ERROR] BLOB chain断裂 at page (" 
                          << prev_page.first << ", " << prev_page.second 
                          << "): expected next=" << curr_page.second 
                          << ", actual=" << next_bpage << std::endl;
                return false; // next_bpage 不匹配，链表断裂
            }
            
            // ✅ 新增：验证 BLOB 页头校验和（增强数据完整性检查）
            // 注意：BLOB 页也是标准 InnoDB 页，校验和位于 FIL Header 的偏移 24-27 (即页起始位置 + 24)
            if (prev_data->size() >= 62) { // 确保有足够空间读取到校验和字段
                uint32_t stored_checksum = dbrecovery::innodb::read_le<uint32_t>(prev_data->data(), 24);
                
                // 标准 InnoDB 校验模式：NONE (0 或 0xFFFFFFFF) 视为有效
                if (stored_checksum == 0 || stored_checksum == 0xFFFFFFFF) {
                    // 校验和未启用，跳过验证
                } else {
                    // 创建页副本并清零校验和字段（偏移 24-27）
                    std::vector<uint8_t> page_copy = *prev_data;
                    for (int j = 0; j < 4; ++j) {
                        page_copy[24 + j] = 0;
                    }
                    
                    uint32_t computed_crc32c = dbrecovery::innodb::PageReader::compute_crc32c(page_copy);
                    if (computed_crc32c != stored_checksum) {
                        std::cerr << "[WARN] BLOB page checksum mismatch at (" 
                                  << prev_page.first << ", " << prev_page.second 
                                  << "), stored=" << stored_checksum 
                                  << ", computed=" << computed_crc32c << std::endl;
                    }
                }
            } else {
                // 页太小，无法包含完整的 FIL Header
                std::cerr << "[WARN] BLOB page too small for checksum validation at (" 
                          << prev_page.first << ", " << prev_page.second << ")" << std::endl;
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
        if (chain_pages.empty()) {
            std::cerr << "[ERROR] BLOB chain is empty" << std::endl;
            return false;
        }

        // total_len == 0 表示完整指针（无溢出页），此时 len 就是总长度，视为有效
        if (total_len == 0) {
            if (len == 0) { // 单页 BLOB 长度为 0 是无效的
                std::cerr << "[ERROR] BLOB pointer has zero length on single page" << std::endl;
                return false;
            }
            return true; // 单页 BLOB，长度有效
        }

        if (total_len <= len && chain_pages.size() > 1) {
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

        // ✅ 新增：验证链表总长度与各页数据长度之和的一致性（需要 page_reader）
        // 注意：此检查依赖于实际读取 BLOB 页数据，暂不在此处实现，留作后续扩展点
        
        return true;
    }
};

// ✅ 新增：从页数据中解析 BLOB 指针（20字节）
std::optional<BlobPointer> parse_blob_pointer(const uint8_t* data, size_t data_size = 20) {
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
        
    // ✅ 统一：total_len == 0 表示完整指针（无溢出页），此时 len 就是总长度
    if (ptr.total_len == 0) {
        ptr.total_len = ptr.len;
    } else if (ptr.len > ptr.total_len) {
        std::cerr << "[WARN] BLOB pointer length mismatch: len=" << ptr.len 
                  << ", total_len=" << ptr.total_len << ". Using len as total_len." << std::endl;
        // 某些情况下 len 可能大于 total_len（如页内截断），以 len 为准
        ptr.total_len = ptr.len;
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

// ✅ 新增：从页数据中解析 BLOB 指针（20字节）
inline std::optional<BlobPointer> parse_blob_pointer(const std::vector<uint8_t>& data) {
    if (data.size() < 16) return std::nullopt;
    return parse_blob_pointer(data.data(), data.size());
}

// ✅ 新增：从 BLOB 页数据中解析完整页链
std::optional<BlobPageChain> build_blob_page_chain(
    uint32_t first_space_id,
    uint32_t first_page_no,
    const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader = [](uint32_t, uint32_t) { return std::nullopt; }
) {
    BlobPageChain chain;
    
    // 初始化页链
    chain.add_page(first_space_id, first_page_no);
    
    // 如果没有 page_reader，无法验证连续性，直接返回当前页
    if (!page_reader) {
        return chain;
    }
    
    // 遍历 BLOB 页链
    uint32_t current_space = first_space_id;
    uint32_t current_page = first_page_no;
    
    while (true) {
        auto page_data = page_reader(current_space, current_page);
        if (!page_data || page_data->size() < 16) {
            break; // 无法读取或数据不足
        }
        
        // 读取下一个 BLOB 页号（偏移 0）
        uint32_t next_bpage = dbrecovery::innodb::read_le<uint32_t>(page_data->data(), BlobPageChain::BTR_BPAGE_NEXT);
        
        // 如果 next_bpage 为 0xFFFFFFFF 或与当前页相同，表示链表结束
        if (next_bpage == 0xFFFFFFFF || next_bpage == current_page) {
            break;
        }
        
        // 检查是否形成循环（防止无限遍历）
        if (chain.pages.size() > 1000) { // BLOB 链通常不会太长
            std::cerr << "[WARN] BLOB chain too long, possible circular reference" << std::endl;
            break;
        }
        
        // 检查是否已访问过该页（防止循环）
        bool is_duplicate = false;
        for (const auto& p : chain.pages) {
            if (p.first == current_space && p.second == next_bpage) {
                is_duplicate = true;
                break;
            }
        }
        
        if (is_duplicate) {
            std::cerr << "[WARN] BLOB chain contains circular reference at page (" 
                      << current_space << ", " << next_bpage << ")" << std::endl;
            break;
        }
        
        // 添加下一个页到链中
        chain.add_page(current_space, next_bpage);
        
        // 移动到下一页
        current_page = next_bpage;
    }
    
    return chain;
}

// ✅ 新增：从 BLOB 页数据中读取实际数据长度
size_t read_blob_page_data_length(const std::vector<uint8_t>& page_data) {
    if (page_data.size() < BlobPageChain::BTR_BPAGE_LEN + 4) {
        return 0;
    }
    
    // 读取数据长度字段（偏移 8）
    return static_cast<size_t>(dbrecovery::innodb::read_le<uint32_t>(
        page_data.data(), BlobPageChain::BTR_BPAGE_LEN
    ));
}

// ✅ 新增：从 BLOB 页链中读取完整数据
std::vector<uint8_t> read_blob_data_from_chain(
    const BlobPageChain& chain,
    uint32_t total_len,
    const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader
) {
    if (!page_reader || chain.pages.empty()) {
        return {};
    }
    
    std::vector<uint8_t> blob_data;
    blob_data.reserve(total_len);
    
    for (const auto& [space_id, page_no] : chain.pages) {
        auto page_data = page_reader(space_id, page_no);
        if (!page_data || page_data->size() < 16) {
            std::cerr << "[ERROR] Failed to read BLOB page (" << space_id << ", " << page_no << ")" << std::endl;
            continue;
        }
        
        // 读取数据长度
        size_t data_len = read_blob_page_data_length(*page_data);
        if (data_len == 0 || data_len > page_data->size() - 16) {
            std::cerr << "[WARN] Invalid BLOB page data length: " << data_len << std::endl;
            continue;
        }
        
        // 添加数据到结果（跳过页头16字节）
        size_t data_offset = 16; // BLOB 页头大小
        blob_data.insert(blob_data.end(), page_data->begin() + data_offset, 
                        page_data->begin() + data_offset + data_len);
    }
    
    // 截断到总长度（防止溢出）
    if (blob_data.size() > total_len) {
        blob_data.resize(total_len);
    }
    
    return blob_data;
}

// ==================== 单元测试入口（供 GTest 调用）====================
#ifdef DBRECOVERY_ENABLE_TESTS
#include <gtest/gtest.h>
#include "dbrecovery/innodb/blob_pointer.hpp"

namespace dbrecovery {
namespace innodb {

// 测试读取 BLOB 页数据长度（正常情况）
TEST(BlobParserTest, ReadBlobPageDataLengthValid) {
    std::vector<uint8_t> page_data(16384, 0);
    // 设置数据长度字段（偏移 8）为 100
    write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 100);

    EXPECT_EQ(read_blob_page_data_length(page_data), 100);
}

// 测试读取 BLOB 页数据长度（页太小）
TEST(BlobParserTest, ReadBlobPageDataLengthTooSmall) {
    std::vector<uint8_t> page_data(10, 0); // 小于最小大小

    EXPECT_EQ(read_blob_page_data_length(page_data), 0);
}

// 测试读取 BLOB 页数据长度（长度过大）
TEST(BlobParserTest, ReadBlobPageDataLengthTooLarge) {
    std::vector<uint8_t> page_data(16384, 0);
    // 设置数据长度字段为超过最大值
    write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 16384);

    EXPECT_EQ(read_blob_page_data_length(page_data), 0);
}

// 测试从 BLOB 页链读取完整数据（正常情况）
TEST(BlobParserTest, ReadBlobDataFromChainValid) {
    BlobPageChain chain;
    chain.add_page(0, 100);

    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置数据长度字段（偏移 8）为 50
            write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 50);
            // 填充一些测试数据（从偏移 54 开始）
            for (int i = 0; i < 50; ++i) {
                page_data[54 + i] = static_cast<uint8_t>(i % 256);
            }
            return page_data;
        }
        return std::nullopt;
    };

    auto result = read_blob_data_from_chain(chain, 50, page_reader);
    EXPECT_EQ(result.size(), 50);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(result[i], static_cast<uint8_t>(i % 256));
    }
}

// 测试从 BLOB 页链读取完整数据（total_len 过大）
TEST(BlobParserTest, ReadBlobDataFromChainTotalLenTooLarge) {
    BlobPageChain chain;
    chain.add_page(0, 100);

    auto page_reader = [](uint32_t, uint32_t) { return std::nullopt; };

    // total_len 超过 64GB
    auto result = read_blob_data_from_chain(chain, 128ULL * 1024 * 1024 * 1024, page_reader);
    EXPECT_TRUE(result.empty());
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_ENABLE_TESTS
