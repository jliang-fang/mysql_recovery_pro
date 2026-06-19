#ifndef DBRECOVERY_RECOVERY_RAW_DISK_CARVER_HPP
#define DBRECOVERY_RECOVERY_RAW_DISK_CARVER_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <fstream>

#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace recovery {

/**
 * @brief RawDiskCarver - PhotoRec风格的原始磁盘数据雕刻
 *
 * 实现无备份情况下的磁盘扫描恢复:
 * 1. 多进程并行扫描原始磁盘设备
 * 2. 按InnoDB页签名检测有效数据页
 * 3. 按index_id分组孤立页
 * 4. Schema-less启发式记录恢复
 * 5. 支持MySQL 5.6/5.7/8.0/8.4/9.0页格式
 *
 * 参考: ibd2sql SCAN_DEV_WORKER实现
 */
class RawDiskCarver {
public:
    // 配置参数
    struct Config {
        std::string device_path;      // 原始设备路径 (如 /dev/sda, /dev/nvme0n1)
        std::string output_dir;       // 输出目录
        uint32_t page_size = 16384;   // InnoDB页大小 (默认16KB)
        uint32_t chunk_size = 64 * 1024 * 1024; // 每次读取块大小 (64MB)
        uint32_t num_workers = 4;     // 工作进程数
        uint64_t offset_start = 0;    // 扫描起始偏移
        uint64_t offset_stop = UINT64_MAX; // 扫描结束偏移
        bool scan_all_index = true;   // 扫描所有INDEX页
        bool recover_deleted = true;  // 恢复已删除记录
        bool verbose = false;         // 详细输出
        bool auto_detect_page_size = true; // 自动检测页大小
        bool scan_frm_files = true;   // 扫描FRM文件签名
        bool scan_sdi_pages = true;   // 扫描SDI页
        // TRUNCATE恢复支持: 指定要恢复的index_id列表
        std::vector<uint64_t> target_index_ids;  // 只恢复指定的index_id
        std::vector<uint64_t> exclude_index_ids; // 排除已知的系统表index_id
    };

    // 页类型枚举
    enum class PageType : uint8_t {
        UNKNOWN = 0,
        INDEX = 1,       // FIL_PAGE_INDEX (0x45BF)
        SDI = 2,         // FIL_PAGE_SDI (0x45BE)
        SDI_ZIP = 3,     // FIL_PAGE_SDI_ZIP (0x45BD)
        UNDO_LOG = 4,    // FIL_PAGE_UNDO_LOG (0x45B2)
        BLOB = 5,        // BLOB页
        FRM = 6,         // FRM文件签名
        ALLOCATED = 7    // FIL_PAGE_TYPE_ALLOCATED (0x0000)
    };

    // 扫描到的页信息
    struct FoundPage {
        PageType type;
        uint64_t offset;
        uint64_t index_id;
        uint32_t space_id;
        uint32_t page_no;
        std::vector<uint8_t> data;
    };

    // 扫描结果
    struct ScanResult {
        bool success = false;
        uint64_t total_pages_scanned = 0;
        uint64_t valid_pages_found = 0;
        uint64_t index_pages_found = 0;
        uint64_t sdi_pages_found = 0;
        uint64_t blob_pages_found = 0;
        uint64_t frm_files_found = 0;
        double elapsed_seconds = 0.0;
        std::map<uint64_t, std::vector<std::vector<uint8_t>>> pages_by_index_id; // index_id -> pages
        std::vector<FoundPage> all_found_pages; // 所有找到的页
        std::map<uint32_t, std::vector<uint8_t>> sdi_pages; // space_id -> SDI data
        std::vector<std::string> frm_files; // FRM文件数据
        std::vector<std::string> errors;
    };

    // 恢复结果
    struct RecoveryResult {
        bool success = false;
        uint64_t total_records = 0;
        std::map<uint64_t, std::vector<std::map<std::string, std::string>>> records_by_index_id;
        std::map<uint64_t, catalog::TableSchema> inferred_schemas; // index_id -> inferred schema
        std::vector<std::string> output_files;
        std::vector<std::string> errors;
    };

