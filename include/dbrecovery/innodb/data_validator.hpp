#ifndef DBRECOVERY_INNODB_DATA_VALIDATOR_HPP
#define DBRECOVERY_INNODB_DATA_VALIDATOR_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <set>
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== 数据验证结果 ====================
struct ValidationResult {
    bool is_valid = true;
    std::string message;
    uint64_t total_records = 0;
    uint64_t valid_records = 0;
    uint64_t invalid_records = 0;
    uint64_t total_pages = 0;
    uint64_t valid_pages = 0;
    uint64_t corrupted_pages = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    uint64_t checksum_errors = 0;
    uint64_t data_integrity_errors = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "ValidationResult{valid=" << (is_valid ? "true" : "false")
            << ", records=" << total_records
            << ", valid=" << valid_records
            << ", invalid=" << invalid_records
            << ", pages=" << total_pages
            << ", corrupted=" << corrupted_pages
            << "}";
        return oss.str();
    }
};

// ==================== 记录验证结果 ====================
struct RecordValidationResult {
    bool is_valid = true;
    uint32_t page_no = 0;
    uint16_t record_offset = 0;
    std::string error_message;
    std::vector<std::string> warnings;
};

// ==================== 页验证结果 ====================
struct PageValidationResult {
    bool is_valid = true;
    uint32_t page_no = 0;
    uint16_t page_type = 0;
    bool checksum_valid = false;
    bool header_valid = false;
    bool structure_valid = false;
    std::string error_message;
    uint32_t expected_checksum = 0;
    uint32_t actual_checksum = 0;
};

// ==================== 数据验证器 ====================
class DataValidator {
public:
    /**
     * @brief 构造函数
     */
    DataValidator();

    /**
     * @brief 析构函数
     */
    ~DataValidator();

    /**
     * @brief 设置表结构
     */
    void set_schema(const catalog::TableSchema& schema);

    /**
     * @brief 设置页大小
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief 设置是否严格模式
     */
    void set_strict_mode(bool enabled);

