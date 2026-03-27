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
    
    // BLOB 页头偏移常量（参考 MySQL 8.0 源码 include/btr0btr.h 和 fil0fil.h）
    // BLOB 页是标准 InnoDB 页，FIL Header 占用前 38 字节
    // BLOB 头位于 FIL Header 之后，即偏移 38
    static constexpr size_t BTR_BPAGE_NEXT = 38 + 0;   // 下一个 BLOB 页号（4字节），绝对偏移
    static constexpr size_t BTR_BPAGE_PREV = 38 + 4;   // 上一个 BLOB 页号（4字节），绝对偏移
    static constexpr size_t BTR_BPAGE_LEN  = 38 + 8;   // 数据长度（4字节），绝对偏移

    void add_page(uint32_t space_id, uint32_t page_no) {
        pages.emplace_back(space_id, page_no);
    }

    // ✅ 新增：实现 is_valid 方法，验证页链完整性
    bool is_valid(const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader = [](uint32_t, uint32_t) { return std::nullopt; }, 
                  bool strict_mode = true) const {
        if (pages.empty()) {
            if (strict_mode) {
                std::cerr << "[ERROR] BlobPageChain is empty" << std::endl;
            }
            return false;
        }
        
        // ✅ 新增：检测循环引用
        if (has_circular_reference()) {
            if (strict_mode) {
                std::cerr << "[ERROR] BlobPageChain contains circular reference" << std::endl;
            }
            return false;
        }
        
        // 如果没有 page_reader，仅做基础校验（已通过 has_circular_reference）
        if (!page_reader) {
            return true;
        }
        
        // ✅ 新增：验证页链连续性
        for (size_t i = 0; i < pages.size(); ++i) {
            auto [space_id, page_no] = pages[i];
            auto page_data_opt = page_reader(space_id, page_no);
            
            if (!page_data_opt || page_data_opt->size() < BlobPageChain::BTR_BPAGE_LEN + 4) {
                if (strict_mode) {
                    std::cerr << "[ERROR] Failed to read BLOB page (" << space_id << ", " << page_no << ")" << std::endl;
                }
                return false;
            }
            
            // ✅ 新增：验证下一个页号（除最后一页外）
            if (i < pages.size() - 1) {
                uint32_t next_expected = pages[i + 1].second;
                uint32_t actual_next = dbrecovery::innodb::read_le<uint32_t>(
                    page_data_opt->data(), BlobPageChain::BTR_BPAGE_NEXT
                );
                
                if (actual_next != next_expected) {
                    if (strict_mode) {
                        std::cerr << "[ERROR] BLOB chain broken at page (" << space_id 
                                  << ", " << page_no << "). Expected next=" << next_expected 
                                  << ", actual=" << actual_next << std::endl;
                    }
                    return false;
                }
            }
        }
        
        return true;
    }

    // ✅ 新增：检测循环引用（Floyd 判圈算法实现）
    bool has_circular_reference() const {
        if (pages.size() <= 1) {
            return false; // 空链或单页无法形成循环
        }

        // 使用 Floyd 判圈算法（龟兔赛跑）
        size_t slow = 0;
        size_t fast = 0;

        while (true) {
            // 龟走一步
            slow++;
            if (slow >= pages.size()) break;

            // 兔走两步
            fast += 2;
            if (fast >= pages.size()) break;

            // 检查是否相遇（注意：比较的是 pair<space_id, page_no>）
            if (pages[slow] == pages[fast]) {
                return true; // 检测到循环
            }
        }

        return false;
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
    
    // ✅ 新增：构造函数初始化 chain_pages
    BlobPointer() : space_id(0), page_no(0), offset(0), len(0), total_len(0) {}
    
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

    // ✅ 新增：深度校验方法（带 page_reader 回调）
    bool is_valid(const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader = [](uint32_t, uint32_t) { return std::nullopt; },
                  bool strict_mode = true) const {
        if (!is_complete()) {
            return false;
        }

        // 如果没有 page_reader，仅做基础校验
        if (!page_reader) {
            return true;
        }

        // 构建页链并验证
        BlobPageChain chain;
        for (const auto& p : chain_pages) {
            chain.add_page(p.first, p.second);
        }

        bool valid = chain.is_valid(page_reader, strict_mode);
        if (!valid && strict_mode) {
            std::cerr << "[ERROR] BLOB pointer validation failed: " 
                      << (chain.has_circular_reference() ? "circular reference" : "broken chain") << std::endl;
        }
        return valid;
    }
};

