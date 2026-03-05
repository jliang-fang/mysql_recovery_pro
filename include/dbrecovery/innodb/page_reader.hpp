#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dbrecovery {
namespace innodb {

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
     * @brief 读取指定页号的数据
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

private:
    int fd_;
    uint64_t file_size_;
    uint32_t page_size_;
    uint32_t total_pages_;
};

} // namespace innodb
} // namespace dbrecovery
