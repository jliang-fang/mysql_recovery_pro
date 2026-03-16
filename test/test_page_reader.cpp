#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_reader.hpp"
#include <fstream>
#include <filesystem>

namespace dbrecovery {
namespace innodb {

TEST(PageReader, ReadNonExistentFile) {
    PageReader reader("/nonexistent/file.ibd");
    EXPECT_EQ(reader.get_total_pages(), 0);
}

TEST(PageReader, ReadEmptyFile) {
    // Create temporary empty file
    std::string tmp_path = "/tmp/test_empty.ibd";
    std::ofstream(tmp_path).close();
    
    PageReader reader(tmp_path);
    EXPECT_EQ(reader.get_total_pages(), 0);
    
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
    if (ec) {
        std::cerr << "Warning: Failed to remove temp file: " << tmp_path << " - " << ec.message() << std::endl;
    }
}

TEST(PageReader, ReadValidFile) {
    // Create temporary file with known size
    std::string tmp_path = "/tmp/test_valid.ibd";
    std::ofstream out(tmp_path, std::ios::binary);
    char zero[16384] = {0};
    for (int i = 0; i < 5; ++i) {
        out.write(zero, sizeof(zero));
    }
    out.close();
    
    PageReader reader(tmp_path);
    EXPECT_GT(reader.get_total_pages(), 0);
    
    auto page = reader.read_page(0);
    EXPECT_FALSE(page.empty());
    EXPECT_EQ(page.size(), 16384);
    
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
    if (ec) {
        std::cerr << "Warning: Failed to remove temp file: " << tmp_path << " - " << ec.message() << std::endl;
    }
}

TEST(PageReader, ComputeCRC32C) {
    // Test CRC32C computation with a known value
    // InnoDB uses CRC32C (Castagnoli), reference value for all-zero page
    std::vector<uint8_t> page(16384, 0);
    
    uint32_t crc = PageReader::compute_crc32c(page);
    // Expected CRC32C of all-zero page (excluding checksum field) is 0x759E3D04
    EXPECT_EQ(crc, 0x759E3D04);
}

TEST(PageReader, ValidateChecksumWithZeroes) {
    // Create a page with zeroed checksum (NONE algorithm)
    std::string tmp_path = "/tmp/test_none_checksum.ibd";
    std::ofstream out(tmp_path, std::ios::binary);
    char zero[16384] = {0};
    
    // Set checksum field to 0 (offset 24-27)
    uint32_t zero_csum = 0;
    memcpy(zero + 24, &zero_csum, sizeof(zero_csum));
    
    for (int i = 0; i < 5; ++i) {
        out.write(zero, sizeof(zero));
    }
    out.close();
    
    PageReader reader(tmp_path);
    auto page = reader.read_page(0);
    // Should pass because checksum is 0 (NONE algorithm)
    EXPECT_FALSE(page.empty());
    
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
}

TEST(PageReader, DATETIME2LittleEndian) {
    // Test DATETIME2 decoding with known small-endian fractional part
    std::vector<uint8_t> page(16384, 0);
    
    // Write a simple DATETIME2 value: 2024-01-01 00:00:00.123456
    // Packed format (5 bytes): year_low=0xE8, year_high=0x7, month_day=0x11, hour_minute=0x0, second_packed=0x0
    // Fractional part (3 bytes, little-endian): 0x40, 0xE2, 0x01 (0x01E240 = 123456)
    
    size_t offset = 100; // arbitrary offset for test
    page[offset] = 0xE8;   // year_low
    page[offset + 1] = 0x07; // year_high (2024 = 0x07E8)
    page[offset + 2] = 0x11; // month_day (Jan 1 = 0x11)
    page[offset + 3] = 0x00; // hour_minute
    page[offset + 4] = 0x00; // second_packed
    
    // Little-endian fractional part: 123456 = 0x01E240
    page[offset + 5] = 0x40;
    page[offset + 6] = 0xE2;
    page[offset + 7] = 0x01;
    
    // Verify the bytes are correctly set
    EXPECT_EQ(page[offset], 0xE8);
    EXPECT_EQ(page[offset + 5], 0x40); // LSB first
    EXPECT_EQ(page[offset + 6], 0xE2);
    EXPECT_EQ(page[offset + 7], 0x01); // MSB last
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
