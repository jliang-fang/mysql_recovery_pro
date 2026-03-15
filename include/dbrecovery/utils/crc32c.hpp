#ifndef DBRECOVERY_UTILS_CRC32C_HPP
#define DBRECOVERY_UTILS_CRC32C_HPP

#include <cstdint>
#include <vector>

namespace dbrecovery {
namespace utils {

/**
 * @brief 计算 CRC32C 校验和（Castagnoli 多项式）
 *
 * @param data 输入数据
 * @param size 数据大小
 * @return uint32_t CRC32C 值
 */
uint32_t compute_crc32c(const uint8_t* data, size_t size);

/**
 * @brief 计算页的 CRC32C 校验和（跳过校验和字段）
 *
 * @param page_data 页数据
 * @return uint32_t CRC32C 值
 */
uint32_t compute_page_crc32c(const std::vector<uint8_t>& page_data);

} // namespace utils
} // namespace dbrecovery

#endif // DBRECOVERY_UTILS_CRC32C_HPP
