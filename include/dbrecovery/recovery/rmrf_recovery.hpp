#ifndef DBRECOVERY_RECOVERY_RMRF_RECOVERY_HPP
#define DBRECOVERY_RECOVERY_RMRF_RECOVERY_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace recovery {

// ✅ P1-3 新增：扫描的文件信息结构
struct ScannedFile {
    std::string file_path;
    uint64_t file_size;
    uint32_t total_pages;
    uint32_t valid_index_pages;
    std::map<uint16_t, uint32_t> page_type_count;
};

// ✅ P1-3 新增：扫描的设备信息结构
struct ScannedDevice {
    std::string device_path;
    uint64_t device_size;
    uint32_t total_pages;
    uint32_t valid_pages;
    std::map<uint16_t, uint32_t> page_type_count;
};

// ==================== PhotoRec 风格双层签名扫描 ====================

/**
 * @brief PhotoRec 风格双层签名扫描配置
 */
struct TwoLevelScanConfig {
    uint64_t start_offset = 0;          // 起始偏移
    uint64_t end_offset = 0;           // 结束偏移 (0 = 扫描到文件末尾)
    uint32_t page_size = 16384;         // 页大小
    std::vector<uint16_t> allowed_page_types;  // 允许的页类型 (空 = 所有)
    bool validate_checksum = true;      // 验证校验和
    bool strict_checksum = false;       // 严格模式：拒绝校验和不通过的页
};

/**
 * @brief PhotoRec 风格双层签名扫描结果
 */
struct TwoLevelScanResult {
    uint64_t file_offset;               // 文件偏移
    uint32_t page_number;              // 页号
    uint32_t space_id;                // 表空间 ID
    uint16_t page_type;                // 页类型
    uint64_t lsn;                     // 日志序列号
    uint32_t page_size;                // 页大小
    std::vector<uint8_t> data;         // 页数据
};

// ==================== FORCE 模式扫描 ====================

/**
 * @brief FORCE 模式扫描配置
 */
struct ForceScanConfig {
    uint32_t page_size = 16384;       // 页大小
    uint32_t expected_index_id = 0;   // 期望的 INDEX_ID (0 = 任意)
    bool verify_checksum = true;       // 验证校验和
    bool strict_mode = false;          // 严格模式：拒绝校验和不通过的页
};

/**
 * @brief FORCE 模式扫描结果
 */
struct ForceScanResult {
    uint64_t file_offset;              // 文件偏移
    uint32_t page_number;             // 页号
    uint32_t space_id;                // 表空间 ID
    uint16_t page_type;              // 页类型
    uint64_t lsn;                    // 日志序列号
    uint32_t index_id;               // 索引 ID
    bool is_leaf_page;                // 是否为叶子页
    bool checksum_valid;              // 校验和是否有效
    std::vector<uint8_t> data;        // 页数据
};

// ==================== 页版本信息 ====================

/**
 * @brief 页版本信息（用于 TRUNCATE 恢复）
 */
struct PageVersionInfo {
    uint64_t disk_offset;              // 磁盘偏移
    uint32_t page_number;             // 页号
    uint32_t space_id;                // 表空间 ID
    uint64_t lsn;                     // 日志序列号
    uint16_t page_type;               // 页类型
    std::vector<uint8_t> data;        // 页数据
};

/**
 * @brief RmrfRecovery: rm -rf 灾难恢复器
 *
 * 提供以下能力：
 * - 目录扫描：查找所有 .ibd 文件
 * - 原始设备扫描：直接扫描磁盘/分区
 * - 页识别：识别 InnoDB 页类型
 * - 记录恢复：从碎片化页中恢复记录
 * - PhotoRec 风格双层签名扫描
 * - FORCE 模式扫描
 * - TRUNCATE 恢复（基于 LSN 过滤）
 */
class RmrfRecovery {
public:
    explicit RmrfRecovery();
    ~RmrfRecovery();

    // 禁止拷贝
    RmrfRecovery(const RmrfRecovery&) = delete;
    RmrfRecovery& operator=(const RmrfRecovery&) = delete;

    /**
     * @brief 设置页大小
     *
     * @param page_size 页大小（4096/8192/16384/32768/65536）
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief 扫描目录查找 .ibd 文件
     *
     * @param dir_path 目录路径
     * @return true 找到文件
     * @return false 未找到文件
     */
    bool scan_directory(const std::string& dir_path);

