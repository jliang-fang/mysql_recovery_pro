#include <gtest/gtest.h>
#include "dbrecovery/innodb/record_chain_rebuilder.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== RecordChainRebuilder Tests ====================

TEST(RecordChainRebuilderTest, RebuildEmptyChain) {
    RecordChainRebuilder rebuilder;
    std::vector<RecordInfo> records;

    EXPECT_FALSE(rebuilder.rebuild_chain(records, 100));
    EXPECT_EQ(rebuilder.stats().total_records, 0);
}

TEST(RecordChainRebuilderTest, RebuildSingleRecord) {
    RecordChainRebuilder rebuilder;

    std::vector<RecordInfo> records;
    RecordInfo record{};
    record.offset = 100;
    record.next_offset = 150;
    record.heap_no = 1;
    record.is_deleted = false;
    record.is_min_rec = false;
    record.n_fields = 3;
    records.push_back(record);

    EXPECT_TRUE(rebuilder.rebuild_chain(records, 200));
    EXPECT_EQ(rebuilder.stats().total_records, 1);
}

TEST(RecordChainRebuilderTest, RebuildMultipleRecords) {
    RecordChainRebuilder rebuilder;

    std::vector<RecordInfo> records(3);

    // Record 1
    records[0].offset = 100;
    records[0].next_offset = 150;
    records[0].heap_no = 1;
    records[0].is_deleted = false;
    records[0].is_min_rec = false;
    records[0].n_fields = 3;

    // Record 2
    records[1].offset = 150;
    records[1].next_offset = 200;
    records[1].heap_no = 2;
    records[1].is_deleted = false;
    records[1].is_min_rec = false;
    records[1].n_fields = 3;

    // Record 3
    records[2].offset = 200;
    records[2].next_offset = 250;
    records[2].heap_no = 3;
    records[2].is_deleted = false;
    records[2].is_min_rec = false;
    records[2].n_fields = 3;

    EXPECT_TRUE(rebuilder.rebuild_chain(records, 300));
    EXPECT_EQ(rebuilder.stats().total_records, 3);
}

TEST(RecordChainRebuilderTest, SortByHeapNo) {
    RecordChainRebuilder rebuilder;

    std::vector<RecordInfo> records(3);

    // Out of order
    records[0].offset = 100;
    records[0].heap_no = 3;
    records[0].next_offset = 150;
    records[0].is_deleted = false;
    records[0].is_min_rec = false;
    records[0].n_fields = 3;

    records[1].offset = 150;
    records[1].heap_no = 1;
    records[1].next_offset = 200;
    records[1].is_deleted = false;
    records[1].is_min_rec = false;
    records[1].n_fields = 3;

    records[2].offset = 200;
    records[2].heap_no = 2;
    records[2].next_offset = 250;
    records[2].is_deleted = false;
    records[2].is_min_rec = false;
    records[2].n_fields = 3;

    EXPECT_TRUE(rebuilder.rebuild_chain(records, 300));

    // 排序后应该按 heap_no 顺序
    auto rebuilt = rebuilder.get_rebuilt_records();
    EXPECT_EQ(rebuilt[0].heap_no, 1);
    EXPECT_EQ(rebuilt[1].heap_no, 2);
    EXPECT_EQ(rebuilt[2].heap_no, 3);
}

TEST(RecordChainRebuilderTest, FixNextPointers) {
    RecordChainRebuilder rebuilder;

    std::vector<RecordInfo> records(3);

    records[0].offset = 100;
    records[0].heap_no = 1;
    records[0].next_offset = 999;  // Broken
    records[0].is_deleted = false;
    records[0].is_min_rec = false;
    records[0].n_fields = 3;

    records[1].offset = 150;
    records[1].heap_no = 2;
    records[1].next_offset = 888;  // Broken
    records[1].is_deleted = false;
    records[1].is_min_rec = false;
    records[1].n_fields = 3;

    records[2].offset = 200;
    records[2].heap_no = 3;
    records[2].next_offset = 777;  // Broken
    records[2].is_deleted = false;
    records[2].is_min_rec = false;
    records[2].n_fields = 3;

    EXPECT_TRUE(rebuilder.rebuild_chain(records, 300));

    // 修复后 next 指针应该正确
    auto rebuilt = rebuilder.get_rebuilt_records();
    EXPECT_EQ(rebuilt[0].next_offset, 150);
    EXPECT_EQ(rebuilt[1].next_offset, 200);
}

TEST(RecordChainRebuilderTest, DetectCircularReference) {
    RecordChainRebuilder rebuilder;

    std::vector<RecordInfo> records(3);

    // Create circular reference
    records[0].offset = 100;
    records[0].heap_no = 1;
    records[0].next_offset = 150;
    records[0].is_deleted = false;
    records[0].is_min_rec = false;
    records[0].n_fields = 3;

    records[1].offset = 150;
    records[1].heap_no = 2;
    records[1].next_offset = 200;
    records[1].is_deleted = false;
    records[1].is_min_rec = false;
    records[1].n_fields = 3;

    records[2].offset = 200;
    records[2].heap_no = 3;
    records[2].next_offset = 100;  // Circular: points back to record 0
    records[2].is_deleted = false;
    records[2].is_min_rec = false;
    records[2].n_fields = 3;

    EXPECT_TRUE(rebuilder.rebuild_chain(records, 300));

    // 重连后应该修复循环引用
    auto rebuilt = rebuilder.get_rebuilt_records();
    EXPECT_EQ(rebuilt[2].next_offset, 0xFFFD);  // SUPREMUM offset
}

