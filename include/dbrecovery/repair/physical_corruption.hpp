#ifndef DBRECOVERY_REPAIR_PHYSICAL_CORRUPTION_HPP
#define DBRECOVERY_REPAIR_PHYSICAL_CORRUPTION_HPP

#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <memory>

namespace dbrecovery {
namespace repair {

/**
 * @brief 物理损坏类型枚举
 */
enum class PhysicalCorruptionType {
    NONE = 0,
    BIT_FLIP,           // 单比特翻转
    MULTI_BIT_FLIP,     // 多比特翻转
    PARTIAL_OVERWRITE,  // 部分覆盖
    ZEROED_REGION,      // 零化区域
    TRUNCATION,         // 文件截断
    PATTERN_OVERWRITE,  // 模式覆盖 (如 0xAA, 0x55)
    RANDOM_DATA,        // 随机数据覆盖
    REPEATED_PATTERN,   // 重复模式 (如坏扇区)
};

/**
 * @brief 物理损坏检测结果
 */
struct PhysicalCorruption {
    PhysicalCorruptionType type;
    size_t offset;              // 损坏起始偏移
    size_t length;              // 损坏长度
    uint8_t original_value;     // 原始值 (如果可以推断)
    uint8_t corrupted_value;    // 损坏后的值
    float confidence;           // 检测置信度 (0.0-1.0)
    std::string description;    // 描述信息
};

/**
 * @brief 物理损坏检测器
 *
 * 检测以下类型的物理损坏：
 * - 字节翻转 (Bit flipping)
 * - 部分覆盖
 * - 零化区域
 * - 截断
 */
class PhysicalCorruptionDetector {
public:
    PhysicalCorruptionDetector();
    ~PhysicalCorruptionDetector() = default;

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) { page_size_ = page_size; }

    /**
     * @brief 检测物理损坏
     *
     * @param data 数据缓冲区
     * @param size 数据大小
     * @return std::vector<PhysicalCorruption> 检测到的损坏列表
     */
    std::vector<PhysicalCorruption> detect(const uint8_t* data, size_t size);

    /**
     * @brief 检测单比特翻转
     */
    std::vector<PhysicalCorruption> detect_bit_flips(const uint8_t* data, size_t size);

    /**
     * @brief 检测零化区域
     */
    std::vector<PhysicalCorruption> detect_zeroed_regions(const uint8_t* data, size_t size);

    /**
     * @brief 检测部分覆盖
     */
    std::vector<PhysicalCorruption> detect_overwrites(const uint8_t* data, size_t size);

    /**
     * @brief 检测截断
     */
    std::optional<PhysicalCorruption> detect_truncation(const uint8_t* data, size_t size);

    /**
     * @brief 获取检测统计
     */
    const std::unordered_map<PhysicalCorruptionType, size_t>& get_stats() const { return stats_; }

    /**
     * @brief 清空统计
     */
    void clear_stats() { stats_.clear(); }

private:
    size_t page_size_ = 16384;
    std::unordered_map<PhysicalCorruptionType, size_t> stats_;

    // 检测阈值
    float bit_flip_threshold_ = 0.01f;      // 1% 以下的字节翻转可能是随机错误
    float zeroed_threshold_ = 0.95f;         // 95% 零字节认为是零化区域
    size_t min_zeroed_length_ = 256;         // 最小零化区域长度
};

/**
 * @brief 物理损坏修复器
 *
 * 修复物理损坏：
 * - 比特翻转修复 (使用ECC或冗余数据)
 * - 零化区域重建 (从备份或其他页推断)
 * - 截断恢复 (部分数据恢复)
 */
class PhysicalCorruptionRepairer {
public:
    PhysicalCorruptionRepairer();
    ~PhysicalCorruptionRepairer() = default;

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) { page_size_ = page_size; }

    /**
     * @brief 修复物理损坏
     *
     * @param data 数据缓冲区 (会被修改)
     * @param size 数据大小
     * @param corruptions 检测到的损坏列表
     * @return size_t 成功修复的损坏数量
     */
    size_t repair(uint8_t* data, size_t size, const std::vector<PhysicalCorruption>& corruptions);

