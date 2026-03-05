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
    
    std::filesystem::remove(tmp_path);
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
    
    std::filesystem::remove(tmp_path);
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
