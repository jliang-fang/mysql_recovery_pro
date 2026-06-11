#pragma once

#include <string>
#include <vector>
#include <map>

// Include schema_defs.hpp first to avoid namespace pollution
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/recovery/blob_reconstructor.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/forensic_manager.hpp"

// schema_defs.hpp 和 blob_reconstructor.hpp 已经正确关闭了 dbrecovery 命名空间

// schema_defs.hpp 和 blob_reconstructor.hpp 已经正确关闭了 dbrecovery 命名空间
// 此处重新打开 dbrecovery 和 innodb 命名空间
namespace dbrecovery {
namespace innodb {

/**
 * @brief RecoveryEngine: 核心数据恢复引擎
 *
 * 提供以下能力：
 * - 从 ibdata/ibd 文件中解析页结构
 * - 应用 redo log 进行前滚恢复
 * - 使用 undo log 进行回滚
 * - 重建 BLOB 数据
 */
class RecoveryEngine {
public:
    explicit RecoveryEngine();
    ~RecoveryEngine();

    // 禁止拷贝
    RecoveryEngine(const RecoveryEngine&) = delete;
    RecoveryEngine& operator=(const RecoveryEngine&) = delete;

    /**
     * @brief 从检查点恢复整个实例（用于 ibdata 恢复）
     *
     * @param ibdata_path ibdata 文件路径
     * @param output_dir 输出目录
     * @return true 成功
     * @return false 失败
     */
    bool recover_from_checkpoint(const std::string& ibdata_path, const std::string& output_dir);

    /**
     * @brief 执行深度取证恢复（用于 TRUNCATE 或 rm -rf 场景）
     *
     * @param target_path 目标文件或设备路径
     * @param output_csv 输出 CSV 文件路径
     * @return true 成功
     * @return false 失败
     */
    bool deep_recover(const std::string& target_path, const std::string& output_csv);

    /**
     * @brief 从单个 .ibd 文件恢复表数据
     *
     * @param ibd_path .ibd 文件路径
     * @param table_id 表 ID（用于元数据查找）
     * @return true 成功
     * @return false 失败
     */
    bool recover_table(const std::string& ibd_path, uint64_t table_id);

    /**
     * @brief 获取解析后的 redo 记录列表
     *
     * @return const std::vector<RedoRecord>& redo 记录引用
     */
    // const std::vector<RedoRecord>& get_redo_records() const;

    /**
     * @brief 获取文件有效性状态
     *
     * @return true 文件有效且已成功初始化
     * @return false 文件无效或未打开
     */
    [[nodiscard]] bool is_valid() const { return is_initialized_; }

    /**
     * @brief 获取恢复的记录列表
     *
     * @return const std::vector<std::map<std::string, std::string>>& 恢复的记录
     */
    [[nodiscard]] const std::vector<std::map<std::string, std::string>>& recovered_records() const {
        return recovered_records_;
    }

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
    bool export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                       const std::string& table_name,
                       const std::string& sql_path);

private:
    // ✅ 新增：关键成员变量
    std::unique_ptr<recovery::BlobReconstructor> blob_reconstructor_;

    // 页缓存，用于存储已读取的页数据
    std::map<uint32_t, std::vector<uint8_t>> page_cache_;

    // 表结构缓存，用于存储从 SDI 解析的元数据
    catalog::TableSchema table_schema_;

    // 恢复状态标志
    bool is_initialized_ = false;

    // 恢复的记录列表
    std::vector<std::map<std::string, std::string>> recovered_records_;
};

} // namespace innodb
} // namespace dbrecovery
