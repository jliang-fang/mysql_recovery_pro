#include <gtest/gtest.h>
#include "dbrecovery/innodb/blob_chain_rebuilder.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== BlobChainRebuilder Tests ====================

TEST(BlobChainRebuilderTest, RebuildFromPointerSinglePage) {
    BlobChainRebuilder rebuilder;

    BlobPointer pointer{};
    pointer.space_id = 1;
    pointer.page_no = 100;
    pointer.offset = 0;
    pointer.len = 50;
    pointer.total_len = 50;
    pointer.chain_pages.emplace_back(1, 100);

    EXPECT_TRUE(rebuilder.rebuild_from_pointer(pointer));
    // 单页 BLOB 需要通过 rebuild_from_page 实际读取数据
    // 这里只测试指针解析
    EXPECT_EQ(rebuilder.chain_info().total_len, 50);
    EXPECT_EQ(rebuilder.chain_info().page_count, 1);  // 修复：应该是 1
    EXPECT_TRUE(rebuilder.chain_info().is_complete);
}

TEST(BlobChainRebuilderTest, RebuildFromPointerMultiPage) {
    BlobChainRebuilder rebuilder;

    BlobPointer pointer{};
    pointer.space_id = 1;
    pointer.page_no = 100;
    pointer.offset = 0;
    pointer.len = 50;
    pointer.total_len = 150;  // Multi-page
    pointer.chain_pages.emplace_back(1, 100);

    EXPECT_TRUE(rebuilder.rebuild_from_pointer(pointer));
    
    // 多页 BLOB 需要文件访问
    EXPECT_EQ(rebuilder.chain_info().total_len, 150);
    EXPECT_EQ(rebuilder.chain_info().page_count, 0);  // 未填充（需要文件访问）
}

TEST(BlobChainRebuilderTest, RebuildFromPage) {
    BlobChainRebuilder rebuilder;

    std::vector<uint8_t> page_data(16384, 0);

    // 设置页信息
    write_le<uint32_t>(page_data.data(), 24, 1);   // space_id
    write_le<uint32_t>(page_data.data(), 20, 100); // page_no

    // 设置 BLOB 数据（从偏移 70 开始）
    size_t blob_offset = 70;
    for (int i = 0; i < 100; i++) {
        page_data[blob_offset + i] = static_cast<uint8_t>(i % 256);
    }

    EXPECT_TRUE(rebuilder.rebuild_from_page(page_data.data(), page_data.size(), 
                                           blob_offset, 100));
    
    EXPECT_EQ(rebuilder.blob_data().size(), 100);
    EXPECT_EQ(rebuilder.chain_info().space_id, 1);
    EXPECT_EQ(rebuilder.chain_info().start_page_no, 100);
    EXPECT_EQ(rebuilder.chain_info().total_len, 100);
    EXPECT_TRUE(rebuilder.chain_info().is_complete);
}

TEST(BlobChainRebuilderTest, RebuildFromInvalidPage) {
    BlobChainRebuilder rebuilder;

    std::vector<uint8_t> page_data(100, 0);  // 太小

    EXPECT_FALSE(rebuilder.rebuild_from_page(page_data.data(), page_data.size(), 
                                            0, 100));
}

TEST(BlobChainRebuilderTest, RebuildFromInvalidOffset) {
    BlobChainRebuilder rebuilder;

    std::vector<uint8_t> page_data(16384, 0);

    // 偏移太大（超过页大小 - BLOB_DATA_OFFSET）
    // BLOB 数据从偏移 70 开始，所以最大有效偏移是 16384 - 70 - 1 = 16313
    EXPECT_FALSE(rebuilder.rebuild_from_page(page_data.data(), page_data.size(), 
                                           16314, 100));
    
    // 检查错误信息
    EXPECT_FALSE(rebuilder.chain_info().is_complete);
}

TEST(BlobChainRebuilderTest, FixChainBreaks) {
    BlobChainRebuilder rebuilder;

    std::vector<uint8_t> page_data(16384, 0);
    write_le<uint32_t>(page_data.data(), 24, 1);
    write_le<uint32_t>(page_data.data(), 20, 100);

    EXPECT_TRUE(rebuilder.rebuild_from_page(page_data.data(), page_data.size(), 
                                           70, 100));

    // 修复链断裂
    rebuilder.fix_chain_breaks();
    
    // 应该没有缺失的页（只有一个页）
    EXPECT_EQ(rebuilder.stats().missing_pages, 0);
    
    // 检查 stats 方法
    EXPECT_EQ(rebuilder.stats().total_pages, 1);
    EXPECT_EQ(rebuilder.stats().valid_pages, 1);
}

TEST(BlobChainRebuilderTest, DetectCircularReference) {
    BlobChainRebuilder rebuilder;

    std::vector<uint8_t> page_data(16384, 0);
    write_le<uint32_t>(page_data.data(), 24, 1);
    write_le<uint32_t>(page_data.data(), 20, 100);

    EXPECT_TRUE(rebuilder.rebuild_from_page(page_data.data(), page_data.size(), 
                                           70, 100));

    // 单页没有循环引用
    EXPECT_FALSE(rebuilder.has_circular_reference());
}

