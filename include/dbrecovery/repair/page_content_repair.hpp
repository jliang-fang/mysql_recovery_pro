#ifndef DBRECOVERY_REPAIR_PAGE_CONTENT_REPAIR_HPP
#define DBRECOVERY_REPAIR_PAGE_CONTENT_REPAIR_HPP

#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <memory>

namespace dbrecovery {
namespace repair {

/**
 * @brief 页内容损坏类型
 */
enum class PageContentCorruptionType {
    NONE = 0,
    PAGE_DIRECTORY_CORRUPTED,      // 页目录槽损坏
    INFIMUM_CORRUPTED,             // INFIMUM 记录损坏
    SUPREMUM_CORRUPTED,            // SUPREMUM 记录损坏
    RECORD_HEADER_CORRUPTED,       // 记录头损坏
    RECORD_DATA_CORRUPTED,         // 记录数据损坏
    FREE_CHAIN_CORRUPTED,          // 已删除记录链损坏
    HEAP_TOP_INVALID,              // heap_top 值无效
    N_HEAP_INVALID,                // n_heap 值无效
    N_DIR_SLOTS_INVALID,           // n_dir_slots 值无效
};

/**
 * @brief 页内容损坏详情
 */
struct PageContentCorruption {
    PageContentCorruptionType type;
    uint16_t offset;               // 页内偏移
    uint16_t length;               // 损坏长度
    std::string description;       // 描述
    float confidence;              // 置信度
    bool repairable;               // 是否可修复
};

/**
 * @brief 页目录槽信息
 */
struct PageDirectorySlot {
    uint16_t slot_offset;          // 槽在页目录中的偏移
    uint16_t record_offset;        // 指向的记录偏移
    uint8_t n_owned;               // 该记录拥有的记录数
    bool is_supremum;              // 是否是 SUPREMUM (最小槽)
};

/**
 * @brief INFIMUM/SUPREMUM 记录信息
 */
struct SystemRecordInfo {
    uint16_t offset;               // 记录偏移
    uint16_t next_offset;          // 下一条记录的相对偏移
    uint8_t rec_type;              // 记录类型
    bool valid;                    // 是否有效
};

/**
 * @brief 页内容修复器
 *
 * 修复页内部结构损坏：
 * - 页目录槽损坏
 * - INFIMUM/SUPREMUM 记录重建
 * - 已删除记录链恢复 (PAGE_FREE)
 * - heap_top/n_heap/n_dir_slots 修复
 */
class PageContentRepairer {
public:
    PageContentRepairer();
    ~PageContentRepairer() = default;

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) { page_size_ = page_size; }