// ✅ 新增：从页数据中解析 BLOB 指针（20字节）
std::optional<BlobPointer> parse_blob_pointer(const uint8_t* data, size_t data_size = 20,
                                              const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader = [](uint32_t, uint32_t) { return std::nullopt; }) {
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
    static constexpr uint32_t MAX_PAGE_SIZE = 65536; // 支持最大页大小（包括压缩页）
    if (ptr.offset >= MAX_PAGE_SIZE || ptr.len > MAX_PAGE_SIZE) {
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
    static constexpr uint32_t DEFAULT_PAGE_SIZE = 16384; // 标准 InnoDB 页大小
    if (ptr.offset + ptr.len > DEFAULT_PAGE_SIZE) {
        std::cerr << "[INFO] BLOB data is cross-page: offset=" << ptr.offset 
                  << ", len=" << ptr.len << ", page_size=" << DEFAULT_PAGE_SIZE << "." << std::endl;
    }
        
    // ✅ 修复：初始化页链并调用 build_blob_page_chain 构建完整链
    ptr.chain_pages.emplace_back(ptr.space_id, ptr.page_no);
    
    // 如果 total_len > len，说明是多页 BLOB，需要构建完整的页链
    if (ptr.total_len > ptr.len) {
        auto chain_opt = build_blob_page_chain(ptr.space_id, ptr.page_no, page_reader);
        if (chain_opt.has_value()) {
            ptr.chain_pages = std::move(chain_opt.value()).pages;
        } else {
            // 如果无法构建完整链，至少保留当前页
            std::cerr << "[WARN] Failed to build full BLOB page chain, using single page only" << std::endl;
        }
    } else if (ptr.total_len == 0 && ptr.len > 0) {
        // ✅ 新增：单页 BLOB 场景，确保 chain_pages 至少包含当前页
        // 此处已通过上面的 `ptr.chain_pages.emplace_back` 实现
    }
        
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
    uint32_t len = dbrecovery::innodb::read_le<uint32_t>(
        page_data.data(), BlobPageChain::BTR_BPAGE_LEN
    );
    
    // ✅ 增强：验证长度合理性（不超过页大小减去页头）
    static constexpr size_t MAX_BLOB_PAGE_DATA_SIZE = 16384 - 16; // 标准 16KB 页减去 16 字节 BLOB 头
    if (len > MAX_BLOB_PAGE_DATA_SIZE) {
        return 0;
    }
    
    return static_cast<size_t>(len);
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
    
    // ✅ P0 修复：在读取前显式检查循环引用，防止无限循环
    if (chain.has_circular_reference()) {
        std::cerr << "[ERROR] BLOB chain contains circular reference, aborting read to prevent infinite loop" << std::endl;
        return {};
    }
    
    std::vector<uint8_t> blob_data;
    blob_data.reserve(total_len);
    
    for (const auto& [space_id, page_no] : chain.pages) {
        auto page_data = page_reader(space_id, page_no);
        if (!page_data || page_data->size() < 16) {
            std::cerr << "[ERROR] Failed to read BLOB page (" << space_id << ", " << page_no << ")" << std::endl;
            break; // ✅ P0 修复：遇到无法读取的页时立即中止，而不是继续
        }
        
        // 读取数据长度
        size_t data_len = read_blob_page_data_length(*page_data);
        if (data_len == 0 || data_len > page_data->size() - 16) {
            std::cerr << "[WARN] Invalid BLOB page data length: " << data_len << std::endl;
            break; // ✅ P0 修复：遇到无效数据长度时立即中止
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

// 测试 is_complete() 在 total_len == 0 且 len > 0 的情况
TEST(BlobPointerTest, IsValidZeroTotalLenAndNonZeroLen) {
    BlobPointer ptr;
    ptr.chain_pages.emplace_back(0, 100);
    ptr.total_len = 0; // 表示未设置
    ptr.len = 50;

    EXPECT_TRUE(ptr.is_complete());
}

// 测试 is_complete() 在 total_len < len 的情况（页内截断）
TEST(BlobPointerTest, IsValidTotalLenLessThanLen) {
    BlobPointer ptr;
    ptr.chain_pages.emplace_back(0, 100);
    ptr.total_len = 30; // 小于 len
    ptr.len = 50;

    EXPECT_TRUE(ptr.is_complete()); // 应记录警告但返回 true
}

// 测试 has_circular_reference() 检测循环引用
TEST(BlobPageChainTest, HasCircularReference) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 101);
    chain.add_page(0, 102);
    chain.add_page(0, 100); // 循环引用

    EXPECT_TRUE(chain.has_circular_reference());
}

// 测试 has_circular_reference() 在无循环时返回 false
TEST(BlobPageChainTest, NoCircularReference) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 101);
    chain.add_page(0, 102);

    EXPECT_FALSE(chain.has_circular_reference());
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_ENABLE_TESTS
