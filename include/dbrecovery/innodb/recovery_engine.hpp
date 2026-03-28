#pragma once

#include <string>
#include <vector>
#include <map>

// Include schema_defs.hpp first to avoid namespace pollution
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/recovery/blob_reconstructor.hpp"

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

private:
    // ✅ 新增：关键成员变量
    std::unique_ptr<recovery::BlobReconstructor> blob_reconstructor_;

    // 页缓存，用于存储已读取的页数据
    std::map<uint32_t, std::vector<uint8_t>> page_cache_;

    // 表结构缓存，用于存储从 SDI 解析的元数据
    catalog::TableSchema table_schema_;

    // 恢复状态标志
    bool is_initialized_ = false;
};

} // namespace innodb
} // namespace dbrecovery
