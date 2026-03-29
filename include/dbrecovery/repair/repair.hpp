#ifndef DBRECOVERY_REPAIR_REPAIR_HPP
#define DBRECOVERY_REPAIR_REPAIR_HPP

#include <cstdint>
#include <vector>
#include <optional>
#include <memory>
#include "dbrecovery/innodb/blob_pointer.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"

namespace dbrecovery {
namespace repair {

class Repairer {
public:
    /**
     * @brief 计算数据熵值（用于检测加密/压缩数据）
     */
    static double calculate_entropy(const uint8_t* data, size_t size);

    /**
     * @brief 检测文件是否可能被加密
     */
    static bool is_possibly_encrypted(const std::string& file_path, size_t sample_size = 16384);

    /**
     * @brief 检查页是否有效
     */
    static bool is_page_valid(const uint8_t* page, size_t page_size);

    /**
     * @brief 检查页是否损坏
     */
    static bool is_page_corrupted(const uint8_t* page, size_t page_size);

    /**
     * @brief 计算 InnoDB 校验和
     */
    static uint32_t calculate_innodb_checksum(const uint8_t* page, size_t page_size);

    /**
     * @brief 修复页校验和
     */
    static bool fix_checksum(uint8_t* page, size_t page_size);

    /**
     * @brief 检查 BLOB 指针是否安全
     */
    static bool is_blob_pointer_safe(const dbrecovery::innodb::BlobPointer& ptr);

    /**
     * @brief 从 SDI 获取 BLOB 信息
     */
    static std::optional<dbrecovery::innodb::BlobPointer> get_blob_info_from_sdi(
        const uint8_t* page,
        size_t page_size,
        std::shared_ptr<dbrecovery::innodb::SDIManager> sdi_manager
    );

    /**
     * @brief 从相邻页获取 BLOB 信息
     */
    static std::optional<dbrecovery::innodb::BlobPointer> get_blob_info_from_neighbor_pages(
        const uint8_t* page,
        size_t page_size,
        size_t page_index,
        const std::vector<uint8_t>& file_data
    );

    /**
     * @brief 修复 BLOB 指针
     */
    static bool fix_blob_pointer(uint8_t* page, size_t page_size);

    /**
     * @brief 修复 SDI 记录
     */
    static bool fix_sdi_record(std::vector<uint8_t>& data, size_t page_size);

    /**
     * @brief 修复单个页
     */
    static bool repair_page(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, size_t page_size);

    /**
     * @brief 修复整个文件
     */
    static bool repair_file(const std::string& input_file, const std::string& output_file, std::optional<size_t> page_size = std::nullopt);
};

} // namespace repair
} // namespace dbrecovery

#endif // DBRECOVERY_REPAIR_REPAIR_HPP
