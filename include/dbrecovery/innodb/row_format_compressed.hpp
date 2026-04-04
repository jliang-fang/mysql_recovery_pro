#ifndef DBRECOVERY_INNODB_ROW_FORMAT_COMPRESSED_HPP
#define DBRECOVERY_INNODB_ROW_FORMAT_COMPRESSED_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <zlib.h>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== COMPRESSED 行格式常量 ====================
constexpr uint16_t ZSD_SIZE = 32;  // 压缩页头大小（ZSD = Zero Space Descriptor）
constexpr uint16_t COMPRESSED_PAGE_HEADER_SIZE = 38 + 32;  // FIL header + ZSD

// 压缩页头字段偏移
constexpr uint16_t ZSD_ZIP_SIZE = 0;      // 压缩页大小（2字节）
constexpr uint16_t ZSD_VERSION = 2;       // 压缩版本（2字节）
constexpr uint16_t ZSD_ZHDR_OFFSET = 4;   // 压缩页头偏移（4字节）
constexpr uint16_t ZSD_ZLEN = 8;          // 压缩长度（2字节）
constexpr uint16_t ZSD_CHECKSUM = 10;     // 校验和（4字节）
constexpr uint16_t ZSD_ORIG_SIZE = 14;    // 原始大小（4字节）
constexpr uint16_t ZSD_TYPE = 18;         // 类型（2字节）
constexpr uint16_t ZSD_CHECKSUM2 = 20;    // 校验和2（4字节）
constexpr uint16_t ZSD_UNUSED = 24;       // 未使用（8字节）

// ==================== 压缩页结构 ====================
struct CompressedPageHeader {
    uint16_t zip_size;        // 压缩页大小（以 512 字节为单位）
    uint16_t version;         // 压缩版本
    uint32_t zhdr_offset;     // 压缩页头偏移
    uint16_t zlen;            // 压缩长度
    uint32_t checksum;        // 校验和
    uint32_t orig_size;       // 原始大小
    uint16_t type;            // 类型
    uint32_t checksum2;       // 校验和2
    std::vector<uint8_t> unused;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "CompressedPageHeader{zip_size=" << zip_size
            << ", version=" << version
            << ", orig_size=" << orig_size
            << ", zlen=" << zlen << "}";
        return oss.str();
    }
};

// ==================== 压缩记录头 ====================
struct CompressedRecordHeader {
    uint8_t info_bits;
    uint16_t next_record_offset;
    uint16_t heap_no;
    
    // COMPRESSED 特有字段
    uint16_t field_count;     // 字段数量
    uint16_t var_count;       // 可变字段数量
    uint16_t null_count;      // NULL 字段数量
    
    bool is_deleted() const { return (info_bits & REC_INFO_DELETED_FLAG) != 0; }
    bool is_min_rec() const { return (info_bits & REC_INFO_MIN_REC_FLAG) != 0; }

    std::string to_string() const {
        std::ostringstream oss;
        oss << "CompressedRecordHeader{next=" << next_record_offset
            << ", heap_no=" << heap_no
            << ", deleted=" << std::boolalpha << is_deleted() << std::noboolalpha
            << ", fields=" << field_count << "}";
        return oss.str();
    }
};

// ==================== 压缩页解析器 ====================
class CompressedPageParser {
public:
    /**
     * @brief 构造函数
     * @param page_data 页数据
     * @param page_size 页大小
     */
    CompressedPageParser(const std::vector<uint8_t>& page_data, size_t page_size = 16384);

    /**
     * @brief 解析压缩页
     * @return true 解析成功
     */
    bool parse();

    /**
     * @brief 解压缩页数据
     * @return 解压缩后的数据
     */
    std::vector<uint8_t> decompress();

    /**
     * @brief 获取压缩页头
     */
    const CompressedPageHeader& header() const { return compressed_header_; }

    /**
     * @brief 获取解压缩后的页数据
     */
    const std::vector<uint8_t>& decompressed_data() const { return decompressed_data_; }

