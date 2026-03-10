#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <cstring>

namespace dbrecovery {
namespace innodb {

/**
 * @brief PageReader: 从 .ibd 文件中读取 InnoDB 页
 *
 * 提供以下能力：
 * - 读取指定偏移的页数据
 * - 校验页校验和（CRC32C, INNODB, NONE）
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
