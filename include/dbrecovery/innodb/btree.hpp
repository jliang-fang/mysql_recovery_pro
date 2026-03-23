#ifndef DBRECOVERY_INNODB_BTREE_HPP
#define DBRECOVERY_INNODB_BTREE_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <memory>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief B+Tree 节点类型
 */
enum class BTreeNodeType : uint8_t {
    ROOT = 0,
    INTERNAL = 1,
    LEAF = 2,
    INVALID = 255
};

/**
 * @brief B+Tree 索引页结构解析器
 */
class BPlusTreeParser {
public:
    explicit BPlusTreeParser(const std::vector<uint8_t>& page_data);
    
    // ✅ 统一异常规范：所有解析方法均不标记 noexcept，确保异常可传播
    bool parse();
    
    [[nodiscard]] bool is_valid_page_type() const { return valid_page_type_; }
    [[nodiscard]] BTreeNodeType node_type() const { return node_type_; }
    
    // ✅ 新增：提供 record_offsets 的公共访问器（供测试使用）
    [[nodiscard]] const std::vector<uint16_t>& record_offsets() const { return record_offsets_; }

private:
    bool valid_page_type_ = false;
    uint32_t page_level_ = 0xFFFFFFFF;
    uint32_t next_page_ = 0xFFFFFFFF;
    uint32_t prev_page_ = 0xFFFFFFFF;
    uint16_t n_records_ = 0;
    std::vector<uint16_t> record_offsets_;
    
    // ✅ 统一：页健康状态检查（消除重复定义）
    struct PageHealth {
        bool is_valid = true;
        std::string status_message;
        uint32_t checksum_expected = 0;
        uint32_t checksum_actual = 0;
        bool checksum_match = false;
        
        // B+Tree specific
        bool has_infimum = false;
        bool has_supremum = false;
        uint16_t n_records = 0;
        uint16_t n_dir_slots = 0;
        uint16_t page_level = 0;
        uint32_t next_page = 0;
        uint32_t prev_page = 0;
    };
    
    [[nodiscard]] PageHealth check_health() const;

private:
    std::vector<uint8_t> page_data_;
    BTreeNodeType node_type_ = BTreeNodeType::INVALID;
    uint32_t page_level_ = 0;
    uint32_t next_page_ = 0xFFFFFFFF;
    uint32_t prev_page_ = 0xFFFFFFFF;
    uint16_t n_records_ = 0;
    std::vector<uint16_t> record_offsets_;
    uint8_t btr_format_ = 0;  // B+树记录格式标志位 (BTR_FORMAT_MASK)
    uint16_t flags_ = 0;      // B+树标志位（用于存储行格式等信息）
    
    // INFIMUM/SUPREMUM 记录偏移
    uint16_t infimum_offset_ = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    uint16_t supremum_offset_ = 0; // 实际值在 parse_infimum_supremum() 中计算

public:
    /**
     * @brief 解析记录数据（仅支持 COMPACT 格式）
     * 
     * @param schema 表结构定义
     * @return std::vector<std::map<std::string, std::string>> 解析后的记录
     */
    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const;

    // ✅ 统一：格式校验方法
    bool is_compact_format() const;
    
    // ✅ 新增：SDI 页解析支持
    [[nodiscard]] uint64_t get_sdi_table_id() const;

private:
    /**
     * @brief 解析单个字段的值（根据列类型）
     * 
     * @param col 列定义
     * @param data_pos 数据位置指针（会更新）
     * @param is_extern 是否为外部存储（如 BLOB 指针）
     * @return std::string 解析后的字符串值
     */
    std::string parse_field_value(
        const catalog::ColumnInfo& col, size_t& data_pos, bool is_extern) const;

    // ✅ 新增：DATETIME2 字段解析辅助函数
    std::string parse_datetime2_value(uint8_t precision, size_t& data_pos) const;

    /**
     * @brief 解析页的目录槽（record directory）
     * 
     * @return true 目录解析成功
     * @return false 目录解析失败
     */
    bool parse_directory();
    
    // ✅ 新增：解析页头信息
    bool parse_header();
    
    // ✅ 修复：移除 inline 和 noexcept，与 .cpp 实现保持一致
    bool parse_infimum_supremum();
    