    /**
     * @brief 检测页内容损坏
     */
    std::vector<PageContentCorruption> detect_corruptions(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 修复页内容损坏
     */
    size_t repair(uint8_t* page_data, size_t page_size,
                  const std::vector<PageContentCorruption>& corruptions);

    /**
     * @brief 重建页目录
     *
     * 从现有的记录重新构建页目录槽
     */
    bool rebuild_page_directory(uint8_t* page_data, size_t page_size);

    /**
     * @brief 重建 INFIMUM 记录
     *
     * INFIMUM 是页中的最小记录，位于 offset 54
     */
    bool rebuild_infimum(uint8_t* page_data, size_t page_size, uint16_t first_user_record_offset);

    /**
     * @brief 重建 SUPREMUM 记录
     *
     * SUPREMUM 是页中的最大记录，位于 heap_top - 8 左右
     */
    bool rebuild_supremum(uint8_t* page_data, size_t page_size);

    /**
     * @brief 恢复已删除记录
     *
     * 从 PAGE_FREE 链中恢复被删除的记录
     */
    std::vector<uint16_t> recover_deleted_records(uint8_t* page_data, size_t page_size);

    /**
     * @brief 修复 heap_top
     */
    bool fix_heap_top(uint8_t* page_data, size_t page_size);

    /**
     * @brief 修复 n_heap
     */
    bool fix_n_heap(uint8_t* page_data, size_t page_size, uint16_t actual_record_count);

    /**
     * @brief 修复 n_dir_slots
     */
    bool fix_n_dir_slots(uint8_t* page_data, size_t page_size);

    /**
     * @brief 验证页结构完整性
     */
    bool validate_page_structure(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 获取最后错误
     */
    const std::string& get_last_error() const { return last_error_; }

    /**
     * @brief 获取修复统计
     */
    size_t get_repaired_count() const { return repaired_count_; }

    /**
     * @brief 清空统计
     */
    void clear_stats() { repaired_count_ = 0; }

private:
    size_t page_size_ = 16384;
    std::string last_error_;
    size_t repaired_count_ = 0;

    /**
     * @brief 获取页目录槽数量
     */
    uint16_t get_n_dir_slots(const uint8_t* page_data) const;

    /**
     * @brief 获取 heap_top
     */
    uint16_t get_heap_top(const uint8_t* page_data) const;

    /**
     * @brief 获取 n_heap
     */
    uint16_t get_n_heap(const uint8_t* page_data) const;

    /**
     * @brief 获取 PAGE_FREE 链头
     */
    uint16_t get_free_list_head(const uint8_t* page_data) const;

    /**
     * @brief 从页目录读取槽
     */
    std::optional<PageDirectorySlot> read_dir_slot(const uint8_t* page_data, uint16_t slot_idx) const;

    /**
     * @brief 写入页目录槽
     */
    void write_dir_slot(uint8_t* page_data, uint16_t slot_idx, uint16_t record_offset);

    /**
     * @brief 扫描用户记录
     */
    std::vector<uint16_t> scan_user_records(const uint8_t* page_data, size_t page_size);
};

/**
 * @brief 页内容损坏处理器 (检测+修复一站式)
 */
class PageContentHandler {
public:
    PageContentHandler() = default;

    /**
     * @brief 分析并修复页内容
     */
    size_t analyze_and_repair(uint8_t* page_data, size_t page_size);

    /**
     * @brief 仅分析
     */
    std::vector<PageContentCorruption> analyze(const uint8_t* page_data, size_t page_size);

    /**
     * @brief 获取修复器
     */
    PageContentRepairer& get_repairer() { return repairer_; }

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) {
        page_size_ = page_size;
        repairer_.set_page_size(page_size);
    }

    /**
     * @brief 获取报告
     */
    std::string get_report() const;

private:
    size_t page_size_ = 16384;
    PageContentRepairer repairer_;
    std::vector<PageContentCorruption> last_corruptions_;
};

/**
 * @brief 批量页内容修复器
 *
 * 对整个文件的所有页进行内容修复
 */
class BatchPageContentRepairer {
public:
    BatchPageContentRepairer();

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) { page_size_ = page_size; }

    /**
     * @brief 修复文件中的所有页内容
     */
    size_t repair_all_pages(uint8_t* file_data, size_t file_size);

    /**
     * @brief 分析文件中的所有页内容
     */
    std::vector<std::pair<uint32_t, PageContentCorruption>> analyze_all_pages(
        const uint8_t* file_data, size_t file_size);

    /**
     * @brief 获取修复报告
     */
    std::string get_report() const;

    /**
     * @brief 获取统计
     */
    struct BatchStats {
        size_t total_pages;
        size_t pages_with_corruption;
        size_t pages_repaired;
        size_t corruptions_detected;
        size_t corruptions_repaired;
    };
    BatchStats get_stats() const { return stats_; }

private:
    size_t page_size_ = 16384;
    PageContentRepairer page_repairer_;
    BatchStats stats_;
};

} // namespace repair
} // namespace dbrecovery

#endif // DBRECOVERY_REPAIR_PAGE_CONTENT_REPAIR_HPP