#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <span>

namespace dbrecovery {
namespace innodb {

/**
 * @brief MachRead: 机器字节序读取工具类
 *
 * 提供跨平台的 little-endian 和 big-endian 整数读取功能。
 */
class MachRead {
public:
    template<typename T>
    static T read_le(const uint8_t* ptr, size_t offset) noexcept {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
        const uint8_t* p = ptr + offset;
        if constexpr (sizeof(T) == 1) return *p;
        else if constexpr (sizeof(T) == 2) {
            #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                return *reinterpret_cast<const uint16_t*>(p);
            #else
                return static_cast<T>(p[0]) | (static_cast<T>(p[1]) << 8);
            #endif
        }
        else if constexpr (sizeof(T) == 4) {
            #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                return *reinterpret_cast<const uint32_t*>(p);
            #else
                return static_cast<T>(p[0]) | (static_cast<T>(p[1]) << 8) |
                       (static_cast<T>(p[2]) << 16) | (static_cast<T>(p[3]) << 24);
            #endif
        }
        else if constexpr (sizeof(T) == 8) {
            #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                return *reinterpret_cast<const uint64_t*>(p);
            #else
                uint64_t lo = static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
                              (static_cast<uint64_t>(p[2]) << 16) | (static_cast<uint64_t>(p[3]) << 24);
                uint64_t hi = static_cast<uint64_t>(p[4]) | (static_cast<uint64_t>(p[5]) << 8) |
                              (static_cast<uint64_t>(p[6]) << 16) | (static_cast<uint64_t>(p[7]) << 24);
                return lo | (hi << 32);
            #endif
        }
    }

    template<typename T>
    static T read_be(const uint8_t* ptr, size_t offset) noexcept {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
        const uint8_t* p = ptr + offset;
        if constexpr (sizeof(T) == 1) return *p;
        else if constexpr (sizeof(T) == 2) {
            #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                return static_cast<T>(p[0]) | (static_cast<T>(p[1]) << 8);
            #else
                return *reinterpret_cast<const uint16_t*>(p);
            #endif
        }
        else if constexpr (sizeof(T) == 4) {
            #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                return static_cast<T>(p[0]) | (static_cast<T>(p[1]) << 8) |
                       (static_cast<T>(p[2]) << 16) | (static_cast<T>(p[3]) << 24);
            #else
                return *reinterpret_cast<const uint32_t*>(p);
            #endif
        }
        else if constexpr (sizeof(T) == 8) {
            #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                uint64_t lo = static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
                              (static_cast<uint64_t>(p[2]) << 16) | (static_cast<uint64_t>(p[3]) << 24);
                uint64_t hi = static_cast<uint64_t>(p[4]) | (static_cast<uint64_t>(p[5]) << 8) |
                              (static_cast<uint64_t>(p[6]) << 16) | (static_cast<uint64_t>(p[7]) << 24);
                return lo | (hi << 32);
            #else
                uint64_t hi = static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
                              (static_cast<uint64_t>(p[2]) << 16) | (static_cast<uint64_t>(p[3]) << 24);
                uint64_t lo = static_cast<uint64_t>(p[4]) | (static_cast<uint64_t>(p[5]) << 8) |
                              (static_cast<uint64_t>(p[6]) << 16) | (static_cast<uint64_t>(p[7]) << 24);
                return lo | (hi << 32);
            #endif
        }
    }
};

/**
 * @brief PageReader: 从 .ibd 文件中读取 InnoDB 页
 *
 * 提供以下能力：
 * - 读取指定偏移的页数据
 * - 校验页校验和（CRC32, INNODB, NONE）
 * - 解析页类型（FIL_PAGE_INDEX, FIL_PAGE_SDI 等）
 */
class PageReader {
public:
    explicit PageReader(const std::string& file_path);
    ~PageReader();

    // 禁止拷贝
    PageReader(const PageReader&) = delete;
    PageReader& operator=(const PageReader&) = delete;

    /**
     * @brief 读取指定页号的数据（带校验）
     *
     * @param page_no 页号（从0开始）
     * @return std::vector<uint8_t> 页数据，失败返回空
     */
    std::vector<uint8_t> read_page(uint32_t page_no);

    /**
     * @brief 获取文件总页数
     *
     * @return uint32_t 总页数
     */
    uint32_t get_total_pages() const { return total_pages_; }

    /**
     * @brief 校验页校验和（支持 CRC32C / INNODB / NONE）
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return true 校验通过
     */
    static bool validate_checksum(const std::vector<uint8_t>& page_data);

    /**
     * @brief 计算页的 CRC32C 校验和
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return uint32_t CRC32C 值
     */
    static uint32_t compute_crc32c(const std::vector<uint8_t>& page_data);

private:
    int fd_;
    uint64_t file_size_;
    uint32_t page_size_;
    uint32_t total_pages_;
    std::unique_ptr<std::vector<uint8_t>> file_data_;
    bool is_valid_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_READER_HPP
