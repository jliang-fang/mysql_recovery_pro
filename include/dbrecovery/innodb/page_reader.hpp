#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <memory>
#include <map>
#include <iostream>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/utils/file_mapper.hpp"

// 前向声明 TDEDecryptor，避免循环依赖
namespace dbrecovery { namespace innodb { class TDEDecryptor; } }

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
     * @param strict_mode 是否启用严格模式（true=失败即返回false, false=记录警告但继续）
     * @param out_stored 可选：输出存储的校验和值
     * @param out_computed 可选：输出计算出的校验和值
     * @return true 校验通过或非严格模式下允许继续
     */
    static bool validate_checksum(const std::vector<uint8_t>& page_data, 
                                  bool strict_mode = true,
                                  uint32_t* out_stored = nullptr,
                                  uint32_t* out_computed = nullptr);

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
    explicit InnoDBFileReader(const std::string& file_path, uint32_t page_size_hint);
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
     * @brief 读取指定页号的数据并尝试解密（TDE）
     *
     * @param page_no 页号（从0开始）
     * @param decryptor TDE 解密器
     * @return std::vector<uint8_t> 页数据（已解密或原始），失败返回空
     */
    std::vector<uint8_t> read_page_with_decrypt(uint32_t page_no,
                                                innodb::TDEDecryptor& decryptor);

    /**
     * @brief 获取指定页号的数据视图（零拷贝，不校验）
     *
     * @param page_no 页号（从0开始）
     * @return std::span<const uint8_t> 页数据视图，失败返回空span
     */
    std::span<const uint8_t> get_page_view(uint32_t page_no);

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

    /**
     * @brief 设置表空间ID（用于TDE密钥派生）
     *
     * @param space_id 表空间ID
     */
    void set_space_id(uint32_t space_id) { space_id_ = space_id; }

    /**
     * @brief 设置页大小提示（用于覆盖自动检测）
     *
     * @param page_size 页大小
     */
    void set_page_size_hint(uint32_t page_size) {
        std::cout << "[DEBUG] set_page_size_hint called with: " << page_size << std::endl;
        page_size_hint_ = page_size;
        // 如果文件已打开且设置了有效的页大小提示，重新计算页大小
        if (page_size > 0 && page_size <= 65536 && file_size_ > 0 && file_size_ % page_size == 0) {
            std::cout << "[DEBUG] Applying page_size_hint: " << page_size << " to file_size: " << file_size_ << std::endl;
            page_size_ = page_size;
            total_pages_ = static_cast<uint32_t>(file_size_ / page_size);
            is_valid_ = (total_pages_ > 0 || file_size_ > 0);
        }
    }

private:
    uint64_t file_size_;
    uint32_t page_size_;
    uint32_t total_pages_;
    uint32_t space_id_ = 0;  // 表空间ID（用于TDE）
    uint32_t page_size_hint_ = 0;  // 页大小提示（非零时覆盖自动检测）
    std::unique_ptr<utils::FileMapper> mapper_;
    bool is_valid_;
};

} // namespace innodb
} // namespace dbrecovery

