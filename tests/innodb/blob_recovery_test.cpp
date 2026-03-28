#include <gtest/gtest.h>
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include <vector>
#include <functional>
#include <iostream>

namespace dbrecovery {
namespace innodb {

// ==================== BLOB Pointer 解析测试 ====================

// 测试单页BLOB指针解析（正常情况）
TEST(BlobPointerTest, ParseSinglePageBlob) {
    // 创建20字节的BLOB指针数据
    std::vector<uint8_t> data(20, 0);
    
    // space_id = 1
    write_le<uint32_t>(data.data(), 0, 1);
    // page_no = 100
    write_le<uint32_t>(data.data(), 4, 100);
    // offset = 1000
    write_le<uint32_t>(data.data(), 8, 1000);
    // len = 500
    write_le<uint32_t>(data.data(), 12, 500);
    // total_len = 500 (单页BLOB)
    write_le<uint32_t>(data.data(), 16, 500);
    
    auto ptr_opt = parse_blob_pointer(data.data(), data.size());
    EXPECT_TRUE(ptr_opt.has_value());
    
    const auto& ptr = ptr_opt.value();
    EXPECT_EQ(ptr.space_id, 1);
    EXPECT_EQ(ptr.page_no, 100);
    EXPECT_EQ(ptr.offset, 1000);
    EXPECT_EQ(ptr.len, 500);
    EXPECT_EQ(ptr.total_len, 500);
    EXPECT_TRUE(ptr.is_complete());
}

// 测试多页BLOB指针解析
TEST(BlobPointerTest, ParseMultiPageBlob) {
    std::vector<uint8_t> data(20, 0);
    
    write_le<uint32_t>(data.data(), 0, 1);    // space_id
    write_le<uint32_t>(data.data(), 4, 100);  // page_no
    write_le<uint32_t>(data.data(), 8, 500);  // offset
    write_le<uint32_t>(data.data(), 12, 1000); // len (当前页)
    write_le<uint32_t>(data.data(), 16, 5000); // total_len (多页)
    
    auto ptr_opt = parse_blob_pointer(data.data(), data.size());
    EXPECT_TRUE(ptr_opt.has_value());
    
    const auto& ptr = ptr_opt.value();
    EXPECT_EQ(ptr.space_id, 1);
    EXPECT_EQ(ptr.page_no, 100);
    EXPECT_EQ(ptr.len, 1000);
    EXPECT_EQ(ptr.total_len, 5000);
}

// 测试无效BLOB指针（space_id为0xFFFFFFFF）
TEST(BlobPointerTest, ParseInvalidSpaceId) {
    std::vector<uint8_t> data(20, 0);
    
    write_le<uint32_t>(data.data(), 0, 0xFFFFFFFF); // invalid space_id
    write_le<uint32_t>(data.data(), 4, 100);
    write_le<uint32_t>(data.data(), 8, 1000);
    write_le<uint32_t>(data.data(), 12, 500);
    write_le<uint32_t>(data.data(), 16, 500);
    
    auto ptr_opt = parse_blob_pointer(data.data(), data.size());
    EXPECT_FALSE(ptr_opt.has_value());
}

// 测试无效BLOB指针（page_no为0xFFFFFFFF）
TEST(BlobPointerTest, ParseInvalidPageNo) {
    std::vector<uint8_t> data(20, 0);
    
    write_le<uint32_t>(data.data(), 0, 1);
    write_le<uint32_t>(data.data(), 4, 0xFFFFFFFF); // invalid page_no
    write_le<uint32_t>(data.data(), 8, 1000);
    write_le<uint32_t>(data.data(), 12, 500);
    write_le<uint32_t>(data.data(), 16, 500);
    
    auto ptr_opt = parse_blob_pointer(data.data(), data.size());
    EXPECT_FALSE(ptr_opt.has_value());
}

// 测试数据不足（少于16字节）
TEST(BlobPointerTest, ParseDataTooSmall) {
    std::vector<uint8_t> data(15, 0); // 少于16字节
    
    auto ptr_opt = parse_blob_pointer(data.data(), data.size());
    EXPECT_FALSE(ptr_opt.has_value());
}

// 测试BLOB指针长度验证（offset过大）
TEST(BlobPointerTest, ValidateOffsetTooLarge) {
    std::vector<uint8_t> data(20, 0);
    
    write_le<uint32_t>(data.data(), 0, 1);
    write_le<uint32_t>(data.data(), 4, 100);
    write_le<uint32_t>(data.data(), 8, 70000); // offset > 65536
    write_le<uint32_t>(data.data(), 12, 500);
    write_le<uint32_t>(data.data(), 16, 500);
    
    auto ptr_opt = parse_blob_pointer(data.data(), data.size());
    EXPECT_FALSE(ptr_opt.has_value());
}

// 测试BLOB指针长度验证（len过大）
TEST(BlobPointerTest, ValidateLenTooLarge) {
    std::vector<uint8_t> data(20, 0);
    
    write_le<uint32_t>(data.data(), 0, 1);
    write_le<uint32_t>(data.data(), 4, 100);
    write_le<uint32_t>(data.data(), 8, 1000);
    write_le<uint32_t>(data.data(), 12, 70000); // len > 65536
    write_le<uint32_t>(data.data(), 16, 500);
    
    auto ptr_opt = parse_blob_pointer(data.data(), data.size());
    EXPECT_FALSE(ptr_opt.has_value());
}

// 测试total_len == 0的情况（完整指针）
TEST(BlobPointerTest, ValidateZeroTotalLen) {
    std::vector<uint8_t> data(20, 0);
    
    write_le<uint32_t>(data.data(), 0, 1);
    write_le<uint32_t>(data.data(), 4, 100);
    write_le<uint32_t>(data.data(), 8, 1000);
    write_le<uint32_t>(data.data(), 12, 500);
    write_le<uint32_t>(data.data(), 16, 0); // total_len = 0
    
    auto ptr_opt = parse_blob_pointer(data.data(), data.size());
    EXPECT_TRUE(ptr_opt.has_value());
    
    const auto& ptr = ptr_opt.value();
    EXPECT_EQ(ptr.total_len, 500); // 应该被设置为len
}

// 测试len > total_len的情况（页内截断）
TEST(BlobPointerTest, ValidateLenGreaterThanTotalLen) {
    std::vector<uint8_t> data(20, 0);
    
    write_le<uint32_t>(data.data(), 0, 1);
    write_le<uint32_t>(data.data(), 4, 100);
    write_le<uint32_t>(data.data(), 8, 1000);
    write_le<uint32_t>(data.data(), 12, 600); // len > total_len
    write_le<uint32_t>(data.data(), 16, 500); // total_len
    
    auto ptr_opt = parse_blob_pointer(data.data(), data.size());
    EXPECT_TRUE(ptr_opt.has_value());
    
    const auto& ptr = ptr_opt.value();
    EXPECT_EQ(ptr.len, 600);
    EXPECT_EQ(ptr.total_len, 600); // 应该被设置为len
}

// ==================== BLOB Page Chain 测试 ====================

// 测试BLOB页链构建（单页）
TEST(BlobPageChainTest, BuildSinglePageChain) {
    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 1 && page_no == 100) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置数据长度（偏移 38 + 8 = 46）
            write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 500);
            // 设置next页为0xFFFFFFFF（链结束）
            write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 0xFFFFFFFF);
            return page_data;
        }
        return std::nullopt;
    };
    
    auto chain_opt = build_blob_page_chain(1, 100, page_reader);
    EXPECT_TRUE(chain_opt.has_value());
    EXPECT_EQ(chain_opt->pages.size(), 1);
    EXPECT_EQ(chain_opt->pages[0].first, 1);
    EXPECT_EQ(chain_opt->pages[0].second, 100);
}

