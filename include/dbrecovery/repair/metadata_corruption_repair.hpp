#ifndef DBRECOVERY_REPAIR_METADATA_CORRUPTION_REPAIR_HPP
#define DBRECOVERY_REPAIR_METADATA_CORRUPTION_REPAIR_HPP

#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <set>

namespace dbrecovery {
namespace repair {

/**
 * @brief 元数据损坏类型
 */
enum class MetadataCorruptionType {
    NONE = 0,
    FSP_HEADER_CORRUPTED,       // FSP 头损坏
    FSP_SPACE_FLAGS_INVALID,    // FSP space flags 无效
    SPACE_ID_MISMATCH,          // 表空间 ID 不匹配
    INODE_PAGE_CORRUPTED,       // INODE 页损坏
    SDI_PAGE_CORRUPTED,         // SDI 页损坏
    SDI_BLOB_CORRUPTED,         // SDI BLOB 数据损坏
    SDI_DECOMPRESS_FAILED,      // SDI 解压失败
    PAGE_0_INVALID,             // Page 0 (FSP header) 无效
    PAGE_1_INVALID,             // Page 1 (IBUF bitmap) 无效
};

/**
 * @brief 元数据损坏详情
 */
struct MetadataCorruption {
    MetadataCorruptionType type;
    uint32_t page_no;               // 涉及的页号
    size_t offset;                  // 损坏偏移
    std::string description;
    float confidence;
    bool repairable;
};

/**
 * @brief FSP (File Space) 头信息
 */
struct FspHeaderInfo {
    uint32_t space_id;              // 表空间 ID
    uint32_t space_flags;           // 空间标志
    uint64_t size;                  // 文件大小 (页数)
    uint64_t free_limit;            // 空闲限制
    uint32_t free_len;              // 空闲链长度
    uint32_t frag_n_used;           // 已用碎片页数
    uint64_t lsn;                   // LSN
    bool is_valid;
};

/**
 * @brief INODE 页信息
 */
struct InodePageInfo {
    uint32_t page_no;
    std::vector<uint32_t> segment_ids;  // 段 ID 列表
    std::vector<uint32_t> root_pages;   // 根页列表
    bool is_valid;
};

/**
 * @brief SDI 页信息
 */
struct SdiPageInfo {
    uint32_t page_no;
    uint32_t uncompressed_len;      // 解压后长度
    uint32_t compressed_len;        // 压缩后长度
    uint8_t compression_type;       // 压缩类型
    bool is_valid;
};

/**
 * @brief 元数据修复器
 *
 * 检测和修复元数据损坏：
 * - FSP space flags 损坏
 * - INODE 页损坏
 * - SDI BLOB 损坏
 * - Tablespace ID 不匹配
 * - Space header page 完整重建
 */
class MetadataCorruptionRepairer {
public:
    MetadataCorruptionRepairer();
    ~MetadataCorruptionRepairer() = default;

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) { page_size_ = page_size; }

    /**
     * @brief 检测元数据损坏
     */
    std::vector<MetadataCorruption> detect_corruptions(const uint8_t* data, size_t size);

    /**
     * @brief 修复元数据损坏
     */
    size_t repair(uint8_t* data, size_t size, const std::vector<MetadataCorruption>& corruptions);

    /**
     * @brief 解析 FSP 头 (Page 0)
     */
    std::optional<FspHeaderInfo> parse_fsp_header(const uint8_t* data, size_t size);

    /**
     * @brief 修复 FSP 头
     */
    bool repair_fsp_header(uint8_t* data, size_t size);

    /**
     * @brief 修复 FSP space flags
     */
    bool repair_space_flags(uint8_t* data, size_t size, uint32_t correct_flags);

    /**
     * @brief 检测并修复表空间 ID 不匹配
     */
    bool repair_space_id_mismatch(uint8_t* data, size_t size, uint32_t correct_space_id);

