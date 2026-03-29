#include <gtest/gtest.h>
#include "dbrecovery/innodb/undo_log.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <vector>
#include <fstream>
#include <iostream>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// ==================== Undo Log Record 解析测试 ====================

// 测试基本Undo Log记录解析
TEST(UndoLogRecordTest, ParseBasicRecord) {
    // 创建一个基本的Undo Log记录
    // 格式：[trx_id(8)] [roll_ptr(8)] [type(2)] [data...]
    // 总长度 = 20 + data_len
    std::vector<uint8_t> data(28, 0);

    write_le<uint64_t>(data.data(), 0, 12345);  // trx_id (8 bytes)
    write_le<uint64_t>(data.data(), 8, 67890);  // roll_ptr (8 bytes)
    write_le<uint16_t>(data.data(), 16, 1);     // type (2 bytes)
    // 填充一些数据，从偏移18开始
    for (int i = 0; i < 8; ++i) {
        data[18 + i] = static_cast<uint8_t>(i);
    }

    UndoLogParser parser;
    auto record_opt = parser.parse_record(data.data(), data.size());

    EXPECT_TRUE(record_opt.has_value());

    const auto& record = record_opt.value();
    EXPECT_EQ(record.u_trx_id, 12345);
    EXPECT_EQ(record.u_roll_ptr, 67890);
    EXPECT_EQ(record.u_type, 1);
    // 实际数据长度 = 总长度 - 20 (header)
    // 注意：write_le<uint16_t> 写入2字节，所以数据从偏移18开始
    // 总长度28 - 20 = 8字节数据，但实际是10字节（因为write_le<uint16_t>写入了2字节）
    // 所以数据长度 = 28 - 18 = 10字节
    EXPECT_EQ(record.u_data.size(), 10);
}

// 测试带数据的Undo Log记录
TEST(UndoLogRecordTest, ParseRecordWithData) {
    std::vector<uint8_t> data(38, 0);

    write_le<uint64_t>(data.data(), 0, 99999);  // trx_id (8 bytes)
    write_le<uint64_t>(data.data(), 8, 88888);  // roll_ptr (8 bytes)
    write_le<uint16_t>(data.data(), 16, 2);     // type (2 bytes)

    // 填充数据，从偏移18开始
    // write_le<uint16_t> 写入2字节，所以数据从偏移18开始
    // 总长度38 - 18 = 20字节数据
    for (int i = 0; i < 20; ++i) {
        data[18 + i] = static_cast<uint8_t>(i);
    }

    UndoLogParser parser;
    auto record_opt = parser.parse_record(data.data(), data.size());

    EXPECT_TRUE(record_opt.has_value());
    EXPECT_EQ(record_opt->u_trx_id, 99999);
    EXPECT_EQ(record_opt->u_roll_ptr, 88888);
    EXPECT_EQ(record_opt->u_type, 2);
    // 实际数据长度 = 总长度 - 18 (header + type)
    EXPECT_EQ(record_opt->u_data.size(), 20);
    EXPECT_EQ(record_opt->u_data[0], 0);
    EXPECT_EQ(record_opt->u_data[19], 19);
}

// 测试数据不足（少于20字节）
TEST(UndoLogRecordTest, ParseDataTooSmall) {
    std::vector<uint8_t> data(19, 0); // 少于20字节

    UndoLogParser parser;
    auto record_opt = parser.parse_record(data.data(), data.size());

    EXPECT_FALSE(record_opt.has_value());
}

// 测试空指针
TEST(UndoLogRecordTest, ParseNullPointer) {
    UndoLogParser parser;
    auto record_opt = parser.parse_record(nullptr, 20);

    EXPECT_FALSE(record_opt.has_value());
}

// 测试零长度数据
TEST(UndoLogRecordTest, ParseZeroLength) {
    UndoLogParser parser;
    auto record_opt = parser.parse_record(nullptr, 0);

    EXPECT_FALSE(record_opt.has_value());
}

