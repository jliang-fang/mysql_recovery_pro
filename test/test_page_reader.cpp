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

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