    // ✅ 修复：移除 inline 和 noexcept，与 .cpp 实现保持一致
    bool parse_user_records();

private:
    uint16_t page_type_ = 0; // ✅ 新增：存储页类型，供 SDI 页判断使用

public:
    /**
     * @brief 解析记录数据（仅支持 COMPACT 格式）
     * 
     * @param schema 表结构定义
     * @return std::vector<std::map<std::string, std::string>> 解析后的记录
     */
    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const {
        std::vector<std::map<std::string, std::string>> records;
        
        if (!is_compact_format()) {
            // 仅支持 COMPACT 格式
            return records;
        }
        
        // 解析每个用户记录
        for (size_t i = 0; i < record_offsets_.size(); ++i) {
            uint16_t offset = record_offsets_[i];
            
            // 跳过 INFIMUM 和 SUPREMUM 系统记录
            if (offset == 0 || offset >= page_data_.size() - 5) {
                continue;
            }
            
            // 解析记录头（COMPACT格式）
            if (offset + 6 > page_data_.size()) {
                continue;
            }
            
            uint8_t info_bits = page_data_[offset];
            uint16_t next_offset = dbrecovery::innodb::read_le<uint16_t>(page_data_.data() + offset + 1, 0) & 0x3FFF;
            
            // 跳过已删除的记录
            if (info_bits & dbrecovery::innodb::REC_INFO_DELETED_FLAG) {
                continue;
            }
            
            // ✅ 统一：解析 n_fields 字段数量（位于记录头第5字节）
            uint16_t n_fields = page_data_[offset + 5];
            if (n_fields == 0 || n_fields > schema.columns.size()) {
                // 兼容性处理：如果无法确定字段数，使用schema中的字段数
                n_fields = static_cast<uint16_t>(schema.columns.size());
            }
            
            // 解析 NULL 位图（COMPACT格式：NULL位图在记录头后）
            size_t data_pos = offset + 5; // 记录头从 offset 开始，第5字节是 info_bits
            uint32_t nullable_count = 0;
            for (const auto& pair : schema.columns) {
                if (pair.second.is_nullable) {
                    nullable_count++;
                }
            }
            size_t null_bitmap_size = (nullable_count + 7) / 8;
            
            // ✅ 修复：跳过 info_bits 后，data_pos 应该指向 NULL 位图的开头
            data_pos += 1; // info_bits
            
            if (data_pos + null_bitmap_size > page_data_.size()) {
                break;
            }
            
            // ✅ 新增：确保 NULL 位图解析正确处理非空列跳过逻辑
            // COMPACT 格式中，NULL 位图只包含可空列，顺序与 schema.columns 中的定义顺序一致
            
            // 解析字段数据
            std::map<std::string, std::string> record;
            
            // ✅ 统一：根据实际解析的字段数进行循环
            uint16_t parsed_fields = 0;
            size_t nullable_idx = 0; // NULL 位图中的索引
            
            for (const auto& pair : schema.columns) {
                const auto& col = pair.second;
                
                if (data_pos >= page_data_.size() || parsed_fields >= n_fields) {
                    break;
                }
                
                // ✅ 新增：对于非空列，跳过 NULL 位图中的对应位
                bool is_nullable_col = col.is_nullable;
                
                // 检查当前列是否在 NULL 位图中（仅当它是可空列时）
                if (is_nullable_col) {
                    // ✅ 修复：data_pos 现在指向 NULL 位图的开头，直接使用
                    size_t byte_idx = nullable_idx / 8;
                    size_t bit_idx = nullable_idx % 8;
                    
                    if (data_pos + byte_idx < page_data_.size()) {
                        uint8_t bitmap_byte = page_data_[data_pos + byte_idx];
                        bool is_null = (bitmap_byte & (1 << bit_idx)) != 0;
                        
                        if (is_null) {
                            record[col.name] = "NULL";
                            nullable_idx++;
                            parsed_fields++;
                            continue; // 跳过字段值解析
                        }
                    }
                    nullable_idx++;
                }
                
                // ✅ 修复：移动 data_pos 到字段数据的起始位置（跳过 NULL 位图）
                if (is_nullable_col && nullable_idx == 0) {
                    data_pos += null_bitmap_size;
                }
                
                std::string value = parse_field_value(col, data_pos, false);
                record[col.name] = value;
                parsed_fields++;
            }
            
            records.push_back(record);
        }
        
        return records;
    }
    
    // ✅ 统一：格式校验方法
    bool is_compact_format() const {
        return (btr_format_ & 0x80) == 0; // COMPACT 格式标志位为 0
    }
    
    // ✅ 新增：SDI 页解析支持
    [[nodiscard]] uint64_t get_sdi_table_id() const;

private:
    /**
     * @brief 解析单个字段的值（根据列类型）
     * 
     * @param col 列定义
     * @param data_pos 数据位置指针（会更新）
     * @param is_extern 是否为外部存储（如 BLOB 指针）
     * @return std::string 解析后的字符串值
     */
    std::string parse_field_value(
        const catalog::ColumnInfo& col, size_t& data_pos, bool is_extern) const;