    /**
     * @brief 获取页类型
     */
    uint16_t page_type() const { return page_type_; }

    /**
     * @brief 获取页号
     */
    uint32_t page_no() const { return page_no_; }

    /**
     * @brief 获取行格式
     */
    uint8_t row_format() const { return row_format_; }

    /**
     * @brief 检查页是否有效
     */
    bool is_valid() const { return valid_; }

    /**
     * @brief 获取错误信息
     */
    const std::string& error_message() const { return error_message_; }

    /**
     * @brief 解析记录
     * @param schema 表结构
     * @return 解析后的记录
     */
    std::vector<std::map<std::string, std::string>> parse_records(
        const catalog::TableSchema& schema) const;

private:
    std::vector<uint8_t> page_data_;
    size_t page_size_;
    CompressedPageHeader compressed_header_;
    std::vector<uint8_t> decompressed_data_;
    uint16_t page_type_;
    uint32_t page_no_;
    uint8_t row_format_;
    bool valid_;
    std::string error_message_;

    // 解析压缩页头
    bool parse_compressed_header();

    // 解压缩数据
    bool decompress_data();

    // 解析记录
    bool parse_records_impl(const catalog::TableSchema& schema,
                           std::vector<std::map<std::string, std::string>>& records) const;
};

// ==================== 压缩页解析器实现 ====================

inline CompressedPageParser::CompressedPageParser(const std::vector<uint8_t>& page_data, size_t page_size)
    : page_data_(page_data), page_size_(page_size), valid_(false) {
    // 验证页大小
    if (page_size_ != 4096 && page_size_ != 8192 &&
        page_size_ != 16384 && page_size_ != 32768 && page_size_ != 65536) {
        page_size_ = 16384;
    }
}

inline bool CompressedPageParser::parse() {
    // 清理之前的状态
    valid_ = false;
    error_message_.clear();
    decompressed_data_.clear();

    // 检查页大小
    if (page_data_.size() < 38) {
        error_message_ = "Page too small";
        return false;
    }

    // 读取页类型
    page_type_ = read_le<uint16_t>(page_data_.data(), 16);
    
    // 检查是否为压缩页
    if (page_type_ != FIL_PAGE_COMPRESSED) {
        error_message_ = "Not a compressed page";
        return false;
    }

    // 读取页号
    page_no_ = read_le<uint32_t>(page_data_.data(), 20);

    // 解析压缩页头
    if (!parse_compressed_header()) {
        return false;
    }

    // 解压缩数据
    if (!decompress_data()) {
        return false;
    }

    valid_ = true;
    return true;
}

inline bool CompressedPageParser::parse_compressed_header() {
    if (page_data_.size() < COMPRESSED_PAGE_HEADER_SIZE) {
        error_message_ = "Page too small for compressed header";
        return false;
    }

    // 读取压缩页头
    compressed_header_.zip_size = read_le<uint16_t>(page_data_.data(), ZSD_ZIP_SIZE);
    compressed_header_.version = read_le<uint16_t>(page_data_.data(), ZSD_VERSION);
    compressed_header_.zhdr_offset = read_le<uint32_t>(page_data_.data(), ZSD_ZHDR_OFFSET);
    compressed_header_.zlen = read_le<uint16_t>(page_data_.data(), ZSD_ZLEN);
    compressed_header_.checksum = read_le<uint32_t>(page_data_.data(), ZSD_CHECKSUM);
    compressed_header_.orig_size = read_le<uint32_t>(page_data_.data(), ZSD_ORIG_SIZE);
    compressed_header_.type = read_le<uint16_t>(page_data_.data(), ZSD_TYPE);
    compressed_header_.checksum2 = read_le<uint32_t>(page_data_.data(), ZSD_CHECKSUM2);

    // 读取未使用字段
    if (page_data_.size() >= ZSD_UNUSED + 8) {
        compressed_header_.unused.assign(page_data_.data() + ZSD_UNUSED, 
                                         page_data_.data() + ZSD_UNUSED + 8);
    }

    // 验证压缩页头
    if (compressed_header_.zip_size == 0 || compressed_header_.zip_size > 16) {
        error_message_ = "Invalid zip size";
        return false;
    }

    if (compressed_header_.orig_size == 0) {
        error_message_ = "Invalid original size";
        return false;
    }

    return true;
}