    /**
     * @brief 扫描原始设备（磁盘/分区）
     *
     * @param device_path 设备路径（如 /dev/sda1）
     * @return true 找到 InnoDB 数据
     * @return false 未找到
     */
    bool scan_raw_device(const std::string& device_path);

    /**
     * @brief 从单个页中恢复记录
     *
     * @param page_data 页数据
     * @param schema 表结构
     * @return std::vector<std::map<std::string, std::string>> 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover_records(
        const std::vector<uint8_t>& page_data,
        const catalog::TableSchema& schema);

    /**
     * @brief 从多个页中重建记录
     *
     * @param pages 页列表（page_no, page_data）
     * @param schema 表结构
     * @param recovered_records 输出：恢复的记录
     * @return true 成功
     * @return false 失败
     */
    bool reconstruct_from_pages(
        const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages,
        const catalog::TableSchema& schema,
        std::vector<std::map<std::string, std::string>>& recovered_records);

    /**
     * @brief 从文件中重建记录
     *
     * @param file_path 文件路径
     * @param schema 表结构
     * @param recovered_records 输出：恢复的记录
     * @return true 成功
     * @return false 失败
     */
    bool reconstruct_from_file(
        const std::string& file_path,
        const catalog::TableSchema& schema,
        std::vector<std::map<std::string, std::string>>& recovered_records);

    /**
     * @brief 获取扫描到的页列表
     *
     * @return const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& 页列表
     */
    [[nodiscard]] const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>&
    scanned_pages() const { return scanned_pages_; }

    /**
     * @brief 获取扫描到的 SDI 页
     *
     * @return const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& SDI 页列表
     */
    [[nodiscard]] const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>&
    sdi_pages() const { return sdi_pages_; }

    // ✅ P1-3 新增：获取总索引页数
    [[nodiscard]] uint32_t get_total_index_pages() const;

    // ✅ P1-3 新增：获取扫描的文件列表
    [[nodiscard]] const std::vector<ScannedFile>& get_scanned_files() const;

    // ✅ P1-3 新增：获取扫描的设备列表
    [[nodiscard]] const std::vector<ScannedDevice>& get_scanned_devices() const;

    /**
     * @brief 基于启发式扫描提取记录（当 B+Tree 解析失败时）
     *
     * @param page_data 页数据
     * @param schema 表结构
     * @return std::vector<std::map<std::string, std::string>> 提取的记录
     */
    std::vector<std::map<std::string, std::string>> extract_records_heuristic(
        const std::vector<uint8_t>& page_data,
        const catalog::TableSchema& schema);

    /**
     * @brief 全分区扫描恢复
     *
     * 遍历整个 IBD 文件的每个页：
     * - 识别页类型（INDEX, SDI, UNDO, ALLOCATED）
     * - 对 INDEX 页执行 B+Tree 解析
     * - 对损坏页使用启发式恢复
     * - 从所有页中提取记录并去重
     *
     * @param file_path IBD 文件路径
     * @return 恢复的记录列表
     */
    std::vector<std::map<std::string, std::string>> full_partition_recovery(
        const std::string& file_path);

    /**
     * @brief 从扫描的完整分区中恢复记录
     *
     * 这个方法使用 ScanStat 信息来优化恢复过程：
     * - 根据页类型分布选择最佳恢复策略
     * - 优先使用 B+Tree 解析
     * - 在 B+Tree 解析失败时回退到启发式恢复
     *
     * @param file_path IBD 文件路径
     * @param schema 表结构（可选，用于优化解析）
     * @param recovered_records 输出：恢复的记录
     * @return true 成功
     * @return false 失败
     */
    bool full_partition_recovery_with_schema(
        const std::string& file_path,
        const catalog::TableSchema& schema,
        std::vector<std::map<std::string, std::string>>& recovered_records);

    /**
     * @brief 从磁盘/镜像文件中恢复 TRUNCATE 的表
     *
     * 使用已知表结构作为模板，扫描磁盘寻找残留的 InnoDB 数据页
     *
     * @param disk_path 磁盘/镜像文件路径
     * @param schema 表结构（TRUNCATE 前的结构）
     * @param recovered_records 输出：恢复的记录
     * @param page_size 页大小（默认 16384）
     * @param min_pages 最小页数（小于这个值被认为不是有效文件）
     * @return true 成功
     * @return false 失败
     */
    bool recover_truncated_table_from_disk(
        const std::string& disk_path,
        const catalog::TableSchema& schema,
        std::vector<std::map<std::string, std::string>>& recovered_records,
        uint32_t page_size = 16384,
        uint32_t min_pages = 2);