    // ✅ 新增：DATETIME2 字段解析辅助函数
    std::string parse_datetime2_value(uint8_t precision, size_t& data_pos) const;

    /**
     * @brief 解析页的目录槽（record directory）
     * 
     * @return true 目录解析成功
     * @return false 目录解析失败
     */
    bool parse_directory();
    
    // ✅ 新增：解析页头信息
    bool parse_header();
    
    // ✅ 修复：移除 inline 和 noexcept，与 .cpp 实现保持一致
    bool parse_infimum_supremum();
    
    // ✅ 修复：移除 inline 和 noexcept，与 .cpp 实现保持一致
    bool parse_user_records();

private:
    std::vector<uint8_t> page_data_;
    BTreeNodeType node_type_ = BTreeNodeType::INVALID;
    uint32_t page_level_ = 0;
    uint32_t next_page_ = 0xFFFFFFFF;
    uint32_t prev_page_ = 0xFFFFFFFF;
    uint16_t n_records_ = 0;
    std::vector<uint16_t> record_offsets_;
    uint8_t btr_format_ = 0;  // B+树记录格式标志位 (BTR_FORMAT_MASK)
    uint16_t flags_ = 0;      // B+树标志位（用于存储行格式等信息）
    
    // INFIMUM/SUPREMUM 记录偏移
    uint16_t infimum_offset_ = dbrecovery::innodb::PAGE_INFIMUM_REC_OFFSET_ABS;
    uint16_t supremum_offset_ = 0; // 实际值在 parse_infimum_supremum() 中计算

    bool parse_header();
    bool parse_infimum_supremum();
    bool parse_user_records();
};

// ==================== 单元测试入口（供 GTest 调用）====================
#ifdef DBRECOVERY_ENABLE_TESTS
#include <gtest/gtest.h>
#include <vector>

namespace dbrecovery {
namespace innodb {

// 测试 BPlusTreeParser 在页太小时返回 false
TEST(BPlusTreeParserTest, ParseTooSmallPage) {
    std::vector<uint8_t> page_data(50, 0); // 小于最小大小 (38 + 16 = 54)
    
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

// 测试 BPlusTreeParser 在页头信息无效时返回 false
TEST(BPlusTreeParserTest, ParseInvalidPageHeader) {
    std::vector<uint8_t> page_data(1024, 0);
    
    // 设置一个无效的页类型（非 FIL_PAGE_INDEX/SDI/UNDO_LOG）
    write_le<uint16_t>(page_data.data(), 38 + 12, 0xFFFF);
    
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

// 测试 BPlusTreeParser 在 INFIMUM/SUPREMUM 记录无效时返回 false
TEST(BPlusTreeParserTest, ParseInvalidInfimumSupremum) {
    std::vector<uint8_t> page_data(1024, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), 38 + 12, FIL_PAGE_INDEX);
    
    // 设置堆顶位置（使 INFIMUM/SUPREMUM 解析失败）
    write_le<uint16_t>(page_data.data(), 38 + 4, 50); // heap_top = 50
    
    BPlusTreeParser parser(page_data);
    EXPECT_FALSE(parser.parse());
}

// 测试 BPlusTreeParser 在正常情况下返回 true
TEST(BPlusTreeParserTest, ParseValidPage) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的页类型
    write_le<uint16_t>(page_data.data(), 38 + 12, FIL_PAGE_INDEX);
    
    // 设置堆顶位置（足够大以包含 INFIMUM 和 SUPREMUM）
    write_le<uint16_t>(page_data.data(), 38 + 4, 100); // heap_top = 100
    
    // 设置有效的 INFIMUM 记录（偏移 54）
    page_data[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page_data.data(), 55, 100); // next_record_offset
    
    // 设置有效的 SUPREMUM 记录
    page_data[99] = REC_STATUS_SUPREMUM;
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
}

// 测试 BPlusTreeParser 在解析 SDI 页时返回 true
TEST(BPlusTreeParserTest, ParseSDIPage) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的 SDI 页类型
    write_le<uint16_t>(page_data.data(), 38 + 12, FIL_PAGE_SDI);
    
    // 设置堆顶位置
    write_le<uint16_t>(page_data.data(), 38 + 4, 100); // heap_top = 100
    
    // 设置有效的 INFIMUM 记录（偏移 54）
    page_data[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page_data.data(), 55, 100);
    