TEST(RecordChainRebuilderTest, GetOrphanRecords) {
    RecordChainRebuilder rebuilder;

    std::vector<RecordInfo> records(3);

    records[0].offset = 100;
    records[0].heap_no = 1;
    records[0].next_offset = 999;  // Invalid
    records[0].is_deleted = false;
    records[0].is_min_rec = false;
    records[0].n_fields = 3;

    records[1].offset = 150;
    records[1].heap_no = 2;
    records[1].next_offset = 200;
    records[1].is_deleted = false;
    records[1].is_min_rec = false;
    records[1].n_fields = 3;

    records[2].offset = 200;
    records[2].heap_no = 3;
    records[2].next_offset = 0xFFFF;  // Valid (last record)
    records[2].is_deleted = false;
    records[2].is_min_rec = false;
    records[2].n_fields = 3;

    EXPECT_TRUE(rebuilder.rebuild_chain(records, 300));

    auto orphan_indices = rebuilder.get_orphan_records();
    
    // 重连后，所有记录都应该被连接
    // 但 records[0] 的原始 next_offset 是 999（无效）
    // 重连后应该被修复，所以没有孤立记录
    EXPECT_EQ(orphan_indices.size(), 0);
}

TEST(RecordChainRebuilderTest, BuildHeapNoMap) {
    RecordChainRebuilder rebuilder;

    std::vector<RecordInfo> records(3);

    records[0].offset = 100;
    records[0].heap_no = 10;
    records[0].next_offset = 150;
    records[0].is_deleted = false;
    records[0].is_min_rec = false;
    records[0].n_fields = 3;

    records[1].offset = 150;
    records[1].heap_no = 20;
    records[1].next_offset = 200;
    records[1].is_deleted = false;
    records[1].is_min_rec = false;
    records[1].n_fields = 3;

    records[2].offset = 200;
    records[2].heap_no = 30;
    records[2].next_offset = 250;
    records[2].is_deleted = false;
    records[2].is_min_rec = false;
    records[2].n_fields = 3;

    EXPECT_TRUE(rebuilder.rebuild_chain(records, 300));

    auto heap_no_map = rebuilder.build_heap_no_map();
    EXPECT_EQ(heap_no_map[10], 0);
    EXPECT_EQ(heap_no_map[20], 1);
    EXPECT_EQ(heap_no_map[30], 2);
}

TEST(RecordChainRebuilderTest, ParseRecordFields) {
    RecordChainRebuilder rebuilder;

    RecordInfo record{};
    record.raw_data.assign(50, 0);
    record.raw_data[0] = 0;  // info_bits
    record.raw_data[1] = 10; // next_offset low
    record.raw_data[2] = 0;  // next_offset high
    record.raw_data[3] = 1;  // heap_no low
    record.raw_data[4] = 0;  // heap_no high
    record.raw_data[5] = 3;  // n_fields

    // 设置字段长度
    record.raw_data[6] = 5;   // field 0 len
    record.raw_data[7] = 10;  // field 1 len
    record.raw_data[8] = 15;  // field 2 len

    // 设置字段数据
    for (int i = 0; i < 30; i++) {
        record.raw_data[9 + i] = static_cast<uint8_t>(i);
    }

    auto fields = rebuilder.parse_record_fields(record, 3);
    EXPECT_EQ(fields.size(), 3);
}

TEST(RecordChainRebuilderTest, RecordChainStatsToString) {
    RecordChainStats stats{};
    stats.total_records = 10;
    stats.fixed_chains = 5;
    stats.orphan_records = 2;
    stats.circular_references = 1;

    std::string str = stats.to_string();
    EXPECT_NE(str.find("total=10"), std::string::npos);
    EXPECT_NE(str.find("fixed=5"), std::string::npos);
    EXPECT_NE(str.find("orphan=2"), std::string::npos);
    EXPECT_NE(str.find("circular=1"), std::string::npos);
}

TEST(RecordChainRebuilderTest, SortByOffset) {
    RecordChainRebuilder rebuilder;

    std::vector<RecordInfo> records(3);

    // Out of order by heap_no, but in order by offset
    records[0].offset = 200;
    records[0].heap_no = 3;
    records[0].next_offset = 250;
    records[0].is_deleted = false;
    records[0].is_min_rec = false;
    records[0].n_fields = 3;

    records[1].offset = 100;
    records[1].heap_no = 1;
    records[1].next_offset = 150;
    records[1].is_deleted = false;
    records[1].is_min_rec = false;
    records[1].n_fields = 3;

    records[2].offset = 150;
    records[2].heap_no = 2;
    records[2].next_offset = 200;
    records[2].is_deleted = false;
    records[2].is_min_rec = false;
    records[2].n_fields = 3;

    EXPECT_TRUE(rebuilder.rebuild_chain(records, 300));

    // 先按 heap_no 排序
    auto rebuilt = rebuilder.get_rebuilt_records();
    EXPECT_EQ(rebuilt[0].heap_no, 1);
    EXPECT_EQ(rebuilt[1].heap_no, 2);
    EXPECT_EQ(rebuilt[2].heap_no, 3);

    // 然后按 offset 排序
    rebuilder.sort_by_offset();
    rebuilt = rebuilder.get_rebuilt_records();
    EXPECT_EQ(rebuilt[0].offset, 100);
    EXPECT_EQ(rebuilt[1].offset, 150);
    EXPECT_EQ(rebuilt[2].offset, 200);
}

} // namespace innodb
} // namespace dbrecovery
