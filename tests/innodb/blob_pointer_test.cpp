#include <gtest/gtest.h>
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// 测试 BlobPointer::is_complete() 正常情况
TEST(BlobPointerTest, IsCompleteValid) {
    BlobPointer ptr;
    ptr.chain_pages.emplace_back(0, 100);
    ptr.total_len = 50;
    ptr.len = 50;

    EXPECT_TRUE(ptr.is_complete());
}

// 测试 BlobPointer::is_complete() 空链
TEST(BlobPointerTest, IsCompleteEmptyChain) {
    BlobPointer ptr;
    // chain_pages 为空

    EXPECT_FALSE(ptr.is_complete());
}

// 测试 BlobPointer::is_complete() total_len 过大
TEST(BlobPointerTest, IsCompleteTotalLenTooLarge) {
    BlobPointer ptr;
    ptr.chain_pages.emplace_back(0, 100);
    ptr.total_len = 128ULL * 1024 * 1024 * 1024; // >64GB
    ptr.len = 50;

    EXPECT_FALSE(ptr.is_complete());
}

// 测试 BlobPointer::is_complete() 单页零长度
TEST(BlobPointerTest, IsCompleteZeroLengthSinglePage) {
    BlobPointer ptr;
    ptr.chain_pages.emplace_back(0, 100);
    ptr.total_len = 0;
    ptr.len = 0; // 单页 BLOB 长度为 0 是无效的

    EXPECT_FALSE(ptr.is_complete());
}

// 测试 parse_blob_pointer 正常情况
TEST(BlobPointerTest, ParseValid) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint32_t>(data.data(), 0, 0);      // space_id
    write_le<uint32_t>(data.data(), 4, 100);    // page_no
    write_le<uint32_t>(data.data(), 8, 54);     // offset
    write_le<uint32_t>(data.data(), 12, 50);    // len
    write_le<uint32_t>(data.data(), 16, 50);    // total_len

    auto result = parse_blob_pointer(data);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->space_id, 0);
    EXPECT_EQ(result->page_no, 100);
    EXPECT_EQ(result->offset, 54);
    EXPECT_EQ(result->len, 50);
    EXPECT_EQ(result->total_len, 50);
}

// 测试 parse_blob_pointer 数据不足
TEST(BlobPointerTest, ParseDataTooSmall) {
    std::vector<uint8_t> data(15, 0); // 小于 16 字节

    auto result = parse_blob_pointer(data);
    EXPECT_FALSE(result.has_value());
}

// 测试 parse_blob_pointer 无效页号
TEST(BlobPointerTest, ParseInvalidPageNo) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint32_t>(data.data(), 4, 0xFFFFFFFF); // 无效页号

    auto result = parse_blob_pointer(data);
    EXPECT_FALSE(result.has_value());
}

// 测试 parse_blob_pointer 边界检查（偏移过大）
TEST(BlobPointerTest, ParseOffsetTooLarge) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint32_t>(data.data(), 8, 65536); // 超过最大页大小

    auto result = parse_blob_pointer(data);
    EXPECT_FALSE(result.has_value());
}

// 测试 BlobPageChain::is_valid() 正常情况
TEST(BlobPageChainTest, IsValidValid) {
    BlobPageChain chain;
    chain.add_page(0, 100);

    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            std::vector<uint8_t> page_data(16384, 0);
            // 设置数据长度字段（偏移 8）为 50
            write_le<uint32_t>(page_data.data(), BlobPageChain::BTR_BPAGE_LEN, 50);
            return page_data;
        }
        return std::nullopt;
    };

    EXPECT_TRUE(chain.is_valid(page_reader, true));
}

// 测试 BlobPageChain::is_valid() 空链
TEST(BlobPageChainTest, IsValidEmpty) {
    BlobPageChain chain;

    EXPECT_FALSE(chain.is_valid());
}

// 测试 BlobPageChain::is_valid() 重复页
TEST(BlobPageChainTest, IsValidDuplicatePages) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 100); // 重复

    EXPECT_FALSE(chain.is_valid());
}

} // namespace innodb
} // namespace dbrecovery