    /**
     * @brief 验证单个页
     */
    PageValidationResult validate_page(const std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 验证记录
     */
    RecordValidationResult validate_record(const std::vector<uint8_t>& record_data,
                                           uint32_t page_no,
                                           uint16_t record_offset,
                                           const catalog::TableSchema& schema);

    /**
     * @brief 验证页校验和
     */
    bool validate_page_checksum(const std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 验证页头
     */
    bool validate_page_header(const std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 验证页结构
     */
    bool validate_page_structure(const std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 验证记录数据
     */
    bool validate_record_data(const std::vector<uint8_t>& record_data,
                              const catalog::TableSchema& schema);

    /**
     * @brief 验证页链
     */
    bool validate_page_chain(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    /**
     * @brief 验证 B+Tree 结构
     */
    bool validate_btree_structure(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    /**
     * @brief 验证所有页
     */
    ValidationResult validate_all_pages(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages);

    /**
     * @brief 获取验证结果
     */
    const ValidationResult& get_validation_result() const;

    /**
     * @brief 获取页验证结果
     */
    const std::vector<PageValidationResult>& get_page_results() const;

    /**
     * @brief 获取记录验证结果
     */
    const std::vector<RecordValidationResult>& get_record_results() const;

    /**
     * @brief 清空数据
     */
    void clear();

    /**
     * @brief 生成验证报告
     */
    std::string generate_report() const;

private:
    catalog::TableSchema schema_;
    uint32_t page_size_ = 16384;
    bool strict_mode_ = false;

    ValidationResult validation_result_;
    std::vector<PageValidationResult> page_results_;
    std::vector<RecordValidationResult> record_results_;

    /**
     * @brief 验证 CRC32 校验和
     */
    bool validate_crc32(const std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 验证页头字段
     */
    bool validate_page_header_fields(const std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 验证页类型
     */
    bool validate_page_type(uint16_t page_type);

    /**
     * @brief 验证页链指针
     */
    bool validate_page_chain_pointers(const std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 验证记录头
     */
    bool validate_record_header(const std::vector<uint8_t>& record_data);

    /**
     * @brief 验证字段长度
     */
    bool validate_field_length(const std::vector<uint8_t>& field_data, uint32_t field_no, const catalog::ColumnInfo& col);

    /**
     * @brief 验证 NULL 位图
     */
    bool validate_null_bitmap(const std::vector<uint8_t>& record_data, const catalog::TableSchema& schema);

    /**
     * @brief 验证变长字段长度
     */
    bool validate_variable_length_fields(const std::vector<uint8_t>& record_data, const catalog::TableSchema& schema);
};

// ==================== 数据完整性检查器 ====================
class DataIntegrityChecker {
public:
    /**
     * @brief 构造函数
     */
    DataIntegrityChecker();

    /**
     * @brief 析构函数
     */
    ~DataIntegrityChecker();

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置输出目录
     */
    void set_output_directory(const std::string& dir);

    /**
     * @brief 设置页大小
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief 设置是否检查所有文件
     */
    void set_check_all(bool enabled);

    /**
     * @brief 执行完整性检查
     */
    ValidationResult check_integrity();

    /**
     * @brief 检查单个 .ibd 文件
     */
    ValidationResult check_ibd_file(const std::string& path);

    /**
     * @brief 检查页链完整性
     */
    bool check_page_chain_integrity(const std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 检查记录完整性
     */
    bool check_record_integrity(const std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 检查 BLOB 链完整性
     */
    bool check_blob_chain_integrity(const std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 获取完整性检查结果
     */
    const ValidationResult& get_integrity_result() const;

    /**
     * @brief 获取损坏的文件列表
     */
    const std::vector<std::string>& get_corrupted_files() const;

    /**
     * @brief 清空数据
     */
    void clear();

    /**
     * @brief 生成完整性报告
     */
    std::string generate_integrity_report() const;

private:
    std::string data_dir_;
    std::string output_dir_;
    uint32_t page_size_ = 16384;
    bool check_all_ = false;

    ValidationResult integrity_result_;
    std::vector<std::string> corrupted_files_;
    DataValidator validator_;

    /**
     * @brief 扫描数据目录
     */
    bool scan_data_directory();

    /**
     * @brief 检查页校验和
     */
    bool check_page_checksums(const std::string& path);

    /**
     * @brief 检查页结构
     */
    bool check_page_structure(const std::string& path);

    /**
     * @brief 检查记录链
     */
    bool check_record_chain(const std::string& path);
};

// ==================== 数据修复器 ====================
class DataRepairer {
public:
    /**
     * @brief 构造函数
     */
    DataRepairer();

    /**
     * @brief 析构函数
     */
    ~DataRepairer();

    /**
     * @brief 设置数据目录
     */
    void set_data_directory(const std::string& dir);

    /**
     * @brief 设置输出目录
     */
    void set_output_directory(const std::string& dir);

    /**
     * @brief 设置页大小
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief 修复单个页
     */
    bool repair_page(std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 修复页校验和
     */
    bool repair_page_checksum(std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 修复页头
     */
    bool repair_page_header(std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 修复记录链
     */
    bool repair_record_chain(std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 修复 B+Tree 结构
     */
    bool repair_btree_structure(std::vector<uint8_t>& page_data, uint32_t page_no);

    /**
     * @brief 修复所有页
     */
    bool repair_all_pages(const std::string& input_path, const std::string& output_path);

    /**
     * @brief 获取修复统计
     */
    struct RepairStats {
        uint32_t total_pages = 0;
        uint32_t repaired_pages = 0;
        uint32_t failed_pages = 0;
        uint32_t checksum_repaired = 0;
        uint32_t header_repaired = 0;
        uint32_t record_chain_repaired = 0;
        uint32_t btree_repaired = 0;
    };

    const RepairStats& get_repair_stats() const;

    /**
     * @brief 清空数据
     */
    void clear();

private:
    std::string data_dir_;
    std::string output_dir_;
    uint32_t page_size_ = 16384;

    RepairStats repair_stats_;

    /**
     * @brief 计算 CRC32 校验和
     */
    uint32_t calculate_crc32(const std::vector<uint8_t>& data);

    /**
     * @brief 计算页校验和
     */
    uint32_t calculate_page_checksum(const std::vector<uint8_t>& page_data);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_DATA_VALIDATOR_HPP
