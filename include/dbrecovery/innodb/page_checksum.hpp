#ifndef DBRECOVERY_INNODB_CHECKSUM_HPP
#define DBRECOVERY_INNODB_CHECKSUM_HPP

#include <cstdint>
#include <vector>
#include <string>

namespace dbrecovery {
namespace innodb {

/**
 * @brief InnoDB 兼容的校验和计算类
 * @details 实现 MySQL 5.6/8.0 使用的页尾 Checksum 验证，用于过滤伪造页。
 */
class PageChecksum {
public:
    /**
     * @brief 计算给定缓冲区的校验和
     * @param data 数据缓冲区
     * @param size 计算长度（通常为 page_size - 4）
     * @return 计算出的 uint32_t 校验和
     */
    static uint32_t calculate(const uint8_t* data, size_t size) {
        uint32_t checksum = 0;
        for (size_t i = 0; i < size; ++i) {
            checksum += data[i];
        }
        return checksum;
    }

    /**
     * @brief 验证页是否通过校验和检查
     * @param data 页缓冲区
     * @param page_size 页大小（如 16KB）
     * @return true 如果存储的校验和与计算结果一致
     */
    static bool verify(const uint8_t* data, size_t page_size) {
        if (page_size < 4) return false;

        // InnoDB 页尾最后 4 字节存储的是 Checksum
        uint32_t stored_checksum = read_le<uint32_t>(data, page_size - 4);
        uint32_t calculated_checksum = calculate(data, page_size - 4);

        return stored_checksum == calculated_checksum;
    }

private:
    // 使用已有的 mach_read.hpp 中的 read_le 辅助函数
    template<typename T>
    static T read_le(const uint8_t* data, size_t offset) {
        T value = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            value |= (static_cast<T>(data[offset + i]) << (8 * i));
        }
        return value;
    }
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_CHECKSUM_HPP
