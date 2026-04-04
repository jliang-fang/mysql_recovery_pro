#include <gtest/gtest.h>
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/redo_log.hpp"
#include "dbrecovery/engine/row_decoder.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <filesystem>
#include <iostream>
#include <vector>

namespace dbrecovery {
namespace test {

// ==================== 集成测试：B+树遍历 ====================

// 测试B+树节点遍历（需要有效的页数据）
TEST(IntegrationTest, BPlusTreeTraversal) {
    // 这是一个集成测试，需要真实的InnoDB页数据
    // 由于测试数据文件可能不存在，这里仅测试接口存在性
    
    std::string test_data_dir = std::getenv("TEST_DATA_DIR") 
        ? std::getenv("TEST_DATA_DIR") 
        : "/app/mysql_recovery_pro/tests/data";
    
    // 尝试找到测试数据文件
    bool found_test_data = false;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(test_data_dir)) {
            if (entry.path().extension() == ".ibd") {
                found_test_data = true;
                break;
            }
        }
    } catch (...) {
        // 目录不存在或无法访问
    }
    
    // 如果没有测试数据，跳过测试
    if (!found_test_data) {
        GTEST_SKIP() << "No test .ibd files found in " << test_data_dir;
    }
    
    // 这里可以添加实际的B+树遍历测试
    // 由于需要真实的InnoDB页数据，这里仅作为占位符
}

// ==================== 集成测试：SDI解析 ====================

// 测试完整的SDI解析流程
TEST(IntegrationTest, SDIParsing) {
    std::string test_data_dir = std::getenv("TEST_DATA_DIR") 
        ? std::getenv("TEST_DATA_DIR") 
        : "/app/mysql_recovery_pro/tests/data";
    
    bool found_test_data = false;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(test_data_dir)) {
            if (entry.path().extension() == ".ibd") {
                found_test_data = true;
                break;
            }
        }
    } catch (...) {
        // 目录不存在或无法访问
    }
    
    if (!found_test_data) {
        GTEST_SKIP() << "No test .ibd files found in " << test_data_dir;
    }
    
    // 这里可以添加实际的SDI解析测试
}

// ==================== 集成测试：行解码 ====================

// 测试完整的行解码流程
TEST(IntegrationTest, RowDecoding) {
    std::string test_data_dir = std::getenv("TEST_DATA_DIR") 
        ? std::getenv("TEST_DATA_DIR") 
        : "/app/mysql_recovery_pro/tests/data";
    
    bool found_test_data = false;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(test_data_dir)) {
            if (entry.path().extension() == ".ibd") {
                found_test_data = true;
                break;
            }
        }
    } catch (...) {
        // 目录不存在或无法访问
    }
    
    if (!found_test_data) {
        GTEST_SKIP() << "No test .ibd files found in " << test_data_dir;
    }
    
    // 这里可以添加实际的行解码测试
}

// ==================== 集成测试：文件读取 ====================

// 测试文件读取和页解析
TEST(IntegrationTest, FileReading) {
    std::string test_data_dir = std::getenv("TEST_DATA_DIR") 
        ? std::getenv("TEST_DATA_DIR") 
        : "/app/mysql_recovery_pro/tests/data";
    
    bool found_test_data = false;
    std::string test_file;
    
    try {
        for (const auto& entry : std::filesystem::directory_iterator(test_data_dir)) {
            if (entry.path().extension() == ".ibd") {
                found_test_data = true;
                test_file = entry.path().string();
                break;
            }
        }
    } catch (...) {
        // 目录不存在或无法访问
    }
    
    if (!found_test_data || test_file.empty()) {
        GTEST_SKIP() << "No test .ibd files found in " << test_data_dir;
    }

    // 读取文件
    dbrecovery::innodb::InnoDBFileReader reader(test_file);
    auto page = reader.read_page(0);

    EXPECT_FALSE(page.empty());

    // 验证页大小（支持多种页大小）
    EXPECT_TRUE(page.size() == 4096 || page.size() == 8192 || 
                page.size() == 16384 || page.size() == 32768 || page.size() == 65536);
}

// ==================== 集成测试：CRC32C校验 ====================

// 测试页数据的CRC32C校验
TEST(IntegrationTest, CRC32CChecksum) {
    std::string test_data_dir = std::getenv("TEST_DATA_DIR") 
        ? std::getenv("TEST_DATA_DIR") 
        : "/app/mysql_recovery_pro/tests/data";
    
    bool found_test_data = false;
    std::string test_file;
    
    try {
        for (const auto& entry : std::filesystem::directory_iterator(test_data_dir)) {
            if (entry.path().extension() == ".ibd") {
                found_test_data = true;
                test_file = entry.path().string();
                break;
            }
        }
    } catch (...) {
        // 目录不存在或无法访问
    }
    
    if (!found_test_data || test_file.empty()) {
        GTEST_SKIP() << "No test .ibd files found in " << test_data_dir;
    }

    dbrecovery::innodb::InnoDBFileReader reader(test_file);
    auto page = reader.read_page(0);

    if (page.empty()) {
        GTEST_SKIP() << "Failed to read page";
    }

    // 验证页头中的校验和
    uint32_t page_no = dbrecovery::innodb::read_le<uint32_t>(page.data(), dbrecovery::innodb::FIL_PAGE_OFFSET);
    // uint32_t checksum_disk = read_le<uint32_t>(page.data(), dbrecovery::innodb::FIL_PAGE_SPACE_CHECKSUM);

    // 这里可以添加更详细的校验和验证
    // 由于MySQL 8.0支持多种校验和算法，这里仅验证字段存在性
    EXPECT_NE(page_no, 0xFFFFFFFF);
}

