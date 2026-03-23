#include <gtest/gtest.h>
#include "dbrecovery/recovery/engine.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace recovery {

// 测试 RecoveryEngine 在未打开文件时的行为
TEST(RecoveryEngineTest, UnopenedFile) {
    RecoveryEngine engine;
    EXPECT_FALSE(engine.is_valid());

    // 尝试恢复 Truncate（应失败）
    catalog::TableSchema schema;
    auto records = engine.recover_truncate(schema);
    EXPECT_TRUE(records.empty());
}

// 测试 RecoveryEngine 在打开无效文件时的行为
TEST(RecoveryEngineTest, OpenInvalidFile) {
    RecoveryEngine engine;
    EXPECT_FALSE(engine.open("/nonexistent/file.ibd"));
    EXPECT_FALSE(engine.is_valid());
}

// 测试 RecoveryEngine 导出空记录集
TEST(RecoveryEngineTest, ExportEmptyRecords) {
    RecoveryEngine engine;

    std::vector<std::map<std::string, std::string>> empty_records;
    EXPECT_FALSE(engine.export_to_csv(empty_records, "/tmp/test_empty.csv"));
    EXPECT_FALSE(engine.export_to_sql(empty_records, "test_table",
"/tmp/test_empty.sql"));
}

// 测试 RecoveryEngine 导出非空记录集
TEST(RecoveryEngineTest, ExportRecords) {
    RecoveryEngine engine;

    std::vector<std::map<std::string, std::string>> records = {
        {{"id", "1"}, {"name", "test"}},
        {{"id", "2"}, {"name", "hello"}}
    };

    EXPECT_TRUE(engine.export_to_csv(records, "/tmp/test_export.csv"));
    EXPECT_TRUE(engine.export_to_sql(records, "test_table",
"/tmp/test_export.sql"));
}

// 测试 RecoveryEngine 修复损坏的文件头
TEST(RecoveryEngineTest, RepairHeader) {
    // 首先创建一个测试用的 .ibd 文件（使用 ibd_generator.py）
    // 然后尝试修复它

    RecoveryEngine engine;

    // 这里仅测试逻辑路径，不依赖实际文件
    EXPECT_FALSE(engine.repair_header("/tmp/test_repaired.ibd")); // 未打开文件
}

} // namespace recovery
} // namespace dbrecovery

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