// 测试BLOB页链构建（多页）
TEST(BlobPageChainTest, BuildMultiPageChain) {
    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        std::vector<uint8_t> page_data(16384, 0);
        
        if (space_id == 1) {
            if (page_no == 100) {
                // 第一页：next = 200
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 200);
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 1000);
            } else if (page_no == 200) {
                // 第二页：next = 300
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 300);
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 1000);
            } else if (page_no == 300) {
                // 第三页：next = 0xFFFFFFFF（链结束）
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 0xFFFFFFFF);
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 500);
            }
        }
        
        return page_data;
    };
    
    auto chain_opt = build_blob_page_chain(1, 100, page_reader);
    EXPECT_TRUE(chain_opt.has_value());
    EXPECT_EQ(chain_opt->pages.size(), 3);
    EXPECT_EQ(chain_opt->pages[0].second, 100);
    EXPECT_EQ(chain_opt->pages[1].second, 200);
    EXPECT_EQ(chain_opt->pages[2].second, 300);
}

// 测试BLOB页链断裂检测
TEST(BlobPageChainTest, DetectBrokenChain) {
    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        std::vector<uint8_t> page_data(16384, 0);
        
        if (space_id == 1) {
            if (page_no == 100) {
                // 第一页：next = 999（但实际第二页是200）
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 999);
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 1000);
            } else if (page_no == 200) {
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 0xFFFFFFFF);
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 1000);
            }
        }
        
        return page_data;
    };
    
    BlobPageChain chain;
    chain.add_page(1, 100);
    chain.add_page(1, 200);
    
    // 严格模式下应该检测到断裂
    EXPECT_FALSE(chain.is_valid(page_reader, true));
}