    /**
     * @brief 解析 INODE 页 (Page 2)
     */
    std::optional<InodePageInfo> parse_inode_page(const uint8_t* data, size_t size);

    /**
     * @brief 修复 INODE 页
     */
    bool repair_inode_page(uint8_t* data, size_t size);

    /**
     * @brief 从扫描结果重建 INODE
     */
    bool rebuild_inode_from_scan(uint8_t* data, size_t size, const std::vector<uint32_t>& root_pages);

    /**
     * @brief 解析 SDI 页
     */
    std::optional<SdiPageInfo> parse_sdi_page(const uint8_t* data, size_t size, uint32_t page_no);

    /**
     * @brief 修复 SDI 页
     */
    bool repair_sdi_page(uint8_t* data, size_t size, uint32_t page_no);

    /**
     * @brief 解压 SDI BLOB 数据
     */
    std::optional<std::vector<uint8_t>> decompress_sdi_blob(const uint8_t* compressed_data, size_t compressed_len, size_t uncompressed_len);

    /**
     * @brief 重建 Page 0 (FSP header)
     */
    bool rebuild_page_zero(uint8_t* data, size_t size);

    /**
     * @brief 从其他页推断表空间 ID
     */
    std::optional<uint32_t> infer_space_id(const uint8_t* data, size_t size);

    /**
     * @brief 验证元数据完整性
     */
    bool validate_metadata(const uint8_t* data, size_t size);

    /**
     * @brief 获取最后错误
     */
    const std::string& get_last_error() const { return last_error_; }

    /**
     * @brief 获取修复统计
     */
    size_t get_repaired_count() const { return repaired_count_; }

private:
    size_t page_size_ = 16384;
    std::string last_error_;
    size_t repaired_count_ = 0;

    /**
     * @brief 获取所有页的 space_id
     */
    std::map<uint32_t, uint32_t> get_all_space_ids(const uint8_t* data, size_t size) const;
};

/**
 * @brief SDI 数据恢复器
 *
 * 专门处理 SDI (Serialized Dictionary Information) 损坏
 */
class SdiDataRepairer {
public:
    SdiDataRepairer();
    ~SdiDataRepairer() = default;

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) { page_size_ = page_size; }

    /**
     * @brief 从 SDI 页提取 JSON 数据
     */
    std::optional<std::string> extract_sdi_json(const uint8_t* data, size_t size);

    /**
     * @brief 修复损坏的 SDI 数据
     */
    std::optional<std::vector<uint8_t>> repair_sdi_data(const uint8_t* data, size_t size);

    /**
     * @brief 从页数据推断表结构
     */
    std::optional<std::string> infer_schema_from_pages(const uint8_t* data, size_t size);

    /**
     * @brief 获取最后错误
     */
    const std::string& get_last_error() const { return last_error_; }

private:
    size_t page_size_ = 16384;
    std::string last_error_;
};

/**
 * @brief 元数据处理器 (检测+修复一站式)
 */
class MetadataCorruptionHandler {
public:
    MetadataCorruptionHandler() = default;

    /**
     * @brief 分析并修复元数据
     */
    size_t analyze_and_repair(uint8_t* data, size_t size);

    /**
     * @brief 仅分析
     */
    std::vector<MetadataCorruption> analyze(const uint8_t* data, size_t size);

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) {
        page_size_ = page_size;
        metadata_repairer_.set_page_size(page_size);
        sdi_repairer_.set_page_size(page_size);
    }

    /**
     * @brief 获取报告
     */
    std::string get_report() const;

private:
    size_t page_size_ = 16384;
    MetadataCorruptionRepairer metadata_repairer_;
    SdiDataRepairer sdi_repairer_;
    std::vector<MetadataCorruption> last_corruptions_;
};

} // namespace repair
} // namespace dbrecovery

#endif // DBRECOVERY_REPAIR_METADATA_CORRUPTION_REPAIR_HPP
