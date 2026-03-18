#include <gtest/gtest.h>
#include "dbrecovery/innodb/blob_pointer.hpp"

namespace dbrecovery {
namespace innodb {

// 测试正常 BLOB 链表验证
TEST(BlobPageChainTest, IsValidNormalChain) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 101);

    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            std::vector<uint8_t> data(16384, 0);
            write_le<uint32_t>(data.data(), BlobPageChain::BTR_BPAGE_NEXT, 101); // next_bpage = 101
            return data;
        } else if (space_id == 0 && page_no == 101) {
            std::vector<uint8_t> data(16384, 0);
            write_le<uint32_t>(data.data(), BlobPageChain::BTR_BPAGE_NEXT, 0xFFFFFFFF); // 链表结束
            return data;
        }
        return std::nullopt;
    };

    EXPECT_TRUE(chain.is_valid(page_reader, true));
}

// 测试断裂 BLOB 链表验证（next_bpage 不匹配）
TEST(BlobPageChainTest, IsValidBrokenChain) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 102); // 期望 next_bpage = 102

    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && page_no == 100) {
            std::vector<uint8_t> data(16384, 0);
            write_le<uint32_t>(data.data(), BlobPageChain::BTR_BPAGE_NEXT, 101); // 实际 next_bpage = 101
            return data;
        }
        return std::nullopt;
    };

    EXPECT_FALSE(chain.is_valid(page_reader, true));
}

// 测试循环引用 BLOB 链表验证
TEST(BlobPageChainTest, IsValidCircularReference) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 101);

    auto page_reader = [](uint32_t space_id, uint32_t page_no) -> std::optional<std::vector<uint8_t>> {
        if (space_id == 0 && (page_no == 100 || page_no == 101)) {
            std::vector<uint8_t> data(16384, 0);
            write_le<uint32_t>(data.data(), BlobPageChain::BTR_BPAGE_NEXT, 101); // 循环指向自己
            return data;
        }
        return std::nullopt;
    };

    EXPECT_FALSE(chain.is_valid(page_reader, true));
}

// 测试空链表验证
TEST(BlobPageChainTest, IsValidEmptyChain) {
    BlobPageChain chain;

    auto page_reader = [](uint32_t, uint32_t) { return std::nullopt; };
    EXPECT_FALSE(chain.is_valid(page_reader, true));
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