    /**
     * @brief 从磁盘中恢复 rm -f 删除的 .ibd/.frm 文件
     *
     * 使用 PhotoRec 风格的文件雕刻，从磁盘原始数据中恢复删除的文件
     * 这是 testdisk/photorec 风格的恢复方法
     *
     * @param disk_path 磁盘/镜像文件路径
     * @param output_dir 输出目录（恢复的文件将保存到这里）
     * @param found_files 输出：找到的 .ibd 文件信息
     * @param min_pages 最小页数
     * @return true 成功找到文件
     * @return false 失败
     */
    bool recover_deleted_ibd_files(
        const std::string& disk_path,
        const std::string& output_dir,
        std::vector<std::string>& found_files,
        uint32_t min_pages = 2);

    /**
     * @brief 从扫描的页中恢复记录（支持多种恢复模式）
     *
     * @param pages 页列表
     * @param schema 表结构
     * @param mode 恢复模式：0=自动, 1=强制B+Tree, 2=仅启发式
     * @param recovered_records 输出：恢复的记录
     * @return true 成功
     * @return false 失败
     */
    bool reconstruct_from_pages_with_mode(
        const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages,
        const catalog::TableSchema& schema,
        int mode,
        std::vector<std::map<std::string, std::string>>& recovered_records);

    // ==================== PhotoRec 风格双层签名扫描 ====================

    /**
     * @brief 使用 PhotoRec 风格的双层签名索引扫描文件
     *
     * Level 1: 按 offset 索引签名
     * Level 2: 在每个 offset 处按首字节值分桶（256个桶）
     *
     * @param file_path 文件路径
     * @param config 扫描配置
     * @param callback 每找到一个页时调用的回调
     * @return 找到的页数
     */
    uint32_t scan_with_two_level_index(
        const std::string& file_path,
        const TwoLevelScanConfig& config,
        std::function<void(const TwoLevelScanResult&)> callback);

    // ==================== FORCE 模式扫描 ====================

    /**
     * @brief 使用 FORCE 模式扫描文件
     *
     * FORCE 模式遍历文件的每一页，不依赖页链：
     * 1. 检查 offset 24 的签名
     * 2. 验证 PAGE_INDEX_ID
     * 3. 验证叶子页标识 (data[64:66] == 0)
     * 4. 提取有效数据
     *
     * @param file_path 文件路径
     * @param config FORCE 扫描配置
     * @param callback 每找到一个页时调用的回调
     * @return 找到的页数
     */
    uint32_t scan_with_force_mode(
        const std::string& file_path,
        const ForceScanConfig& config,
        std::function<void(const ForceScanResult&)> callback);

    // ==================== TRUNCATE 恢复 ====================

    /**
     * @brief TRUNCATE 恢复：基于 LSN 过滤的页版本选择
     *
     * TRUNCATE 操作会保留数据页但更新元数据。本函数通过 LSN 过滤找到 TRUNCATE 前的数据版本。
     *
     * @param pages 所有找到的页版本
     * @param schema 表结构
     * @param max_lsn TRUNCATE 前的最大 LSN
     * @param callback 每恢复一条记录时调用的回调
     * @return 恢复的记录数
     */
    uint32_t recover_truncate_by_lsn(
        const std::vector<PageVersionInfo>& pages,
        const catalog::TableSchema& schema,
        uint64_t max_lsn,
        std::function<void(const std::map<std::string, std::string>&)> callback);

private:
    // 内部辅助函数
    static bool validate_page_checksum(const uint8_t* page, uint32_t page_size, bool strict);
    static uint32_t compute_crc32c(const uint8_t* data, size_t len);
    static uint32_t compute_innodb_checksum(const uint8_t* data, size_t len);

    uint32_t page_size_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> scanned_pages_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> sdi_pages_;

    // ✅ P1-3 新增：存储扫描的文件和设备信息
    std::vector<ScannedFile> scanned_files_;
    std::vector<ScannedDevice> scanned_devices_;
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_RMRF_RECOVERY_HPP
