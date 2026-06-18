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
    };

    // 扫描结果
    struct ScanResult {
        bool success = false;
        uint64_t total_pages_scanned = 0;
        uint64_t valid_pages_found = 0;
        uint64_t index_pages_found = 0;
        uint64_t sdi_pages_found = 0;
        uint64_t blob_pages_found = 0;
        double elapsed_seconds = 0.0;
        std::map<uint64_t, std::vector<std::vector<uint8_t>>> pages_by_index_id; // index_id -> pages (multiple pages)
        std::vector<std::string> errors;
    };

    // 恢复结果
    struct RecoveryResult {
        bool success = false;
        uint64_t total_records = 0;
        std::map<uint64_t, std::vector<std::map<std::string, std::string>>> records_by_index_id;
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
     * @brief 从页数据提取index_id
     * 参考: index_id在offset 66-74 (大端序)
     */
    static uint64_t extract_index_id(const uint8_t* data, size_t page_size);

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
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_RAW_DISK_CARVER_HPP
