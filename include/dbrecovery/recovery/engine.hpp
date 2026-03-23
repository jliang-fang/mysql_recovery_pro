#ifndef DBRECOVERY_RECOVERY_ENGINE_HPP
#define DBRECOVERY_RECOVERY_ENGINE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
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
     * @brief 恢复被 Truncate 的表（基于页链和 Redo Log）
     *
     * @param schema 表结构定义（用于解析记录）
     * @return std::vector<std::map<std::string, std::string>> 恢复的记录
     */
    std::vector<std::map<std::string, std::string>> recover_truncate(
        const catalog::TableSchema& schema);

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
     * @param csv_path 输出文件路径
     * @return true 成功导出
     * @return false 失败
     */
    bool export_to_csv(const std::vector<std::map<std::string, std::string>>&
records,
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
     * @brief 获取文件有效性状态
     *
     * @return true 文件有效且已成功初始化
     * @return false 文件无效或未打开
     */
    [[nodiscard]] bool is_valid() const { return file_reader_ &&
file_reader_->is_valid(); }

private:
    std::unique_ptr<innodb::InnoDBFileReader> file_reader_;
    std::string current_file_path_;
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_ENGINE_HPP
