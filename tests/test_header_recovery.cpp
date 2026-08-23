#include <gtest/gtest.h>

#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/repair/header_repairer.hpp"

#include <vector>

namespace {

using dbrecovery::innodb::ChecksumAlgo;
using dbrecovery::innodb::FIL_PAGE_INDEX;
using dbrecovery::innodb::FIL_PAGE_OFFSET;
using dbrecovery::innodb::FIL_PAGE_TYPE_ABS;
using dbrecovery::innodb::write_be;
using dbrecovery::innodb::write_le;
using dbrecovery::innodb::write_page_checksum;
using dbrecovery::repair::HeaderRepairer;

std::vector<uint8_t> create_index_page(uint32_t page_no = 7) {
    std::vector<uint8_t> page(16384, 0);
    write_be<uint32_t>(page.data(), FIL_PAGE_OFFSET, page_no);
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_le<uint32_t>(page.data(), 44, 1);
    write_page_checksum(page.data(), page.size(), ChecksumAlgo::CRC32C);
    return page;
}

}  // namespace

TEST(HeaderRecoveryTest, RepairsInvalidIndexPageType) {
    HeaderRepairer repairer;
    auto page = create_index_page();
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, 0xFFFF);

    ASSERT_TRUE(repairer.repair_page_header(page.data(), page.size()));
    EXPECT_EQ(dbrecovery::innodb::read_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS),
              FIL_PAGE_INDEX);
    EXPECT_TRUE(repairer.validate_page_header(page.data(), page.size()));
    EXPECT_TRUE(dbrecovery::innodb::validate_page_checksum(
                    page.data(), page.size(), true)
                    .is_valid);
}

TEST(HeaderRecoveryTest, RewritesCorruptedChecksum) {
    HeaderRepairer repairer;
    auto page = create_index_page();
    page[100] ^= 0x01;

    ASSERT_FALSE(dbrecovery::innodb::validate_page_checksum(
                     page.data(), page.size(), false)
                     .is_valid);
    const uint32_t checksum = repairer.calculate_and_fix_checksum(page.data(), page.size());

    EXPECT_NE(checksum, 0u);
    EXPECT_TRUE(dbrecovery::innodb::validate_page_checksum(
                    page.data(), page.size(), true)
                    .is_valid);
}

TEST(HeaderRecoveryTest, RebuildsHeaderWithRequestedIdentity) {
    HeaderRepairer repairer;
    std::vector<uint8_t> page(16384, 0xFF);

    ASSERT_TRUE(repairer.rebuild_page_header(
        page.data(), page.size(), 23, FIL_PAGE_INDEX));
    EXPECT_EQ(dbrecovery::innodb::read_be<uint32_t>(page.data(), FIL_PAGE_OFFSET), 23u);
    EXPECT_EQ(dbrecovery::innodb::read_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS),
              FIL_PAGE_INDEX);

    repairer.calculate_and_fix_checksum(page.data(), page.size());
    EXPECT_TRUE(repairer.validate_page_header(page.data(), page.size()));
}

TEST(HeaderRecoveryTest, RejectsNullAndShortPages) {
    HeaderRepairer repairer;
    std::vector<uint8_t> short_page(73, 0);

    EXPECT_FALSE(repairer.repair_page_header(nullptr, 16384));
    EXPECT_FALSE(repairer.repair_page_header(short_page.data(), short_page.size()));
    EXPECT_EQ(repairer.calculate_and_fix_checksum(nullptr, 16384), 0u);
}
