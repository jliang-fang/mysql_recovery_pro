#pragma once

#include <cstdint>
#include <vector>
#include <map>
#include <string>
#include "dbrecovery/innodb/redo_log.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief RedoLogReplayer: Redo Log 前滚恢复器
 *
 * 提供以下能力：
 * - Redo Log 记录回放
 * - 数据页更新
 * - 页校验和更新
 * - 事务一致性保证
 */
class RedoLogReplayer {
public:
    explicit RedoLogReplayer();
    ~RedoLogReplayer();

    /**
     * @brief 设置页数据
     *
     * @param page_no 页号
     * @param page_data 页数据
     */
    void set_page(uint32_t page_no, std::vector<uint8_t> page_data);

    /**
     * @brief 获取页数据
     *
     * @param page_no 页号
     * @return std::vector<uint8_t> 页数据
     */
    std::vector<uint8_t> get_page(uint32_t page_no) const;

    /**
     * @brief 应用 Redo Log 记录
     *
     * @param record Redo Log 记录
     * @return true 成功
     * @return false 失败
     */
    bool apply_record(const RedoLogRecord& record);

    /**
     * @brief 应用所有 Redo Log 记录
     *
     * @param records Redo Log 记录列表
     * @return size_t 成功应用的记录数
     */
    size_t apply_records(const std::vector<RedoLogRecord>& records);

    /**
     * @brief 更新页校验和
     *
     * @param page_no 页号
     */
    void update_page_checksum(uint32_t page_no);

    /**
     * @brief 获取应用的记录数
     *
     * @return size_t 应用的记录数
     */
    [[nodiscard]] size_t applied_count() const { return applied_count_; }

    /**
     * @brief 获取失败的记录数
     *
     * @return size_t 失败的记录数
     */
    [[nodiscard]] size_t failed_count() const { return failed_count_; }

private:
    // 页缓存
    std::map<uint32_t, std::vector<uint8_t>> pages_;

    // 统计信息
    size_t applied_count_ = 0;
    size_t failed_count_ = 0;
};

} // namespace innodb
} // namespace dbrecovery