TEST(BlobChainRebuilderTest, GetMissingPages) {
    BlobChainRebuilder rebuilder;

    // 创建一个有断裂的链
    std::vector<uint8_t> page_data(16384, 0);
    write_le<uint32_t>(page_data.data(), 24, 1);
    write_le<uint32_t>(page_data.data(), 20, 100);

    EXPECT_TRUE(rebuilder.rebuild_from_page(page_data.data(), page_data.size(), 
                                           70, 100));

    // 单页没有缺失的页
    auto missing = rebuilder.get_missing_pages();
    EXPECT_EQ(missing.size(), 0);
}

TEST(BlobChainRebuilderTest, ExportToFile) {
    BlobChainRebuilder rebuilder;

    std::vector<uint8_t> page_data(16384, 0);
    write_le<uint32_t>(page_data.data(), 24, 1);
    write_le<uint32_t>(page_data.data(), 20, 100);

    // 设置 BLOB 数据
    for (int i = 0; i < 100; i++) {
        page_data[70 + i] = static_cast<uint8_t>(i);
    }

    EXPECT_TRUE(rebuilder.rebuild_from_page(page_data.data(), page_data.size(), 
                                           70, 100));

    // 导出到文件
    EXPECT_TRUE(rebuilder.export_to_file("/tmp/test_blob_export.bin"));
}

TEST(BlobChainRebuilderTest, BlobChainInfoToString) {
    BlobChainInfo info{};
    info.space_id = 1;
    info.start_page_no = 100;
    info.offset = 0;
    info.total_len = 1000;
    info.current_len = 1000;
    info.page_count = 5;
    info.is_complete = true;

    std::string str = info.to_string();
    EXPECT_NE(str.find("space_id=1"), std::string::npos);
    EXPECT_NE(str.find("start_page=100"), std::string::npos);
    EXPECT_NE(str.find("total_len=1000"), std::string::npos);
    EXPECT_NE(str.find("complete=true"), std::string::npos);  // 使用 std::boolalpha
}

TEST(BlobChainRebuilderTest, BlobPageInfoToString) {
    BlobPageInfo info{};
    info.space_id = 1;
    info.page_no = 100;
    info.offset = 0;
    info.len = 500;
    info.total_len = 1000;
    info.is_valid = true;

    std::string str = info.to_string();
    EXPECT_NE(str.find("space=1"), std::string::npos);
    EXPECT_NE(str.find("page=100"), std::string::npos);
    EXPECT_NE(str.find("len=500"), std::string::npos);
    EXPECT_NE(str.find("valid=true"), std::string::npos);  // 使用 std::boolalpha
}

TEST(BlobChainRebuilderTest, BlobChainStatsToString) {
    BlobChainStats stats{};
    stats.total_pages = 10;
    stats.valid_pages = 8;
    stats.missing_pages = 2;
    stats.fixed_chains = 3;
    stats.circular_references = 0;
    stats.total_size = 163840;

    std::string str = stats.to_string();
    EXPECT_NE(str.find("total=10"), std::string::npos);
    EXPECT_NE(str.find("valid=8"), std::string::npos);
    EXPECT_NE(str.find("missing=2"), std::string::npos);
    EXPECT_NE(str.find("size=163840"), std::string::npos);
}

TEST(BlobChainRebuilderTest, RebuildFromInvalidData) {
    BlobChainRebuilder rebuilder;

    // 空数据
    EXPECT_FALSE(rebuilder.rebuild_from_page(nullptr, 16384, 70, 100));
    
    // 检查错误信息
    EXPECT_FALSE(rebuilder.chain_info().is_complete);
}

TEST(BlobChainRebuilderTest, RebuildFromLargeBlob) {
    BlobChainRebuilder rebuilder;

    std::vector<uint8_t> page_data(16384, 0);
    write_le<uint32_t>(page_data.data(), 24, 1);
    write_le<uint32_t>(page_data.data(), 20, 100);

    // 设置大 BLOB 数据
    uint32_t blob_size = 8000;
    for (uint32_t i = 0; i < blob_size; i++) {
        page_data[70 + i] = static_cast<uint8_t>(i % 256);
    }

    EXPECT_TRUE(rebuilder.rebuild_from_page(page_data.data(), page_data.size(), 
                                           70, blob_size));
    
    EXPECT_EQ(rebuilder.blob_data().size(), blob_size);
}

TEST(BlobChainRebuilderTest, RebuildWithDifferentPageSizes) {
    // 4KB 页
    std::vector<uint8_t> page4k(4096, 0);
    write_le<uint32_t>(page4k.data(), 24, 1);
    write_le<uint32_t>(page4k.data(), 20, 100);

    BlobChainRebuilder rebuilder4k(4096);
    EXPECT_TRUE(rebuilder4k.rebuild_from_page(page4k.data(), page4k.size(), 
                                            70, 100));

    // 32KB 页
    std::vector<uint8_t> page32k(32768, 0);
    write_le<uint32_t>(page32k.data(), 24, 1);
    write_le<uint32_t>(page32k.data(), 20, 100);

    BlobChainRebuilder rebuilder32k(32768);
    EXPECT_TRUE(rebuilder32k.rebuild_from_page(page32k.data(), page32k.size(), 
                                             70, 100));
}

} // namespace innodb
} // namespace dbrecovery
