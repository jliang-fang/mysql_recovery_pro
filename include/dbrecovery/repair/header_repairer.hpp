#ifndef DBRECOVERY_REPAIR_HEADER_REPAIRER_HPP
#define DBRECOVERY_REPAIR_HEADER_REPAIRER_HPP

#include <cstdint>
#include <vector>
#include <optional>
#include <string>

namespace dbrecovery {
namespace repair {

/**
 * @brief HeaderRepairer: 修复损坏的 InnoDB 文件头
 *
 * 提供以下能力：
 * - 修复 FIL Header 中的校验和
 * - 重建页类型信息
 * - 修复空间ID和页号
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

private:
    // 页头修复状态
    bool is_valid_ = true;
    std::string last_error_;
};

} // namespace repair
} // namespace dbrecovery

#endif // DBRECOVERY_REPAIR_HEADER_REPAIRER_HPP