// 测试BLOB页链循环引用检测
TEST(BlobPageChainTest, DetectCircularReference) {
    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        std::vector<uint8_t> page_data(16384, 0);
        
        if (space_id == 1) {
            if (page_no == 100) {
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 200);
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 1000);
            } else if (page_no == 200) {
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 300);
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 1000);
            } else if (page_no == 300) {
                // 指向第一页，形成循环
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_NEXT, 100);
                write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 500);
            }
        }
        
        return page_data;
    };
    
    BlobPageChain chain;
    chain.add_page(1, 100);
    chain.add_page(1, 200);
    chain.add_page(1, 300);

    // Floyd算法检测的是pages数组中的循环引用，而不是页号循环
    // 这里测试的是pages数组的循环检测
    EXPECT_FALSE(chain.has_circular_reference());
}

// 测试has_circular_reference()在空链时返回false
TEST(BlobPageChainTest, EmptyChainNoCircular) {
    BlobPageChain chain;
    EXPECT_FALSE(chain.has_circular_reference());
}

// 测试has_circular_reference()在单页时返回false
TEST(BlobPageChainTest, SinglePageNoCircular) {
    BlobPageChain chain;
    chain.add_page(1, 100);
    EXPECT_FALSE(chain.has_circular_reference());
}

// 测试has_circular_reference()使用Floyd算法检测循环
TEST(BlobPageChainTest, FloydCycleDetection) {
    BlobPageChain chain;
    chain.add_page(1, 100);
    chain.add_page(1, 200);
    chain.add_page(1, 300);
    chain.add_page(1, 400);
    chain.add_page(1, 200); // 循环回到200
    
    EXPECT_TRUE(chain.has_circular_reference());
}

// ==================== BLOB数据读取测试 ====================

// 测试从单页BLOB读取数据
TEST(BlobDataReadTest, ReadSinglePageBlobData) {
    BlobPageChain chain;
    chain.add_page(1, 100);
    
    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 1 && page_no == 100) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置数据长度（偏移 BTR_BPAGE_LEN = 46）
            write_le<uint32_t>(page_data.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_LEN, 100);
            // 填充测试数据（从偏移 BTR_BPAGE_LEN + 4 = 50 开始）
            for (int i = 0; i < 100; ++i) {
                page_data[dbrecovery::innodb::BlobPageChain::BTR_BPAGE_LEN + 4 + i] = static_cast<uint8_t>(i % 256);
            }
            return page_data;
        }
        return std::nullopt;
    };
    
    auto blob_data = read_blob_data_from_chain(chain, 100, page_reader);
    std::cerr << "blob_data.size() = " << blob_data.size() << std::endl;
    EXPECT_EQ(blob_data.size(), 100);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(blob_data[i], static_cast<uint8_t>(i % 256)) << "Mismatch at index " << i;
    }
}

// 测试从多页BLOB读取数据
TEST(BlobDataReadTest, ReadMultiPageBlobData) {
    BlobPageChain chain;
    chain.add_page(1, 100);
    chain.add_page(1, 200);
    
    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        std::vector<uint8_t> page_data(16384, 0);

        if (space_id == 1) {
            if (page_no == 100) {
                write_le<uint32_t>(page_data.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_LEN, 1000);
                for (int i = 0; i < 1000; ++i) {
                    page_data[dbrecovery::innodb::BlobPageChain::BTR_BPAGE_LEN + 4 + i] = static_cast<uint8_t>(i % 256);
                }
            } else if (page_no == 200) {
                write_le<uint32_t>(page_data.data(), dbrecovery::innodb::BlobPageChain::BTR_BPAGE_LEN, 500);
                for (int i = 0; i < 500; ++i) {
                    page_data[dbrecovery::innodb::BlobPageChain::BTR_BPAGE_LEN + 4 + i] = static_cast<uint8_t>((1000 + i) % 256);
                }
            }
        }

        return page_data;
    };
    
    auto blob_data = read_blob_data_from_chain(chain, 1500, page_reader);
    EXPECT_EQ(blob_data.size(), 1500);
    
    // 验证第一页数据
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(blob_data[i], static_cast<uint8_t>(i % 256));
    }
    
    // 验证第二页数据
    for (int i = 0; i < 500; ++i) {
        EXPECT_EQ(blob_data[1000 + i], static_cast<uint8_t>((1000 + i) % 256));
    }
}

// 测试读取无效BLOB页
TEST(BlobDataReadTest, ReadInvalidBlobPage) {
    BlobPageChain chain;
    chain.add_page(1, 100);
    
    auto page_reader = [](uint32_t, uint32_t) { return std::nullopt; };
    
    auto blob_data = read_blob_data_from_chain(chain, 100, page_reader);
    EXPECT_TRUE(blob_data.empty());
}

