#ifndef DBRECOVERY_RECOVERY_ENGINE_HPP
#define DBRECOVERY_RECOVERY_ENGINE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <set>
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/roll_ptr_decoder.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/flashback_generator.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace recovery {

/**
 * @brief RecoveryEngine: 统一的恢复引擎入口点
 *
 * 提供以下能力：
 * - 多场景灾难恢复（Truncate, rm -rf, header corrupt）
 * - 数据导出（CSV, SQL, ibd）
 * - 页健康状态检查与修复
 * - TDE 加密数据解密支持
 */
class RecoveryEngine {
public:
    explicit RecoveryEngine();
    ~RecoveryEngine();

    // 禁止拷贝
    RecoveryEngine(const RecoveryEngine&) = delete;
    RecoveryEngine& operator=(const RecoveryEngine&) = delete;

    /**
     * @brief 打开 InnoDB 数据文件（.ibd 或 ibdata）
     *
     * @param file_path 文件路径
     * @return true 成功打开
     * @return false 失败
     */
    bool open(const std::string& file_path);

    /**
     * @brief 关闭当前打开的文件
     */
    void close();

    /**
     * @brief 设置表空间ID（用于TDE密钥派生）
     *
     * @param space_id 表空间ID
     */
    void set_space_id(uint32_t space_id);

    /**
     * @brief 设置 TDE 主密钥
     *
     * @param key_hex 十六进制格式的密钥字符串
     * @return true 成功设置
     * @return false 失败
     */
    bool set_tde_primary_key(const std::string& key_hex);

    /**
     * @brief 添加 TDE 表密钥
     *
     * @param key_id 密钥ID
     * @param key_hex 十六进制格式的密钥字符串
     * @return true 成功添加
     * @return false 失败
     */
    bool add_tde_table_key(uint32_t key_id, const std::string& key_hex);

    /**
     * @brief 从 Keyring 文件加载密钥
     *
     * @param keyring_path Keyring 文件路径
     * @return true 成功加载
     * @return false 失败
     */
    bool load_tde_keyring(const std::string& keyring_path);

    /**
     * @brief 设置是否检测加密
     *
     * @param detect 是否检测
     */
    void set_detect_encryption(bool detect);

    /**
     * @brief 设置页大小（用于覆盖自动检测）
     *
     * @param page_size 页大小（4096, 8192, 16384, 32768, 65536）
     */
    void set_page_size(uint32_t page_size);

    /**
     * @brief 恢复被 Truncate 的表（基于页链和 Redo Log）
     *
     * @param schema 表结构定义（用于解析记录）
     * @return std::vector<std::map<std::string, std::string>> 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover_truncate(
        const catalog::TableSchema& schema);

    /**
     * @brief 设置进度回调函数
     *
     * @param callback 进度回调函数（参数：当前页号，总页数）
     */
    void set_progress_callback(std::function<void(uint32_t, uint32_t)> callback) {
        progress_callback_ = std::move(callback);
    }

    /**
     * @brief 从损坏的文件头中恢复元数据
     *
     * @param output_path 输出修复后的文件路径
     * @return true 成功修复
     * @return false 失败
     */
    bool repair_header(const std::string& output_path);

    /**
     * @brief 导出为 CSV 格式
     *
     * @param records 记录数据
     * @param schema 表结构（用于确定列顺序）
     * @param csv_path 输出文件路径
     * @return true 成功导出
     * @return false 失败
     */
    bool export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                       const catalog::TableSchema& schema,
                       const std::string& csv_path);

    /**
     * @brief 导出为 SQL 格式
     *
     * @param records 记录数据
     * @param table_name 目标表名
     * @param sql_path 输出文件路径
     * @return true 成功导出
     * @return false 失败
     */
    bool export_to_sql(const std::vector<std::map<std::string, std::string>>&
records,
                       const std::string& table_name,
                       const std::string& sql_path);

    /**
     * @brief 导出为 SQL 格式（带用户和数据库名）
     *
     * @param records 记录数据
     * @param table_name 目标表名
     * @param sql_path 输出文件路径
     * @param user 数据库用户
     * @param database 数据库名
     * @return true 成功导出
     * @return false 失败
     */
    bool export_to_sql_with_user(const std::vector<std::map<std::string, std::string>>&
records,
                                  const std::string& table_name,
                                  const std::string& sql_path,
                                  const std::string& user,
                                  const std::string& database);

    /**
     * @brief 导出为 JSON 格式
     *
     * @param records 记录数据
     * @param json_path 输出文件路径
     * @return true 成功导出
     * @return false 失败
     */
    bool export_to_json(const std::vector<std::map<std::string, std::string>>& records,
                        const std::string& json_path);

    // ==================== Undo-log-assisted recovery (Task 19) ====================

    /**
     * @brief Set path to ibdata1 or undo tablespace for undo log scanning
     */
    void set_undo_data_path(const std::string& path) { undo_data_path_ = path; }

    /**
     * @brief Set MySQL version for flashback undo-format selection
     *
     * Propagated to FlashbackGenerator so its internal DeepUndoAnalyzer picks
     * the correct roll-pointer width (LEGACY_6BYTE for 5.6/5.7, MYSQL8_7BYTE
     * otherwise). Without this the analyzer defaults to UNRESOLVED and rejects
     * UPDATE/DELETE undo records.
     */
    void set_mysql_version(const std::string& version) {
        mysql_version_ = version;
        flashback_generator_.set_mysql_version(version);
    }

    /**
     * @brief Forensic inventory of legacy undo log entries (NOT verified recovery)
     *
     * Uses the legacy UndoLogParser heuristic path to enumerate TRX_UNDO_DEL_MARK
     * candidates for a table. Output records carry FORENSIC_LEGACY_UNDO provenance;
     * they have NOT been validated against roll-pointer chains, so they must not be
     * treated as verified row history or used to generate flashback SQL.
     *
     * @param table_name Target table name (used for SQL generation context)
     * @param schema Table schema for column naming
     * @return Non-authoritative candidate records, tagged FORENSIC_LEGACY_UNDO.
     */
    std::vector<std::map<std::string, std::string>> recover_from_undo_logs(
        const std::string& table_name,
        const catalog::TableSchema& schema);

    /**
     * @brief Generate DELETE flashback SQL statements from undo logs
     *
     * Scans undo logs for delete-marked records and generates reverse
     * INSERT statements to undo the deletes.
     *
     * @param table_name Target table name for SQL generation
     * @param start_time Optional start time filter (ISO format)
     * @param end_time Optional end time filter (ISO format)
     * @return Vector of SQL INSERT strings
     */
    std::vector<std::string> flashback_deletes(
        const std::string& table_name = "",
        const std::string& start_time = "",
        const std::string& end_time = "");

    /**
     * @brief Traverse undo chains and reconstruct historical row versions
     *
     * For each record's DB_ROLL_PTR, follows the undo chain backward
     * to find all previous versions of the row data.
     *
     * @param start_page_no Undo page number from DB_ROLL_PTR
     * @param start_offset Offset within undo page
     * @return Ordered undo entries (oldest first)
     */
    std::vector<innodb::UndoLogEntry> traverse_undo_chain(
        uint32_t start_page_no, uint32_t start_offset,
        innodb::UndoFormatProfile profile = innodb::UndoFormatProfile::UNRESOLVED);

    /**
     * @brief Get the UndoLogParser statistics
     */
    const innodb::UndoLogParser::UndoLogStats& get_undo_stats() const;

    /**
     * @brief 获取文件有效性状态
     *
     * @return true 文件有效且已成功初始化
     * @return false 文件无效或未打开
     */
    [[nodiscard]] bool is_valid() const { return file_reader_ &&
file_reader_->is_valid(); }

protected:
    std::unique_ptr<innodb::InnoDBFileReader> file_reader_;

private:
    std::string current_file_path_;

    // ✅ P2 新增：辅助函数
    /**
     * @brief 从 SDI 页提取表结构
     */
    std::optional<catalog::TableSchema> extract_schema_from_sdi() const;

    /**
     * @brief 启发式检测行格式
     */
    uint8_t detect_row_format() const;

    /**
     * @brief 检测页大小
     */
    uint32_t detect_page_size() const;

    /**
     * @brief 从页中提取记录（支持多种解析策略）
     */
    std::vector<std::map<std::string, std::string>> extract_records_from_page(
        const std::vector<uint8_t>& page_data,
        const catalog::TableSchema& schema,
        uint32_t page_no) const;

    /**
     * @brief 顺序扫描页数据查找记录（当 B+Tree 解析失败时）
     */
    std::vector<std::map<std::string, std::string>> extract_records_sequentially(
        const catalog::TableSchema& schema) const;

    /**
     * @brief 扫描测试文件格式的记录（基于 user_ 模式）
     */
    std::vector<std::map<std::string, std::string>> extract_records_test_format(
        const catalog::TableSchema& schema,
        const std::vector<uint8_t>& page_data,
        uint32_t page_no) const;

    /**
     * @brief 去重记录
     */
    std::vector<std::map<std::string, std::string>> deduplicate_records(
        const std::vector<std::map<std::string, std::string>>& records,
        const catalog::TableSchema& schema) const;

    /**
     * @brief TDE 解密器
     */
    innodb::TDEDecryptor tde_decryptor_;

    /**
     * @brief Undo log parser for undo-assisted recovery
     */
    mutable innodb::UndoLogParser undo_parser_;

    /**
     * @brief Deep undo analyzer for MVCC version chain & flashback recovery
     */
    mutable innodb::FlashbackGenerator flashback_generator_;

    /**
     * @brief Path to ibdata1 or undo tablespace for undo log scanning
     */
    std::string undo_data_path_;

    /**
     * @brief Tablespace ID of the currently targeted table (for undo scanning)
     */
    uint32_t space_id_ = 0;

    /**
     * @brief 是否自动检测加密
     */
    bool detect_encryption_ = false;

    /**
     * @brief Keyring 是否已加载
     */
    bool keyring_loaded_ = false;

    /**
     * @brief 页大小提示（如果非零，覆盖自动检测）
     */
    uint32_t page_size_hint_ = 0;

    /**
     * @brief MySQL 版本字符串（用于 flashback undo 格式选择）
     */
    std::string mysql_version_;
    
    /**
     * @brief 获取页大小提示
     */
    uint32_t get_page_size_hint() const { return page_size_hint_; }

    /**
     * @brief 表结构定义（用于 Undo flashback 等需要 schema 的路径）
     */
    catalog::TableSchema schema_;

    /**
     * @brief 设置表结构（用于 Undo flashback 等需要 schema 的路径）
     *
     * @param schema 表结构定义
     */
    void set_table_schema(const catalog::TableSchema& schema) { schema_ = schema; }

    /**
     * @brief 获取当前表结构
     */
    const catalog::TableSchema& get_table_schema() const { return schema_; }

    /**
     * @brief 主密钥（用于密钥派生）
     */
    std::string primary_key_hex_;

    /**
     * @brief 进度回调函数
     */
    std::function<void(uint32_t, uint32_t)> progress_callback_;
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_ENGINE_HPP
