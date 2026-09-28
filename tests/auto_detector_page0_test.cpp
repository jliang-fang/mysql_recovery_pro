#include <gtest/gtest.h>

#include <vector>

#include "dbrecovery/innodb/auto_detector.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery::innodb {

TEST(AutoDetectorPage0, GenericIndexPageIsInconclusive) {
    std::vector<uint8_t> page(FIL_PAGE_SPACE_ID + sizeof(uint32_t), 0);
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_be<uint32_t>(page.data(), FIL_PAGE_SPACE_ID, 1);

    EXPECT_EQ(AutoDetector::detect_version_from_page0(page.data(), page.size()),
              MySQLVersion::UNKNOWN);
}

TEST(AutoDetectorPage0, DetectsExplicitSDIPage) {
    std::vector<uint8_t> page(FIL_PAGE_SPACE_ID + sizeof(uint32_t), 0);
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);

    EXPECT_EQ(AutoDetector::detect_version_from_page0(page.data(), page.size()),
              MySQLVersion::MYSQL_80);
}

TEST(AutoDetectorPage0, AmbiguousMetadataIsInconclusive) {
    EXPECT_EQ(AutoDetector::detect_version_from_page_type(FIL_PAGE_INDEX),
              MySQLVersion::UNKNOWN);
    EXPECT_EQ(AutoDetector::detect_version_from_space_id(0), MySQLVersion::UNKNOWN);
    EXPECT_EQ(AutoDetector::detect_version_from_space_id(1), MySQLVersion::UNKNOWN);
    EXPECT_EQ(AutoDetector::detect_version_from_page_size(4096), MySQLVersion::UNKNOWN);
    EXPECT_EQ(AutoDetector::detect_version_from_page_size(16384), MySQLVersion::UNKNOWN);
}

TEST(AutoDetectorPage0, FileHeaderReportsOnlyObservedSDI) {
    std::vector<uint8_t> index_page(16384, 0);
    write_be<uint16_t>(index_page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    auto index_result = AutoDetector::detect_from_file_header(index_page.data(), index_page.size());
    EXPECT_EQ(index_result.version, MySQLVersion::UNKNOWN);
    EXPECT_FALSE(index_result.has_sdi);

    std::vector<uint8_t> sdi_page(16384, 0);
    write_be<uint16_t>(sdi_page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_SDI);
    auto sdi_result = AutoDetector::detect_from_file_header(sdi_page.data(), sdi_page.size());
    EXPECT_EQ(sdi_result.version, MySQLVersion::MYSQL_80);
    EXPECT_TRUE(sdi_result.has_sdi);
}

TEST(AutoDetectorPage0, RejectsTruncatedAndNullInput) {
    std::vector<uint8_t> page(FIL_PAGE_SPACE_ID + sizeof(uint32_t), 0);
    write_be<uint16_t>(page.data(), FIL_PAGE_TYPE_ABS, FIL_PAGE_INDEX);
    write_be<uint32_t>(page.data(), FIL_PAGE_SPACE_ID, 1);

    EXPECT_EQ(AutoDetector::detect_version_from_page0(page.data(), page.size() - 1),
              MySQLVersion::UNKNOWN);
    EXPECT_EQ(AutoDetector::detect_version_from_page0(nullptr, 0), MySQLVersion::UNKNOWN);
}

}  // namespace dbrecovery::innodb
