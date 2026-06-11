#ifndef DBRECOVERY_REPAIR_HEADER_REPAIRER_HPP
#define DBRECOVERY_REPAIR_HEADER_REPAIRER_HPP

#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include <unordered_set>
#include <functional>

namespace dbrecovery {
namespace repair {

/**
 * @brief HeaderRepairer: 修复损坏的 InnoDB 文件头
 *
 * 提供以下能力：
 * - 修复 FIL Header 中的校验和
 * - 重建页类型信息
 * - 修复空间ID和页号
 * - 修复校验和类型检测
 * - 多种校验和算法支持
 * - 文件级批量修复
 */
class HeaderRepairer {
public:
    HeaderRepairer() = default;
    ~HeaderRepairer() = default;

    // 禁止拷贝
    HeaderRepairer(const HeaderRepairer&) = delete;
    HeaderRepairer& operator=(const HeaderRepairer&) = delete;

    /**
     * @brief 修复文件头（包括 FIL Header 和 Page Header）
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @return true 修复成功
     * @return false 修复失败
     */
    bool repair_page_header(uint8_t* page_data, size_t page_size);

    /**
     * @brief 修复文件头校验和
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @return uint32_t 计算出的正确校验和
     */
    uint32_t calculate_and_fix_checksum(uint8_t* page_data, size_t page_size);

    /**
     * @brief 修复文件头校验和（指定算法）
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @param checksum_type 校验和类型: 0=CRC32C, 1=STATIC, 2=NONE
     * @return uint32_t 计算出的正确校验和
     */
    uint32_t calculate_and_fix_checksum_with_type(uint8_t* page_data, size_t page_size, int checksum_type);

    /**
     * @brief 验证页头有效性
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @return true 页头有效
     * @return false 页头无效
     */
    bool validate_page_header(const uint8_t* page_data, size_t page_size) const;

    /**
     * @brief 从损坏的文件中提取页类型
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @return std::optional<uint16_t> 页类型，失败返回空
     */
    std::optional<uint16_t> extract_page_type(const uint8_t* page_data, size_t page_size) const;

    /**
     * @brief 重建页头信息
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @param page_no 页号
     * @param page_type 页类型
     * @return true 重建成功
     * @return false 重建失败
     */
    bool rebuild_page_header(uint8_t* page_data, size_t page_size,
                            uint32_t page_no, uint16_t page_type);

    /**
     * @brief 批量修复文件中的所有页
     *
     * @param file_data 文件数据
     * @param file_size 文件大小
     * @param page_size 页大小
     * @return size_t 修复的页数
     */
    size_t repair_file_pages(uint8_t* file_data, size_t file_size, size_t page_size);

    /**
     * @brief 修复损坏的页号
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @param expected_page_no 期望的页号
     * @return true 修复成功
     * @return false 修复失败
     */
    bool repair_page_number(uint8_t* page_data, size_t page_size, uint32_t expected_page_no);

    /**
     * @brief 修复 LSN（日志序列号）
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @param lsn LSN 值
     * @return true 修复成功
     * @return false 修复失败
     */
    bool repair_lsn(uint8_t* page_data, size_t page_size, uint64_t lsn);

    /**
     * @brief 修复空间ID
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @param space_id 空间ID
     * @return true 修复成功
     * @return false 修复失败
     */
    bool repair_space_id(uint8_t* page_data, size_t page_size, uint32_t space_id);

    /**
     * @brief 修复页链关系（prev/next page）
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @param prev_page 前一个页
     * @param next_page 后一个页
     * @return true 修复成功
     * @return false 修复失败
     */
    bool repair_page_chain(uint8_t* page_data, size_t page_size, uint32_t prev_page, uint32_t next_page);

    /**
     * @brief 检测校验和类型
     *
     * @param page_data 页数据
     * @param page_size 页大小
     * @return int 校验和类型: 0=CRC32C, 1=STATIC, 2=NONE
     */
    int detect_checksum_type(const uint8_t* page_data, size_t page_size) const;

    /**
     * @brief 获取最后错误信息
     */
    const std::string& get_last_error() const { return last_error_; }

    /**
     * @brief 获取修复统计
     */
    const std::unordered_set<uint32_t>& get_repaired_pages() const { return repaired_pages_; }

    /**
     * @brief 清空统计信息
     */
    void clear_stats() { repaired_pages_.clear(); error_pages_.clear(); }

private:
    // 页头修复状态
    bool is_valid_ = true;
    std::string last_error_;
    
    // 修复统计
    std::unordered_set<uint32_t> repaired_pages_;
    std::unordered_set<uint32_t> error_pages_;
    
    // 修复参数
    int default_checksum_type_ = 0;  // 0=CRC32C, 1=STATIC, 2=NONE
    bool repair_all_headers_ = true;
};

} // namespace repair
} // namespace dbrecovery

#endif // DBRECOVERY_REPAIR_HEADER_REPAIRER_HPP