inline bool CompressedPageParser::decompress_data() {
    if (page_data_.size() < COMPRESSED_PAGE_HEADER_SIZE) {
        error_message_ = "Page too small for decompression";
        return false;
    }

    // 计算压缩数据的起始位置
    size_t compressed_start = COMPRESSED_PAGE_HEADER_SIZE;
    size_t compressed_size = page_data_.size() - compressed_start;

    if (compressed_size == 0) {
        error_message_ = "No compressed data";
        return false;
    }

    // 解压缩
    uLongf dest_len = compressed_header_.orig_size;
    decompressed_data_.resize(dest_len);

    int ret = uncompress(decompressed_data_.data(), &dest_len,
                        page_data_.data() + compressed_start, 
                        static_cast<uLong>(compressed_size));

    if (ret != Z_OK) {
        error_message_ = "Decompression failed: " + std::to_string(ret);
        return false;
    }

    decompressed_data_.resize(dest_len);
    return true;
}

inline std::vector<std::map<std::string, std::string>> CompressedPageParser::parse_records(
    const catalog::TableSchema& schema) const {
    
    std::vector<std::map<std::string, std::string>> records;

    if (!valid_ || decompressed_data_.empty()) {
        return records;
    }

    // 使用 BPlusTreeParser 解析解压缩后的数据
    BPlusTreeParser parser(decompressed_data_);
    if (!parser.parse()) {
        error_message_ = "Failed to parse decompressed page";
        return records;
    }

    return parser.parse_records(schema);
}

// ==================== 压缩页构建器 ====================
class CompressedPageBuilder {
public:
    /**
     * @brief 构造函数
     * @param page_size 页大小
     * @param zip_size 压缩页大小（以 512 字节为单位）
     */
    CompressedPageBuilder(size_t page_size = 16384, uint16_t zip_size = 16);

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 添加记录
     */
    void add_record(const std::map<std::string, std::string>& record);

    /**
     * @brief 构建压缩页
     * @return 压缩页数据
     */
    std::vector<uint8_t> build();

    /**
     * @brief 设置压缩级别
     */
    void set_compression_level(int level) { compression_level_ = level; }

private:
    size_t page_size_;
    uint16_t zip_size_;
    catalog::TableSchema schema_;
    std::vector<std::map<std::string, std::string>> records_;
    int compression_level_ = Z_DEFAULT_COMPRESSION;

    // 构建未压缩的页
    std::vector<uint8_t> build_uncompressed_page();

    // 压缩页数据
    std::vector<uint8_t> compress_page(const std::vector<uint8_t>& data);
};

// ==================== 压缩页构建器实现 ====================

inline CompressedPageBuilder::CompressedPageBuilder(size_t page_size, uint16_t zip_size)
    : page_size_(page_size), zip_size_(zip_size) {
    // 验证页大小
    if (page_size_ != 4096 && page_size_ != 8192 &&
        page_size_ != 16384 && page_size_ != 32768 && page_size_ != 65536) {
        page_size_ = 16384;
    }
}

inline void CompressedPageBuilder::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

inline void CompressedPageBuilder::add_record(const std::map<std::string, std::string>& record) {
    records_.push_back(record);
}

inline std::vector<uint8_t> CompressedPageBuilder::build() {
    // 构建未压缩的页
    std::vector<uint8_t> uncompressed = build_uncompressed_page();

    // 压缩页数据
    return compress_page(uncompressed);
}