    // 设置有效的 SUPREMUM 记录
    page_data[99] = REC_STATUS_SUPREMUM;
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
}

// 测试 BPlusTreeParser 在解析 Undo Log 页时返回 true
TEST(BPlusTreeParserTest, ParseUndoLogPage) {
    std::vector<uint8_t> page_data(16384, 0);
    
    // 设置有效的 Undo Log 页类型
    write_le<uint16_t>(page_data.data(), 38 + 12, FIL_PAGE_UNDO_LOG);
    
    // 设置堆顶位置
    write_le<uint16_t>(page_data.data(), 38 + 4, 100); // heap_top = 100
    
    // 设置有效的 INFIMUM 记录（偏移 54）
    page_data[54] = REC_INFO_MIN_REC_FLAG;
    write_le<uint16_t>(page_data.data(), 55, 100);
    
    // 设置有效的 SUPREMUM 记录
    page_data[99] = REC_STATUS_SUPREMUM;
    
    BPlusTreeParser parser(page_data);
    EXPECT_TRUE(parser.parse());
}

// ==================== BlobPointer 单元测试 ====================
TEST(BlobPointerTest, ParseValidSinglePageBlob) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint32_t>(data.data(), 0, 0);   // space_id
    write_le<uint32_t>(data.data(), 4, 100); // page_no
    write_le<uint32_t>(data.data(), 8, 0);   // offset
    write_le<uint32_t>(data.data(), 12, 50); // len
    write_le<uint32_t>(data.data(), 16, 50); // total_len

    auto ptr = parse_blob_pointer(data);
    EXPECT_TRUE(ptr.has_value());
    EXPECT_EQ(ptr->space_id, 0);
    EXPECT_EQ(ptr->page_no, 100);
    EXPECT_EQ(ptr->len, 50);
    EXPECT_EQ(ptr->total_len, 50);
    EXPECT_EQ(ptr->chain_pages.size(), 1);
}

TEST(BlobPointerTest, ParseValidMultiPageBlob) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint32_t>(data.data(), 0, 0);   // space_id
    write_le<uint32_t>(data.data(), 4, 100); // page_no (first page)
    write_le<uint32_t>(data.data(), 8, 0);   // offset
    write_le<uint32_t>(data.data(), 12, 50); // len (current page)
    write_le<uint32_t>(data.data(), 16, 150); // total_len (multi-page)

    auto ptr = parse_blob_pointer(data);
    EXPECT_TRUE(ptr.has_value());
    EXPECT_EQ(ptr->total_len, 150);
    EXPECT_EQ(ptr->len, 50);
}

TEST(BlobPointerTest, ParseInvalidZeroTotalLenAndZeroLen) {
    std::vector<uint8_t> data(20, 0);
    write_le<uint32_t>(data.data(), 0, 0);   // space_id
    write_le<uint32_t>(data.data(), 4, 100); // page_no
    write_le<uint32_t>(data.data(), 8, 0);   // offset
    write_le<uint32_t>(data.data(), 12, 0);  // len = 0
    write_le<uint32_t>(data.data(), 16, 0);  // total_len = 0

    auto ptr = parse_blob_pointer(data);
    EXPECT_FALSE(ptr.has_value()); // 单页 BLOB 长度为 0 是无效的
}

TEST(BlobPageChainTest, DetectCircularReference) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 200);
    chain.add_page(0, 300);
    chain.add_page(0, 100); // 循环引用

    EXPECT_TRUE(chain.has_circular_reference());
}

TEST(BlobPageChainTest, NoCircularReference) {
    BlobPageChain chain;
    chain.add_page(0, 100);
    chain.add_page(0, 200);
    chain.add_page(0, 300);

    EXPECT_FALSE(chain.has_circular_reference());
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_ENABLE_TESTS

/**
 * @brief B+Tree 恢复器
 */
class BPlusTreeReconstructor {
public:
    explicit BPlusTreeReconstructor(uint32_t root_page_no);
    
    /**
     * @brief 添加索引页数据
     */
    void add_page(uint32_t page_no, std::vector<uint8_t> page_data);
    
    /**
     * @brief 重建索引结构
     */
    bool reconstruct() noexcept;
    
    /**
     * @brief 获取所有记录
     */
    [[nodiscard]] const std::vector<std::map<std::string, std::string>>& records() const {
        return records_;
    }

private:
    uint32_t root_page_no_;
    std::map<uint32_t, std::vector<uint8_t>> pages_;
    std::vector<std::map<std::string, std::string>> records_;
    
    bool traverse_tree(uint32_t page_no, const catalog::TableSchema& schema) noexcept;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BTREE_HPP