    RawDiskCarver();
    ~RawDiskCarver();

    /**
     * @brief 设置配置
     */
    void set_config(const Config& config);

    /**
     * @brief 执行磁盘扫描
     * @param progress 进度回调 (0.0-1.0, message)
     * @return 扫描结果
     */
    ScanResult scan(std::function<void(double, const std::string&)> progress = nullptr);

    /**
     * @brief 从扫描结果恢复数据
     * @param scan_result 扫描结果
     * @param progress 进度回调
     * @return 恢复结果
     */
    RecoveryResult recover(const ScanResult& scan_result,
                          std::function<void(double, const std::string&)> progress = nullptr);

    /**
     * @brief 扫描并恢复 (一体化流程)
     */
    RecoveryResult scan_and_recover(std::function<void(double, const std::string&)> progress = nullptr);

    /**
     * @brief 获取上次扫描结果
     */
    const ScanResult& get_last_scan_result() const;

private:
    Config config_;
    ScanResult last_scan_result_;
    uint64_t actual_scan_stop_ = 0;  // 实际扫描结束位置

    // 进程间共享的原子变量
    std::atomic<uint64_t> current_offset_{0};
    std::atomic<uint64_t> pages_scanned_{0};
    std::atomic<uint64_t> pages_found_{0};
    std::atomic<bool> should_stop_{false};

    // 线程安全的页收集
    std::mutex pages_mutex_;
    std::map<uint64_t, std::vector<std::vector<uint8_t>>> collected_pages_;

    /**
     * @brief 检测是否是有效的InnoDB页 (LSN验证)
     * 参考ibd2sql IS_PAGE: data[:4] == data[-8:-4]
     */
    static bool is_valid_page(const uint8_t* data, size_t page_size);

    /**
     * @brief 检测是否是INDEX页
     * 参考ibd2sql IS_INDEX: data[24:26] == b'E\xbf' (即0x45BF)
     */
    static bool is_index_page(const uint8_t* data, size_t page_size);

    /**
     * @brief 检测是否是SDI页
     */
    static bool is_sdi_page(const uint8_t* data, size_t page_size);

    /**
     * @brief 检测是否是BLOB页
     */
    static bool is_blob_page(const uint8_t* data, size_t page_size);

    /**
     * @brief 检测FRM文件签名
     * 参考ibd2sql IS_FRM: data[:10] == b'\xfe\x01\n\x0c\x03\x00\x00\x10\x01\x00'
     */
    static bool is_frm_signature(const uint8_t* data, size_t len);

    /**
     * @brief 从页数据提取index_id
     * 参考: index_id在offset 66-74 (大端序)
     */
    static uint64_t extract_index_id(const uint8_t* data, size_t page_size);

    /**
     * @brief 从页数据提取space_id
     */
    static uint32_t extract_space_id(const uint8_t* data, size_t page_size);

    /**
     * @brief 从页数据提取page_no
     */
    static uint32_t extract_page_no(const uint8_t* data, size_t page_size);

    /**
     * @brief 自动检测页大小
     * 支持4KB/8KB/16KB/32KB/64KB
     */
    static uint32_t detect_page_size(const uint8_t* data, size_t max_len);

    /**
     * @brief 工作线程函数
     */
    void worker_thread(uint32_t worker_id,
                       std::function<void(double, const std::string&)> progress);

    /**
     * @brief 无Schema启发式记录解析
     */
    std::vector<std::map<std::string, std::string>> parse_records_schemaless(
        const std::vector<uint8_t>& page_data);

    /**
     * @brief 启发式字段边界检测
     */
    std::vector<std::pair<size_t, size_t>> detect_field_boundaries(
        const uint8_t* record_start, size_t max_len);

    /**
     * @brief 从SDI页提取表结构 (MySQL 8.0+)
     */
    std::optional<catalog::TableSchema> extract_schema_from_sdi(
        const std::vector<uint8_t>& page_data);

    /**
     * @brief 从FRM数据提取表结构 (MySQL 5.6/5.7)
     */
    std::optional<catalog::TableSchema> extract_schema_from_frm(
        const uint8_t* data, size_t len);
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_RAW_DISK_CARVER_HPP
