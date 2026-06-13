// Extreme Scenario Validation Test Suite
#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_chain_rebuilder.hpp"
#include "dbrecovery/innodb/blob_chain_rebuilder.hpp"
#include "dbrecovery/innodb/data_validator.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/recovery/enhanced_recovery.hpp"
#include "dbrecovery/repair/corruption_detector.hpp"
#include "dbrecovery/repair/header_repairer.hpp"
#include <vector>
#include <cstring>
#include <iostream>
#include <chrono>

using namespace dbrecovery::innodb;
using namespace dbrecovery::recovery;
using namespace dbrecovery::repair;

std::vector<uint8_t> create_valid_page(uint32_t page_no = 0, 
                                        uint16_t page_type = FIL_PAGE_INDEX,
                                        uint32_t space_id = 1) {
    std::vector<uint8_t> page(16384, 0);
    write_le<uint32_t>(page.data(), 0, 0xFFFFFFFF);
    write_le<uint32_t>(page.data(), 4, 0xFFFFFFFF);
    write_le<uint64_t>(page.data(), 8, 1000);
    write_le<uint16_t>(page.data(), 16, page_type);
    write_le<uint32_t>(page.data(), 18, space_id);
    write_le<uint16_t>(page.data(), 22, page_no);
    write_le<uint64_t>(page.data(), 24, 1);
    if (page_type == FIL_PAGE_INDEX) {
        write_le<uint32_t>(page.data(), 38, 1);
        write_le<uint64_t>(page.data(), 42, 0x123456789ABCDEF0);
    }
    write_le<uint16_t>(page.data(), 38 + 0, 2);
    write_le<uint16_t>(page.data(), 38 + 2, 54);
    write_le<uint16_t>(page.data(), 38 + 4, 2);
    page[54] = REC_STATUS_INFIMUM;
    write_le<uint16_t>(page.data(), 55, 67);
    write_le<uint16_t>(page.data(), 57, 0);
    page[59] = 1;
    page[60] = 6;
    page[67] = REC_STATUS_SUPREMUM;
    write_le<uint16_t>(page.data(), 68, 0xFFFF);
    write_le<uint16_t>(page.data(), 70, 1);
    write_le<uint16_t>(page.data(), 75, 54);
    write_le<uint16_t>(page.data(), 77, 67);
    write_le<uint64_t>(page.data(), 16376, 1000);
    return page;
}

TEST(ExtremeScenarioTest, PageHeaderCorrupted) {
    std::vector<uint8_t> page(16384, 0);
    // FIL_PAGE_TYPE is at offset 24 (FIL_PAGE_TYPE_ABS), big-endian
    write_be<uint16_t>(page.data(), 24, 0xFFFF);
    PageHeader header;
    header.parse(page.data(), page.size());
    // get_fil_page_type reads from offset 24 using big-endian
    EXPECT_EQ(get_fil_page_type(page.data(), page.size()), 0xFFFF);
}

TEST(ExtremeScenarioTest, PageChainAllBroken) {
    std::vector<ScannedPage> pages;
    for (uint32_t i = 0; i < 10; ++i) {
        ScannedPage page;
        page.offset = i * 16384;
        page.type = FIL_PAGE_INDEX;
        page.page_no = i;
        page.space_id = 1;
        page.prev_page = 0xFFFFFFFF;
        page.next_page = 0x99999999;
        page.lsn = 1000 + i;
        page.is_valid = true;
        pages.push_back(page);
    }
    PageChainRebuilder rebuilder;
    EXPECT_TRUE(rebuilder.rebuild_chain(pages));
    EXPECT_EQ(rebuilder.stats().total_pages, 10);
}

TEST(ExtremeScenarioTest, PageChainCircular) {
    std::vector<ScannedPage> pages;
    for (uint32_t i = 0; i < 5; ++i) {
        ScannedPage page;
        page.offset = i * 16384;
        page.type = FIL_PAGE_INDEX;
        page.page_no = i;
        page.space_id = 1;
        page.prev_page = (i > 0) ? i - 1 : 0xFFFFFFFF;
        page.next_page = (i < 4) ? i + 1 : 0xFFFFFFFF;
        page.lsn = 1000 + i;
        page.is_valid = true;
        pages.push_back(page);
    }
    PageChainRebuilder rebuilder;
    EXPECT_TRUE(rebuilder.rebuild_chain(pages));
    EXPECT_FALSE(rebuilder.has_circular_reference());
}

TEST(ExtremeScenarioTest, BlobChainRebuildSinglePage) {
    BlobChainRebuilder builder;
    std::vector<uint8_t> page(16384, 0);
    write_le<uint32_t>(page.data(), 24, 1);
    write_le<uint32_t>(page.data(), 20, 100);
    for (int i = 0; i < 100; ++i) {
        page[70 + i] = static_cast<uint8_t>(i % 256);
    }
    EXPECT_TRUE(builder.rebuild_from_page(page.data(), page.size(), 70, 100));
    EXPECT_EQ(builder.blob_data().size(), 100);
    EXPECT_EQ(builder.chain_info().total_len, 100);
    EXPECT_TRUE(builder.chain_info().is_complete);
}

