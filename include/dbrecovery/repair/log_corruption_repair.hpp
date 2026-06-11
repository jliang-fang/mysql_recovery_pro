#ifndef DBRECOVERY_REPAIR_LOG_CORRUPTION_REPAIR_HPP
#define DBRECOVERY_REPAIR_LOG_CORRUPTION_REPAIR_HPP

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
 * @brief 日志损坏类型
 */
enum class LogCorruptionType {
    NONE = 0,
    REDO_HEADER_CORRUPTED,      // Redo log 文件头损坏
    REDO_BLOCK_HEADER_CORRUPTED, // Redo log 块头损坏
    REDO_CHECKPOINT_CORRUPTED,   // Checkpoint 损坏
    REDO_LSN_INVALID,            // LSN 无效
    REDO_RECORD_TRUNCATED,       // 记录被截断
    REDO_CHECKSUM_MISMATCH,      // 校验和不匹配
    UNDO_HEADER_CORRUPTED,       // Undo log 头损坏
    UNDO_SEGMENT_CORRUPTED,      // Undo segment 损坏
    UNDO_PAGE_CORRUPTED,         // Undo page 损坏
};

/**
 * @brief 日志损坏详情
 */
struct LogCorruption {
    LogCorruptionType type;
    size_t offset;                  // 损坏偏移
    size_t length;                  // 损坏长度
    std::string description;
    float confidence;
    bool repairable;
};

/**
 * @brief Redo Log 块头信息
 */
struct RedoBlockHeader {
    uint64_t lsn;                   // LSN
    uint16_t data_len;              // 数据长度
    uint16_t first_rec_offset;      // 第一条记录偏移
    uint32_t checkpoint_no;         // Checkpoint 编号
    bool is_valid;
};

/**
 * @brief Redo Log 文件头信息
 */
struct RedoFileHeader {
    uint32_t log_group_id;          // 日志组 ID
    uint64_t lsn;                   // 起始 LSN
    uint64_t lsn_capacity;          // LSN 容量
    uint32_t file_size;             // 文件大小
    uint32_t block_size;            // 块大小
    bool is_valid;
};

/**
 * @brief Checkpoint 信息
 */
struct CheckpointInfo {
    uint64_t lsn;                   // Checkpoint LSN
    uint64_t lsn_offset;            // LSN 在文件中的偏移
    uint32_t checkpoint_no;         // Checkpoint 编号
    uint32_t arch_log_no;           // 归档日志编号
    bool is_valid;
};

/**
 * @brief Redo Log 修复器
 *
 * 检测和修复 Redo Log 损坏：
 * - Redo log block header 损坏
 * - Redo log file header 损坏
 * - LSN checkpoint 损坏
 */
class RedoLogRepairer {
public:
    RedoLogRepairer();
    ~RedoLogRepairer() = default;

    /**
     * @brief 设置块大小 (默认 512 字节)
     */
    void set_block_size(size_t block_size) { block_size_ = block_size; }

    /**
     * @brief 检测 Redo Log 损坏
     */
    std::vector<LogCorruption> detect_corruptions(const uint8_t* data, size_t size);

    /**
     * @brief 修复 Redo Log 损坏
     */
    size_t repair(uint8_t* data, size_t size, const std::vector<LogCorruption>& corruptions);

    /**
     * @brief 解析 Redo 文件头
     */
    std::optional<RedoFileHeader> parse_file_header(const uint8_t* data, size_t size);

    /**
     * @brief 解析 Redo 块头
     */
    std::optional<RedoBlockHeader> parse_block_header(const uint8_t* data, size_t size, size_t block_idx);

    /**
     * @brief 解析 Checkpoint 信息
     */
    std::optional<CheckpointInfo> parse_checkpoint(const uint8_t* data, size_t size);

    /**
     * @brief 修复 Redo 文件头
     */
    bool repair_file_header(uint8_t* data, size_t size);

    /**
     * @brief 修复 Redo 块头
     */
    bool repair_block_header(uint8_t* data, size_t size, size_t block_idx);

    /**
     * @brief 修复 Checkpoint
     */
    bool repair_checkpoint(uint8_t* data, size_t size, const CheckpointInfo& checkpoint);

    /**
     * @brief 重建 LSN 链
     */
    bool rebuild_lsn_chain(uint8_t* data, size_t size);

    /**
     * @brief 跳过损坏的块
     */
    std::vector<size_t> find_valid_blocks(const uint8_t* data, size_t size);

    /**
     * @brief 获取最后错误
     */
    const std::string& get_last_error() const { return last_error_; }

    /**
     * @brief 获取修复统计
     */
    size_t get_repaired_count() const { return repaired_count_; }

private:
    size_t block_size_ = 512;  // Redo log 块大小
    std::string last_error_;
    size_t repaired_count_ = 0;

    /**
     * @brief 计算 Redo 块校验和
     */
    uint32_t calculate_block_checksum(const uint8_t* block_data);
};

/**
 * @brief Undo Log 修复器
 *
 * 检测和修复 Undo Log 损坏：
 * - Undo log segment header 损坏
 * - Undo log page 损坏
 */
class UndoLogRepairer {
public:
    UndoLogRepairer();
    ~UndoLogRepairer() = default;

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size) { page_size_ = page_size; }

    /**
     * @brief 检测 Undo Log 损坏
     */
    std::vector<LogCorruption> detect_corruptions(const uint8_t* data, size_t size);

    /**
     * @brief 修复 Undo Log 损坏
     */
    size_t repair(uint8_t* data, size_t size, const std::vector<LogCorruption>& corruptions);

    /**
     * @brief 解析 Undo Segment 头
     */
    std::optional<uint32_t> parse_segment_header(const uint8_t* data, size_t size);

    /**
     * @brief 修复 Undo Segment 头
     */
    bool repair_segment_header(uint8_t* data, size_t size);

    /**
     * @brief 修复 Undo Page
     */
    bool repair_undo_page(uint8_t* page_data, size_t page_size);

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
};

/**
 * @brief 事务日志处理器 (检测+修复一站式)
 */
class LogCorruptionHandler {
public:
    LogCorruptionHandler() = default;

    /**
     * @brief 分析并修复 Redo Log
     */
    size_t analyze_and_repair_redo(uint8_t* data, size_t size);

    /**
     * @brief 分析并修复 Undo Log
     */
    size_t analyze_and_repair_undo(uint8_t* data, size_t size);

    /**
     * @brief 仅分析 Redo Log
     */
    std::vector<LogCorruption> analyze_redo(const uint8_t* data, size_t size);

    /**
     * @brief 仅分析 Undo Log
     */
    std::vector<LogCorruption> analyze_undo(const uint8_t* data, size_t size);

    /**
     * @brief 设置块大小 (Redo)
     */
    void set_block_size(size_t block_size) {
        block_size_ = block_size;
        redo_repairer_.set_block_size(block_size);
    }

    /**
     * @brief 设置页大小 (Undo)
     */
    void set_page_size(size_t page_size) {
        page_size_ = page_size;
        undo_repairer_.set_page_size(page_size);
    }

    /**
     * @brief 获取报告
     */
    std::string get_report() const;

private:
    size_t block_size_ = 512;
    size_t page_size_ = 16384;
    RedoLogRepairer redo_repairer_;
    UndoLogRepairer undo_repairer_;
    std::vector<LogCorruption> last_corruptions_;
};

} // namespace repair
} // namespace dbrecovery

#endif // DBRECOVERY_REPAIR_LOG_CORRUPTION_REPAIR_HPP
