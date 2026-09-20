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
#include "dbrecovery/innodb/page_type_inferrer.hpp"
#include "dbrecovery/carve/page_reassembler.hpp"
#include "dbrecovery/safety/safety_gate.hpp"

namespace dbrecovery {
namespace recovery {

// Re-export from carve namespace for convenience
using ReassemblyStats = dbrecovery::carve::ReassemblyStats;

/**
 * @brief RawDiskCarver - PhotoRec风格的原始磁盘数据雕刻
 *
 * 实现无备份情况下的磁盘扫描恢复:
 * 1. 多进程并行扫描原始磁盘设备
 * 2. 按InnoDB页签名检测有效数据页
 * 3. 按index_id分组孤立页
 * 4. Schema-less启发式记录恢复
 * 5. 支持MySQL 5.6/5.7/8.0/8.4/9.0页格式
 */
class RawDiskCarver {
public:
    // 配置参数
    struct Config {
        std::string device_path;      // 原始设备路径 (如 /dev/sda, /dev/nvme0n1)
        std::string output_dir;       // 输出目录
        uint32_t page_size = 16384;   // InnoDB页大小 (默认16KB)
        uint32_t chunk_size = 256 * 1024 * 1024; // 每次读取块大小 (256MB，优化大磁盘)
        uint32_t num_workers = 8;     // 工作进程数（默认8，支持更多并行）
        uint64_t offset_start = 0;    // 扫描起始偏移
        uint64_t offset_stop = UINT64_MAX; // 扫描结束偏移
        uint32_t scan_step = 4096;    // 扫描步长（默认4KB，优化性能）
        uint32_t fine_scan_step = 512; // 精细扫描步长（用于非对齐扫描，默认512字节）
        bool scan_all_index = true;   // 扫描所有INDEX页
        bool recover_deleted = true;  // 恢复已删除记录
        bool verbose = false;         // 详细输出
        bool auto_detect_page_size = true; // 自动检测页大小
        bool scan_frm_files = true;   // 扫描FRM文件签名
        bool scan_sdi_pages = true;   // 扫描SDI页
        bool use_page_aligned_scan = true; // 使用页对齐扫描（提升性能）
        bool scan_data_patterns = true; // 扫描数据模式（用于删除文件恢复）
        uint32_t progress_interval_ms = 1000; // 进度报告间隔（毫秒）
        // TRUNCATE恢复支持: 指定要恢复的index_id列表
        std::vector<uint64_t> target_index_ids;  // 只恢复指定的index_id
        std::vector<uint64_t> exclude_index_ids; // 排除已知的系统表index_id
        bool exclude_system_tables = false;      // 排除系统表index_id (0-100 and >= 0xFFFF0000)
        // 数据模式匹配（用于删除文件恢复）
        std::vector<std::string> data_patterns;  // 要搜索的数据模式

        // PageTypeInferrer — enhanced page detection
        bool use_page_type_inferrer = false;
        innodb::PageInferrerConfig page_inferrer_config;
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
     * @brief Validate a raw source before any scan or read is attempted.
     *
     * Regular files and directories are accepted. Block devices require all
     * mandatory SafetyGate facts; other special files are rejected.
     */
    static bool admit_source(const std::string& source_path, std::string* reason = nullptr);

    /**
     * @brief 设置配置
     */
    void set_config(const Config& config);

    /**
     * @brief Transfer an already-authorized raw-device capability to this carver.
     */
    void adopt_authorized_raw_device(safety::AuthorizedRawDevice&& device);

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
     * @brief 重新组装扫描到的页到逻辑表空间文件 (Deep Carve v2.0)
     *
     * 将裸盘扫描中发现的分散 InnoDB 页按照 SPACE_ID 分组、PAGE_NO 排序、
     * 去重，并合成为逻辑上完整的 .ibd 文件。
     *
     * @param scan_result 扫描结果 (来自 scan() 调用)
     * @param output_dir 输出目录 (存放合成的 .ibd 文件)
     * @param progress 进度回调
     * @return 重组统计信息
     */
    ReassemblyStats reassemble_to_tablespaces(
        const ScanResult& scan_result,
        const std::string& output_dir,
        std::function<void(double, const std::string&)> progress = nullptr);

    /**
     * @brief 获取上次扫描结果
     */
    const ScanResult& get_last_scan_result() const;

    /**
     * @brief 设置外部schema (用于MySQL 5.6/5.7等无SDI的场景)
     */
    void set_external_schema(const catalog::TableSchema& schema);

private:
    Config config_;
    ScanResult last_scan_result_;
    std::optional<catalog::TableSchema> external_schema_;  // 外部提供的schema
    uint64_t actual_scan_stop_ = 0;  // 实际扫描结束位置
    std::optional<safety::AuthorizedRawDevice> authorized_raw_device_;

    // 进程间共享的原子变量
    std::atomic<uint64_t> current_offset_{0};
    std::atomic<uint64_t> pages_scanned_{0};
    std::atomic<uint64_t> pages_found_{0};
    std::atomic<bool> should_stop_{false};

    // 线程安全的页收集
    std::mutex pages_mutex_;
    std::map<uint64_t, std::vector<std::vector<uint8_t>>> collected_pages_;

    // PageTypeInferrer — enhanced page detection engine
    mutable innodb::PageTypeInferrer page_type_inferrer_{innodb::PageInferrerConfig{}};

    // Static utility methods — made public for testing
public:
    /**
     * @brief 检测是否是有效的InnoDB页 (LSN验证)
     */
    static bool is_valid_page(const uint8_t* data, size_t page_size);

    /**
     * @brief 严格页验证
     * 检查data[:4]+data[4:8]是否等于data[-8:-4]+data[-4:]
     * 这是一种更严格的页检测方法
     */
    static bool is_valid_page_strict(const uint8_t* data, size_t page_size);

    /**
     * @brief 检测是否是INDEX页
     */
    static bool is_index_page(const uint8_t* data, size_t page_size);

    /**
     * @brief 检测是否是SDI页
     * @param data 页数据
     * @param page_size 页大小
     * @param page_no 页号（FIL header 中读取）。FIL_PAGE_SDI_ZIP (0x45BD) 同时被
     *        压缩 SDI 页和 BLOB 页使用，需用页号位置和 SDI 签名区分。
     */
    static bool is_sdi_page(const uint8_t* data, size_t page_size, uint32_t page_no);

    /**
     * @brief 检测是否是BLOB页
     * @param data 页数据
     * @param page_size 页大小
     * @param page_no 页号（FIL header 中读取）。FIL_PAGE_SDI_ZIP (0x45BD) 同时被
     *        压缩 SDI 页和 BLOB 页使用，需用页号位置和 SDI 签名区分。
     */
    static bool is_blob_page(const uint8_t* data, size_t page_size, uint32_t page_no);

    /**
     * @brief 检测FRM文件签名
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
     * @brief 使用Schema进行schema-aware记录解析
     */
    std::vector<std::map<std::string, std::string>> parse_records_with_schema(
        const std::vector<uint8_t>& page_data,
        const catalog::TableSchema& schema);

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
     * @brief 从JSON对象解析表结构 (内部辅助函数)
     */
    static catalog::TableSchema parse_sdi_json_to_schema(const nlohmann::json& json_obj);

    /**
     * @brief 从FRM数据提取表结构 (MySQL 5.6/5.7)
     */
    std::optional<catalog::TableSchema> extract_schema_from_frm(
        const uint8_t* data, size_t len);
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_RAW_DISK_CARVER_HPP