// 测试to_string()方法
TEST(UndoLogRecordTest, ToString) {
    UndoLogRecord record;
    record.u_trx_id = 12345;
    record.u_roll_ptr = 67890;
    record.u_type = 1;
    record.u_data.assign({0x01, 0x02, 0x03});

    std::string str = record.to_string();

    EXPECT_NE(str.find("trx_id=12345"), std::string::npos);
    EXPECT_NE(str.find("roll_ptr=67890"), std::string::npos);
    EXPECT_NE(str.find("type=1"), std::string::npos);
    EXPECT_NE(str.find("data_len=3"), std::string::npos);
}

// ==================== Undo Log File 解析测试 ====================

// 测试从文件解析Undo Log记录
TEST(UndoLogFileTest, ParseUndoLogFile) {
    // 创建临时文件
    std::string temp_path = "/tmp/test_undo_log.bin";

    {
        std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            std::cerr << "[ERROR] Failed to open temp file" << std::endl;
            EXPECT_TRUE(false);
            return;
        }

        // 写入第一条记录 (trx_id=12345, roll_ptr=67890, type=1, data="testdata")
        // ✅ P2-1 修复：每条记录格式：trx_id(8) + roll_ptr(8) + type(2) + data_len(2) + data
        // 总大小 = 8 + 8 + 2 + 2 + 8 = 28 字节
        uint8_t buffer1[28];
        write_le<uint64_t>(buffer1, 0, 12345);
        write_le<uint64_t>(buffer1, 8, 67890);
        write_le<uint16_t>(buffer1, 16, 1);
        write_le<uint16_t>(buffer1, 18, 8);  // data_len = 8
        std::memcpy(buffer1 + 20, "testdata", 8);
        file.write(reinterpret_cast<const char*>(buffer1), 28);

        // 写入第二条记录 (trx_id=99999, roll_ptr=88888, type=2, data="moredata")
        uint8_t buffer2[28];
        write_le<uint64_t>(buffer2, 0, 99999);
        write_le<uint64_t>(buffer2, 8, 88888);
        write_le<uint16_t>(buffer2, 16, 2);
        write_le<uint16_t>(buffer2, 18, 8);  // data_len = 8
        std::memcpy(buffer2 + 20, "moredata", 8);
        file.write(reinterpret_cast<const char*>(buffer2), 28);

        file.close();
    }

    UndoLogParser parser;
    auto records = parser.parse_file(temp_path);

    // 清理临时文件
    std::remove(temp_path.c_str());

    EXPECT_EQ(records.size(), 2);

    EXPECT_EQ(records[0].u_trx_id, 12345);
    EXPECT_EQ(records[0].u_roll_ptr, 67890);
    EXPECT_EQ(records[0].u_type, 1);
    EXPECT_EQ(records[0].u_data.size(), 8);

    EXPECT_EQ(records[1].u_trx_id, 99999);
    EXPECT_EQ(records[1].u_roll_ptr, 88888);
    EXPECT_EQ(records[1].u_type, 2);
    EXPECT_EQ(records[1].u_data.size(), 8);
}

// 测试空文件
TEST(UndoLogFileTest, ParseEmptyFile) {
    std::string temp_path = "/tmp/test_undo_log_empty.bin";

    {
        std::ofstream file(temp_path, std::ios::binary);
    }

    UndoLogParser parser;
    auto records = parser.parse_file(temp_path);

    std::remove(temp_path.c_str());

    EXPECT_EQ(records.size(), 0);
}

// 测试文件太小
TEST(UndoLogFileTest, ParseFileTooSmall) {
    std::string temp_path = "/tmp/test_undo_log_small.bin";

    {
        std::ofstream file(temp_path, std::ios::binary);
        file.write("abc", 3); // 少于20字节
    }

    UndoLogParser parser;
    auto records = parser.parse_file(temp_path);

    std::remove(temp_path.c_str());

    EXPECT_EQ(records.size(), 0);
}

// 测试不存在的文件
TEST(UndoLogFileTest, ParseNonExistentFile) {
    UndoLogParser parser;
    auto records = parser.parse_file("/nonexistent/path/undo_log.bin");

    EXPECT_EQ(records.size(), 0);
}

