#include <gtest/gtest.h>
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== BPlusTreeParser 测试 ====================

TEST(BPlusTreeParser, ParseValidLeafPage) {
    // 创建一个有效的叶子页
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置页类型为 FIL_PAGE_INDEX (9969)
    write_le<uint16_t>(page_data.data(), 50, FIL_PAGE_INDEX);
    
    // 设置页级别为 0（叶子节点）
    write_le<uint16_t>(page_data.data(), 42, 0);
    
    // 设置目录槽数量
    write_le<uint16_t>(page_data.data(), 38, 5);  // n_dir_slots
    
    // 设置堆顶位置
    write_le<uint16_t>(page_data.data(), 40, 100);  // heap_top
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
}

TEST(BPlusTreeParser, ParseInvalidPageType) {
    // 创建一个无效页类型的数据
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置页类型为 FIL_PAGE_TYPE_ALLOCATED (0)
    write_le<uint16_t>(page_data.data(), 50, FIL_PAGE_TYPE_ALLOCATED);
    
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

TEST(BPlusTreeParser, ParseEmptyData) {
    std::vector<uint8_t> page_data;
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
