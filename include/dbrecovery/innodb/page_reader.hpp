#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <memory>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/utils/file_mapper.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief PageReader: InnoDB 页校验和计算与验证工具类
 *
 * 提供以下能力：
 * - 校验页校验和（CRC32C, INNODB, NONE）
 * - 计算各种校验和算法
 *
 * 此类为纯静态工具类，不持有任何状态。
 */
class PageReader {
public:
    // 禁止实例化
    PageReader() = delete;
    ~PageReader() = delete;
    PageReader(const PageReader&) = delete;
    PageReader& operator=(const PageReader&) = delete;

    /**
     * @brief 校验页校验和（支持 CRC32C / INNODB / NONE）
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return true 校验通过
     */
    static bool validate_checksum(const std::vector<uint8_t>& page_data);

    /**
     * @brief 计算页的 INNODB 校验和（MySQL 5.6/5.7 兼容）
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return uint32_t INNODB 校验和值
     */
    static uint32_t compute_innodb_checksum(const std::vector<uint8_t>& page_data);

    /**
     * @brief 计算页的 CRC32C 校验和（MySQL 8.0 默认）
     *
     * 使用 Castagnoli 多项式 (0x82F63B78) 实现 CRC32C
     * 参考：https://en.wikipedia.org/wiki/Cyclic_redundancy_check#CRC32C
     * 
     * ✅ 优化：使用查表法提升性能（每页处理时间从数毫秒降至微秒级）
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return uint32_t CRC32C 值
     */
    static uint32_t compute_crc32c(const std::vector<uint8_t>& page_data);

    /**
     * @brief 计算页的标准 CRC32 (ISO/Hermann) 校验和
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return uint32_t CRC32 值
     */
    static uint32_t compute_crc32(const std::vector<uint8_t>& page_data);

    /**
     * @brief 计算页的 OLD 校验和（MySQL 5.5/5.6 兼容）
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return uint32_t OLD 校验和值
     */
    static uint32_t compute_old_checksum(const std::vector<uint8_t>& page_data);
};

/**
 * @brief InnoDBFileReader: 管理 .ibd 文件并提供页读取服务
 *
 * 此类负责文件的打开、内存映射，并使用 PageReader 工具类来验证数据。
 */
class InnoDBFileReader {
public:
    explicit InnoDBFileReader(const std::string& file_path);
    ~InnoDBFileReader();

    // 禁止拷贝
    InnoDBFileReader(const InnoDBFileReader&) = delete;
    InnoDBFileReader& operator=(const InnoDBFileReader&) = delete;

    // 支持移动语义
    InnoDBFileReader(InnoDBFileReader&& other) noexcept;
    InnoDBFileReader& operator=(InnoDBFileReader&& other) noexcept;

    /**
     * @brief 读取指定页号的数据（带校验）
     *
     * @param page_no 页号（从0开始）
     * @return std::vector<uint8_t> 页数据，失败返回空
     */
    std::vector<uint8_t> read_page(uint32_t page_no);

    /**
     * @brief 读取指定页号的数据（不校验）
     *
     * @param page_no 页号（从0开始）
     * @return std::vector<uint8_t> 页数据，失败返回空
     */
    std::vector<uint8_t> read_page_raw(uint32_t page_no);

    /**
     * @brief 获取文件总页数
     *
     * @return uint32_t 总页数
     */
    uint32_t get_total_pages() const { return total_pages_; }

    /**
     * @brief 获取文件大小（字节）
     *
     * @return uint64_t 文件大小
     */
    uint64_t get_file_size() const { return file_size_; }

    /**
     * @brief 获取页大小
     *
     * @return uint32_t 页大小
     */
    uint32_t get_page_size() const { return page_size_; }

    /**
     * @brief 获取文件有效性状态
     *
     * @return true 文件有效且已成功初始化
     * @return false 文件无效或初始化失败
     */
    [[nodiscard]] bool is_valid() const { return is_valid_; }

private:
    uint64_t file_size_;
    uint32_t page_size_;
    uint32_t total_pages_;
    std::unique_ptr<FileMapper> mapper_;
    bool is_valid_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_READER_HPP