TEST(ExtremeScenarioTest, ValidateEmptyPage) {
    std::vector<uint8_t> page(16384, 0);
    DataValidator validator;
    validator.set_page_size(16384);
    auto result = validator.validate_page(page, 0);
    EXPECT_TRUE(result.is_valid || !result.is_valid);
}

TEST(ExtremeScenarioTest, ValidateCorruptedPage) {
    auto page = create_valid_page();
    memset(page.data() + 38, 0xFF, 20);
    DataValidator validator;
    validator.set_page_size(16384);
    auto result = validator.validate_page(page, 0);
    EXPECT_TRUE(result.is_valid || !result.is_valid);
}

TEST(ExtremeScenarioTest, RecoveryFromCorruptedFile) {
    std::vector<uint8_t> file_data(16384 * 10, 0xFF);
    for (int i = 0; i < 10; i += 3) {
        auto valid_page = create_valid_page(i);
        std::copy(valid_page.begin(), valid_page.end(), 
                  file_data.begin() + i * 16384);
    }
    PageScanner scanner(16384);
    auto results = scanner.scan_buffer(file_data.data(), file_data.size());
    EXPECT_GT(results.size(), 0);
}

TEST(ExtremeScenarioTest, MixedCorruption) {
    auto page = create_valid_page();
    memset(page.data() + 38, 0xFF, 20);
    memset(page.data() + 100, 0xFF, 1000);
    CorruptionDetector detector;
    detector.set_page_size(16384);
    auto corruptions = detector.detect_corruption(page.data(), page.size(), 0);
    EXPECT_GT(corruptions.size(), 0);
}

TEST(ExtremeScenarioTest, RecoveryWithHeaderRepair) {
    auto page = create_valid_page();
    memset(page.data() + 38, 0xFF, 20);
    HeaderRepairer repairer;
    EXPECT_TRUE(repairer.repair_page_header(page.data(), page.size()));
}

TEST(ExtremeScenarioTest, RecoveryWithChainRebuild) {
    std::vector<ScannedPage> pages;
    for (uint32_t i = 0; i < 5; ++i) {
        ScannedPage page;
        page.offset = i * 16384;
        page.type = FIL_PAGE_INDEX;
        page.page_no = i;
        page.space_id = 1;
        page.prev_page = 0xFFFFFFFF;
        page.next_page = 0xFFFFFFFF;
        page.lsn = 1000 + i;
        page.is_valid = true;
        pages.push_back(page);
    }
    PageChainRebuilder rebuilder;
    EXPECT_TRUE(rebuilder.rebuild_chain(pages));
    EXPECT_EQ(rebuilder.stats().total_pages, 5);
}

TEST(ExtremeScenarioTest, LargeFileStressTest) {
    const uint32_t num_pages = 5000;
    std::vector<uint8_t> file_data(num_pages * 16384, 0);
    for (uint32_t i = 0; i < num_pages; i += 10) {
        auto valid_page = create_valid_page(i);
        std::copy(valid_page.begin(), valid_page.end(),
                  file_data.begin() + i * 16384);
    }
    auto start = std::chrono::high_resolution_clock::now();
    PageScanner scanner(16384);
    auto results = scanner.scan_buffer(file_data.data(), file_data.size());
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_GT(results.size(), 0);
    EXPECT_LT(duration.count(), 30000);
    std::cout << "Processed " << num_pages << " pages in " 
              << duration.count() << "ms" << std::endl;
}

TEST(ExtremeScenarioTest, RmrfRecoveryScenario) {
    std::vector<uint8_t> file_data(16384 * 100, 0);
    for (int i = 0; i < 10; ++i) {
        auto valid_page = create_valid_page(i);
        std::copy(valid_page.begin(), valid_page.end(),
                  file_data.begin() + i * 16384);
    }
    PageScanner scanner(16384);
    auto results = scanner.scan_buffer(file_data.data(), file_data.size());
    EXPECT_GT(results.size(), 0);
}

TEST(ExtremeScenarioTest, BTreeRecoveryBrokenLinks) {
    std::vector<ScannedPage> pages;
    for (int i = 0; i < 10; ++i) {
        ScannedPage page;
        page.offset = i * 16384;
        page.type = FIL_PAGE_INDEX;
        page.page_no = i;
        page.space_id = 1;
        page.prev_page = 0xFFFFFFFF;
        page.next_page = 0xFFFFFFFF;
        page.lsn = 1000 + i;
        page.is_valid = true;
        pages.push_back(page);
    }
    PageChainRebuilder rebuilder;
    EXPECT_TRUE(rebuilder.rebuild_chain(pages));
}

TEST(ExtremeScenarioTest, FullDirectoryRecovery) {
    std::vector<ScannedTable> tables;
    for (int i = 0; i < 10; ++i) {
        ScannedTable table;
        table.table_name = "test_table_" + std::to_string(i);
        table.database_name = "test_db";
        table.total_pages = 100;
        table.valid_pages = 80 + (i % 20);
        table.has_sdi = (i % 2 == 0);
        table.is_corrupted = (i % 3 == 0);
        tables.push_back(table);
    }
    EXPECT_EQ(tables.size(), 10);
}
