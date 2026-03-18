#include <gtest/gtest.h>
#include "dbrecovery/innodb/blob_pointer.hpp"

namespace dbrecovery {
namespace innodb {

// 测试：空链表（任何模式都应失败）
TEST(BlobChainTest, EmptyChain) {
    BlobPageChain chain;
    
    auto page_reader = [](uint32_t, uint32_t) { return std::nullopt; };
    EXPECT_FALSE(chain.is_valid(page_reader, true));
    EXPECT_FALSE(chain.is_valid(page_reader, false));
}

// 测试：无 page_reader + 严格模式（应失败）
TEST(BlobChainTest, NoReaderStrictMode) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    
    std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)> page_reader = nullptr;
    EXPECT_FALSE(chain.is_valid(page_reader, true));
}

// 测试：无 page_reader + 宽松模式（应成功）
TEST(BlobChainTest, NoReaderNonStrictMode) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    
    std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)> page_reader = nullptr;
    EXPECT_TRUE(chain.is_valid(page_reader, false));
}

// 测试：有 page_reader + 完整数据（应成功）
TEST(BlobChainTest, ValidChainWithReader) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 101);
    
    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            std::vector<uint8_t> data(16, 0);
            write_le<uint32_t>(data.data(), BlobPageChain::BTR_BPAGE_NEXT, 101); // next_bpage = 101
            return data;
        } else if (space_id == 0 && page_no == 101) {
            std::vector<uint8_t> data(16, 0);
            write_le<uint32_t>(data.data(), BlobPageChain::BTR_BPAGE_NEXT, 0xFFFFFFFF); // 链表结束
            return data;
        }
        return std::nullopt;
    };
    
    EXPECT_TRUE(chain.is_valid(page_reader, true));
}

// 测试：链断裂（应失败）
TEST(BlobChainTest, BrokenChain) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 102); // 实际 next 应为 101
    
    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            std::vector<uint8_t> data(16, 0);
            write_le<uint32_t>(data.data(), BlobPageChain::BTR_BPAGE_NEXT, 101); // 实际 next 是 101
            return data;
        }
        return std::nullopt;
    };
    
    EXPECT_FALSE(chain.is_valid(page_reader, true));
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_ENABLE_TESTS
