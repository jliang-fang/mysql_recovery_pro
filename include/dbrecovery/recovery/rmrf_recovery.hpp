#ifndef DBRECOVERY_RECOVERY_RMRF_RECOVERY_HPP
#define DBRECOVERY_RECOVERY_RMRF_RECOVERY_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace recovery {

/**
 * @brief RmrfRecovery: rm -rf 灾难恢复器
 *
 * 提供以下能力：
 * - 目录扫描：查找所有 .ibd 文件
 * - 原始设备扫描：直接扫描磁盘/分区
 * - 页识别：识别 InnoDB 页类型
 * - 记录恢复：从碎片化页中恢复记录
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

private:
    uint32_t page_size_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> scanned_pages_;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> sdi_pages_;
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_RMRF_RECOVERY_HPP