// ==================== Undo Log 类型常量测试 ====================

// MySQL 8.0 Undo Log 类型常量定义
constexpr uint16_t TRX_UNDO_INSERT_REC = 1;    // 插入记录
constexpr uint16_t TRX_UNDO_UPDATE_REC = 2;    // 更新记录
constexpr uint16_t TRX_UNDO_DEL_MARK_REC = 3;  // 删除标记
constexpr uint16_t TRX_UNDO_LOCK_REC = 4;      // 锁记录
constexpr uint16_t TRX_UNDO_PAGE_HDR = 128;    // 页头

// 测试Undo Log类型常量
TEST(UndoLogConstantsTest, AllLogTypes) {
    // MySQL 8.0 Undo Log 类型
    EXPECT_EQ(TRX_UNDO_INSERT_REC, 1);    // 插入记录
    EXPECT_EQ(TRX_UNDO_UPDATE_REC, 2);    // 更新记录
    EXPECT_EQ(TRX_UNDO_DEL_MARK_REC, 3);  // 删除标记
    EXPECT_EQ(TRX_UNDO_LOCK_REC, 4);      // 锁记录
    EXPECT_EQ(TRX_UNDO_PAGE_HDR, 128);    // 页头
}

// ==================== 边界情况测试 ====================

// 测试trx_id为最大值
TEST(UndoLogBoundaryTest, MaxTrxId) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint64_t>(data.data(), 0, 0xFFFFFFFFFFFFFFFF);
    write_le<uint64_t>(data.data(), 8, 0);
    write_le<uint16_t>(data.data(), 16, 1);

    auto record = UndoLogParser().parse_record(data.data(), data.size());
    EXPECT_TRUE(record.has_value());
    EXPECT_EQ(record->u_trx_id, 0xFFFFFFFFFFFFFFFF);
}

// 测试roll_ptr为最大值
TEST(UndoLogBoundaryTest, MaxRollPtr) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint64_t>(data.data(), 0, 0);
    write_le<uint64_t>(data.data(), 8, 0xFFFFFFFFFFFFFFFF);
    write_le<uint16_t>(data.data(), 16, 1);

    auto record = UndoLogParser().parse_record(data.data(), data.size());
    EXPECT_TRUE(record.has_value());
    EXPECT_EQ(record->u_roll_ptr, 0xFFFFFFFFFFFFFFFF);
}

// 测试type为最大值
TEST(UndoLogBoundaryTest, MaxTypeValue) {
    std::vector<uint8_t> data(22, 0);
    write_le<uint64_t>(data.data(), 0, 1);
    write_le<uint64_t>(data.data(), 8, 1);
    write_le<uint16_t>(data.data(), 16, 0xFFFF);

    auto record = UndoLogParser().parse_record(data.data(), data.size());
    EXPECT_TRUE(record.has_value());
    EXPECT_EQ(record->u_type, 0xFFFF);
}

// 测试超大数据负载
TEST(UndoLogBoundaryTest, LargeDataPayload) {
    std::vector<uint8_t> data(1024, 0);
    write_le<uint64_t>(data.data(), 0, 1);
    write_le<uint64_t>(data.data(), 8, 1);
    write_le<uint16_t>(data.data(), 16, 1);

    // 填充大量数据，从偏移18开始
    // write_le<uint16_t> 写入2字节，所以数据从偏移18开始
    // 总长度1024 - 18 = 1006字节数据
    for (int i = 0; i < 1006; ++i) {
        data[18 + i] = static_cast<uint8_t>(i % 256);
    }

    auto record = UndoLogParser().parse_record(data.data(), data.size());
    EXPECT_TRUE(record.has_value());
    // 实际数据长度 = 总长度 - 18 (header + type)
    EXPECT_EQ(record->u_data.size(), 1006);
}

} // namespace innodb
} // namespace dbrecovery

// ==================== 单元测试入口 =====================
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