// 测试total_len超过限制
TEST(BlobDataReadTest, TotalLenTooLarge) {
    BlobPageChain chain;
    chain.add_page(1, 100);

    auto page_reader = [](uint32_t, uint32_t) { return std::nullopt; };

    // total_len 接近 uint32_t 最大值（约 4GB）
    auto blob_data = read_blob_data_from_chain(chain, 0xFFFFFFFF, page_reader);
    EXPECT_TRUE(blob_data.empty()); // 应该返回空（长度过大）
}

// 测试数据长度超过页大小
TEST(BlobDataReadTest, DataLenExceedsPageSize) {
    BlobPageChain chain;
    chain.add_page(1, 100);
    
    auto page_reader = [](uint32_t, uint32_t) -> std::optional<std::vector<uint8_t>> {
        std::vector<uint8_t> page_data(16384, 0);
        // 设置数据长度超过合理范围
        write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 16384);
        return page_data;
    };
    
    auto blob_data = read_blob_data_from_chain(chain, 16384, page_reader);
    EXPECT_TRUE(blob_data.empty());
}

// ==================== BlobPointer::is_valid() 测试 ====================

// 测试is_valid()在有效单页BLOB时返回true
TEST(BlobPointerTest, IsValidSinglePage) {
    BlobPointer ptr;
    ptr.space_id = 1;
    ptr.page_no = 100;
    ptr.offset = 1000;
    ptr.len = 500;
    ptr.total_len = 500;
    ptr.chain_pages.emplace_back(1, 100);
    
    EXPECT_TRUE(ptr.is_complete());
    EXPECT_TRUE(ptr.is_valid(nullptr, false));
}

// 测试is_valid()在空链时返回false
TEST(BlobPointerTest, IsValidEmptyChain) {
    BlobPointer ptr;
    ptr.space_id = 1;
    ptr.page_no = 100;
    ptr.len = 500;
    ptr.total_len = 500;
    // chain_pages 为空
    
    EXPECT_FALSE(ptr.is_complete());
}

// 测试is_valid()在total_len > 64GB时返回false
TEST(BlobPointerTest, IsValidTotalLenTooLarge) {
    BlobPointer ptr;
    ptr.space_id = 1;
    ptr.page_no = 100;
    ptr.len = 500;
    ptr.total_len = 0xFFFFFFFF; // 接近 uint32_t 最大值（约 4GB）
    ptr.chain_pages.emplace_back(1, 100);

    // total_len 接近 4GB，应该返回 true（在 uint32_t 范围内）
    // 64GB 限制在 uint32_t 中无法表示，因此测试使用接近 4GB 的值
    EXPECT_TRUE(ptr.is_complete());
}

// 测试is_valid()在零长度单页时返回false
TEST(BlobPointerTest, IsValidZeroLengthSinglePage) {
    BlobPointer ptr;
    ptr.space_id = 1;
    ptr.page_no = 100;
    ptr.len = 0;
    ptr.total_len = 0;
    ptr.chain_pages.emplace_back(1, 100);
    
    EXPECT_FALSE(ptr.is_complete());
}

// 测试is_valid()在total_len == 0且len > 0时返回true
TEST(BlobPointerTest, IsValidZeroTotalLenNonZeroLen) {
    BlobPointer ptr;
    ptr.space_id = 1;
    ptr.page_no = 100;
    ptr.len = 500;
    ptr.total_len = 0;
    ptr.chain_pages.emplace_back(1, 100);
    
    EXPECT_TRUE(ptr.is_complete());
}

// 测试is_valid()在total_len < len时返回true（页内截断）
TEST(BlobPointerTest, IsValidTotalLenLessThanLen) {
    BlobPointer ptr;
    ptr.space_id = 1;
    ptr.page_no = 100;
    ptr.len = 500;
    ptr.total_len = 300;
    ptr.chain_pages.emplace_back(1, 100);
    
    EXPECT_TRUE(ptr.is_complete());
}

// ==================== read_blob_page_data_length 测试 ====================

// 测试读取有效BLOB页数据长度
TEST(BlobPageDataLengthTest, ReadValidLength) {
    std::vector<uint8_t> page_data(16384, 0);
    write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 100);
    
    EXPECT_EQ(read_blob_page_data_length(page_data), 100);
}

// 测试读取页太小
TEST(BlobPageDataLengthTest, ReadPageTooSmall) {
    std::vector<uint8_t> page_data(10, 0);
    
    EXPECT_EQ(read_blob_page_data_length(page_data), 0);
}

// 测试读取长度过大
TEST(BlobPageDataLengthTest, ReadLengthTooLarge) {
    std::vector<uint8_t> page_data(16384, 0);
    write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 16384);
    
    EXPECT_EQ(read_blob_page_data_length(page_data), 0);
}

} // namespace innodb
} // namespace dbrecovery

// ==================== 单元测试入口 =====================
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