inline std::vector<uint8_t> CompressedPageBuilder::build_uncompressed_page() {
    std::vector<uint8_t> page(page_size_, 0);

    // 填充 FIL Header
    write_le<uint32_t>(page.data(), 0, 0xFFFFFFFF);  // prev_page
    write_le<uint32_t>(page.data(), 4, 0xFFFFFFFF);  // next_page
    write_le<uint64_t>(page.data(), 8, 1000);        // LSN
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_INDEX);  // page_type
    write_le<uint32_t>(page.data(), 20, 0);          // page_no
    write_le<uint32_t>(page.data(), 24, 0);          // space_id

    // 填充 Page Header
    write_le<uint16_t>(page.data(), 38, 10);         // n_dir_slots
    write_le<uint16_t>(page.data(), 40, 100);        // heap_top
    write_le<uint16_t>(page.data(), 42, 2);          // n_heap (INFIMUM + SUPREMUM)
    write_le<uint16_t>(page.data(), 44, 0);          // free_offset

    // 填充 INFIMUM 记录
    page[54] = REC_STATUS_INFIMUM;                   // info_bits
    write_le<uint16_t>(page.data(), 55, 99);         // next_record_offset

    // 填充 SUPREMUM 记录
    page[99] = REC_STATUS_SUPREMUM;                  // info_bits
    write_le<uint16_t>(page.data(), 100, 0x3FFF);    // next_record_offset

    return page;
}

inline std::vector<uint8_t> CompressedPageBuilder::compress_page(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return {};
    }

    // 计算压缩后的大小
    uLongf dest_len = compressBound(static_cast<uLong>(data.size()));
    std::vector<uint8_t> compressed(dest_len);

    // 压缩数据
    int ret = compress2(compressed.data(), &dest_len,
                       data.data(), static_cast<uLong>(data.size()),
                       compression_level_);

    if (ret != Z_OK) {
        return {};
    }

    compressed.resize(dest_len);

    // 构建压缩页
    std::vector<uint8_t> page(page_size_, 0);

    // 填充 FIL Header
    write_le<uint32_t>(page.data(), 0, 0xFFFFFFFF);  // prev_page
    write_le<uint32_t>(page.data(), 4, 0xFFFFFFFF);  // next_page
    write_le<uint64_t>(page.data(), 8, 1000);        // LSN
    write_le<uint16_t>(page.data(), 16, FIL_PAGE_COMPRESSED);  // page_type
    write_le<uint32_t>(page.data(), 20, 0);          // page_no
    write_le<uint32_t>(page.data(), 24, 0);          // space_id

    // 填充压缩页头
    write_le<uint16_t>(page.data(), COMPRESSED_PAGE_HEADER_SIZE + ZSD_ZIP_SIZE, zip_size_);
    write_le<uint16_t>(page.data(), COMPRESSED_PAGE_HEADER_SIZE + ZSD_VERSION, 1);  // version
    write_le<uint32_t>(page.data(), COMPRESSED_PAGE_HEADER_SIZE + ZSD_ZHDR_OFFSET, 0);
    write_le<uint16_t>(page.data(), COMPRESSED_PAGE_HEADER_SIZE + ZSD_ZLEN, 
                      static_cast<uint16_t>(compressed.size()));
    write_le<uint32_t>(page.data(), COMPRESSED_PAGE_HEADER_SIZE + ZSD_ORIG_SIZE, 
                      static_cast<uint32_t>(data.size()));
    write_le<uint16_t>(page.data(), COMPRESSED_PAGE_HEADER_SIZE + ZSD_TYPE, 0);

    // 复制压缩数据
    size_t compressed_offset = COMPRESSED_PAGE_HEADER_SIZE + ZSD_SIZE;
    if (compressed_offset + compressed.size() <= page_size_) {
        std::memcpy(page.data() + compressed_offset, compressed.data(), compressed.size());
    }

    return page;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_ROW_FORMAT_COMPRESSED_HPP