    /**
     * @brief 修复单比特翻转
     *
     * @param page_data 页数据
     * @param corruption 损坏信息
     * @return true 修复成功
     */
    bool repair_bit_flip(uint8_t* page_data, const PhysicalCorruption& corruption);

    /**
     * @brief 修复零化区域
     *
     * 使用以下策略：
     * 1. 从同页其他部分推断
     * 2. 从相邻页推断
     * 3. 使用默认值
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @param corruption 损坏信息
     * @return true 修复成功
     */
    bool repair_zeroed_region(uint8_t* page_data, size_t page_size, const PhysicalCorruption& corruption);

    /**
     * @brief 处理截断文件
     *
     * @param file_data 文件数据
     * @param file_size 文件大小
     * @param expected_size 期望的文件大小 (可选)
     * @return std::vector<uint8_t> 恢复的数据 (可能部分)
     */
    std::vector<uint8_t> handle_truncation(const uint8_t* file_data, size_t file_size,
                                            std::optional<size_t> expected_size = std::nullopt);

    /**
     * @brief 使用ECC修复比特翻转
     *
     * @param data 数据块
     * @param size 数据大小
     * @return std::optional<std::vector<uint8_t>> 修复后的数据
     */
    std::optional<std::vector<uint8_t>> repair_with_ecc(const uint8_t* data, size_t size);

    /**
     * @brief 使用冗余页修复
     *
     * 从相邻页推断损坏页的内容
     *
     * @param corrupted_page 损坏的页
     * @param prev_page 前一个页 (可选)
     * @param next_page 后一个页 (可选)
     * @return std::optional<std::vector<uint8_t>> 修复后的页
     */
    std::optional<std::vector<uint8_t>> repair_from_neighbors(
        const uint8_t* corrupted_page,
        const uint8_t* prev_page,
        const uint8_t* next_page);

    /**
     * @brief 获取最后错误
     */
    const std::string& get_last_error() const { return last_error_; }

    /**
     * @brief 获取修复统计
     */
    size_t get_repaired_count() const { return repaired_count_; }
    size_t get_failed_count() const { return failed_count_; }

    /**
     * @brief 清空统计
     */
    void clear_stats() { repaired_count_ = 0; failed_count_ = 0; }

private:
    size_t page_size_ = 16384;
    std::string last_error_;
    size_t repaired_count_ = 0;
    size_t failed_count_ = 0;

    /**
     * @brief 尝试修复单个字节的比特翻转
     */
    std::optional<uint8_t> try_fix_byte(uint8_t corrupted_byte,
                                         const std::vector<uint8_t>& expected_values);
};

/**
 * @brief 综合物理损坏处理类
 *
 * 提供检测和修复的一站式接口
 */
class PhysicalCorruptionHandler {
public:
    PhysicalCorruptionHandler();
    ~PhysicalCorruptionHandler() = default;

    /**
     * @brief 分析并修复文件
     *
     * @param file_data 文件数据 (会被修改)
     * @param file_size 文件大小
     * @return size_t 修复的损坏数量
     */
    size_t analyze_and_repair(uint8_t* file_data, size_t file_size);

    /**
     * @brief 仅分析，不修复
     */
    std::vector<PhysicalCorruption> analyze(const uint8_t* file_data, size_t file_size);

    /**
     * @brief 获取检测器
     */
    PhysicalCorruptionDetector& get_detector() { return detector_; }

    /**
     * @brief 获取修复器
     */
    PhysicalCorruptionRepairer& get_repairer() { return repairer_; }

    /**
     * @brief 获取报告
     */
    std::string get_report() const;

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) {
        page_size_ = page_size;
        detector_.set_page_size(page_size);
        repairer_.set_page_size(page_size);
    }

private:
    size_t page_size_ = 16384;
    PhysicalCorruptionDetector detector_;
    PhysicalCorruptionRepairer repairer_;
    std::vector<PhysicalCorruption> last_corruptions_;
};

} // namespace repair
} // namespace dbrecovery

#endif // DBRECOVERY_REPAIR_PHYSICAL_CORRUPTION_HPP