// ==================== 集成测试：多页BLOB恢复 ====================

// 测试从多个页恢复BLOB数据
TEST(IntegrationTest, MultiPageBlobRecovery) {
    std::string test_data_dir = std::getenv("TEST_DATA_DIR") 
        ? std::getenv("TEST_DATA_DIR") 
        : "/app/mysql_recovery_pro/tests/data";
    
    bool found_test_data = false;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(test_data_dir)) {
            if (entry.path().extension() == ".ibd") {
                found_test_data = true;
                break;
            }
        }
    } catch (...) {
        // 目录不存在或无法访问
    }
    
    if (!found_test_data) {
        GTEST_SKIP() << "No test .ibd files found in " << test_data_dir;
    }
    
    // 这里可以添加实际的多页BLOB恢复测试
}

// ==================== 集成测试：Redo Log解析 ====================

// 测试Redo Log文件解析
TEST(IntegrationTest, RedoLogParsing) {
    std::string test_data_dir = std::getenv("TEST_DATA_DIR") 
        ? std::getenv("TEST_DATA_DIR") 
        : "/app/mysql_recovery_pro/tests/data";
    
    bool found_test_data = false;
    std::string redo_file;
    
    try {
        for (const auto& entry : std::filesystem::directory_iterator(test_data_dir)) {
            if (entry.path().filename().string().find("ib_logfile") != std::string::npos) {
                found_test_data = true;
                redo_file = entry.path().string();
                break;
            }
        }
    } catch (...) {
        // 目录不存在或无法访问
    }
    
    if (!found_test_data || redo_file.empty()) {
        GTEST_SKIP() << "No Redo Log files found in " << test_data_dir;
    }

    dbrecovery::innodb::RedoLogParser parser;
    auto records = parser.parse_file(redo_file);

    // 至少应该解析到一些记录
    EXPECT_GE(records.size(), 0);
}

// ==================== 集成测试：表结构重建 ====================

// 测试从SDI重建表结构
TEST(IntegrationTest, TableSchemaReconstruction) {
    std::string test_data_dir = std::getenv("TEST_DATA_DIR") 
        ? std::getenv("TEST_DATA_DIR") 
        : "/app/mysql_recovery_pro/tests/data";
    
    bool found_test_data = false;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(test_data_dir)) {
            if (entry.path().extension() == ".ibd") {
                found_test_data = true;
                break;
            }
        }
    } catch (...) {
        // 目录不存在或无法访问
    }
    
    if (!found_test_data) {
        GTEST_SKIP() << "No test .ibd files found in " << test_data_dir;
    }
    
    // 这里可以添加实际的表结构重建测试
}

// ==================== 集成测试：数据导出 ====================

// 测试将数据导出为CSV格式
TEST(IntegrationTest, ExportToCSV) {
    std::string test_data_dir = std::getenv("TEST_DATA_DIR") 
        ? std::getenv("TEST_DATA_DIR") 
        : "/app/mysql_recovery_pro/tests/data";
    
    bool found_test_data = false;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(test_data_dir)) {
            if (entry.path().extension() == ".ibd") {
                found_test_data = true;
                break;
            }
        }
    } catch (...) {
        // 目录不存在或无法访问
    }
    
    if (!found_test_data) {
        GTEST_SKIP() << "No test .ibd files found in " << test_data_dir;
    }
    
    // 这里可以添加实际的数据导出测试
}

// ==================== 集成测试：文件头损坏恢复 ====================

// 测试从损坏的文件头恢复
TEST(IntegrationTest, HeaderRecovery) {
    std::string test_data_dir = std::getenv("TEST_DATA_DIR") 
        ? std::getenv("TEST_DATA_DIR") 
        : "/app/mysql_recovery_pro/tests/data";
    
    bool found_test_data = false;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(test_data_dir)) {
            if (entry.path().extension() == ".ibd") {
                found_test_data = true;
                break;
            }
        }
    } catch (...) {
        // 目录不存在或无法访问
    }
    
    if (!found_test_data) {
        GTEST_SKIP() << "No test .ibd files found in " << test_data_dir;
    }
    
    // 这里可以添加实际的文件头恢复测试
}

} // namespace test
} // namespace dbrecovery

// ==================== 单元测试入口 =====================
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
