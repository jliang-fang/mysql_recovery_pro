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
 * @brief PageReader: 从 .ibd 文件中读取 InnoDB 页
 *
 * 提供以下能力：
 * - 读取指定偏移的页数据
 * - 校验页校验和（CRC32C, INNODB, NONE）
 * - 解析页类型（FIL_PAGE_INDEX, FIL_PAGE_SDI 等）
 */
class PageReader {
public:
    explicit PageReader(const std::string& file_path)
        : page_size_(16384), total_pages_(0), is_valid_(false) {
        
        mapper_ = std::make_unique<FileMapper>(file_path);
        if (!mapper_->is_valid()) {
            return;
        }
        
        // 计算总页数
        file_size_ = mapper_->size();
        total_pages_ = static_cast<uint32_t>(file_size_ / page_size_);
        
        is_valid_ = (total_pages_ > 0);
    }

    ~PageReader() = default;

    // 禁止拷贝
    PageReader(const PageReader&) = delete;
    PageReader& operator=(const PageReader&) = delete;

    /**
     * @brief 读取指定页号的数据（带校验）
     *
     * @param page_no 页号（从0开始）
     * @return std::vector<uint8_t> 页数据，失败返回空
     */
    std::vector<uint8_t> read_page(uint32_t page_no) {
        if (!is_valid_ || total_pages_ == 0) {
            return {};
        }
        
        // 计算页在文件中的偏移
        uint64_t offset = static_cast<uint64_t>(page_no) * page_size_;
        if (offset >= file_size_) {
            return {};
        }
        
        // 读取一页数据
        std::vector<uint8_t> page_data(page_size_);
        const uint8_t* data = mapper_->data() + offset;
        std::memcpy(page_data.data(), data, page_size_);
        
        // 校验校验和
        if (!validate_checksum(page_data)) {
            std::cerr << "[WARN] Page " << page_no << " checksum validation failed" << std::endl;
            return {};
        }
        
        return page_data;
    }

    /**
     * @brief 读取指定页号的数据（不校验）
     *
     * @param page_no 页号（从0开始）
     * @return std::vector<uint8_t> 页数据，失败返回空
     */
    std::vector<uint8_t> read_page_raw(uint32_t page_no) {
        if (!is_valid_ || total_pages_ == 0) {
            return {};
        }
        
        // 计算页在文件中的偏移
        uint64_t offset = static_cast<uint64_t>(page_no) * page_size_;
        if (offset >= file_size_) {
            return {};
        }
        
        // 读取一页数据
        std::vector<uint8_t> page_data(page_size_);
        const uint8_t* data = mapper_->data() + offset;
        std::memcpy(page_data.data(), data, page_size_);
        
        return page_data;
    }

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
     * @brief 校验页校验和（支持 CRC32C / INNODB / NONE）
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return true 校验通过
     */
    static bool validate_checksum(const std::vector<uint8_t>& page_data) {
        if (page_data.size() < 50) {
            return false; // 页太小，无法校验
        }
        
        // 读取存储的校验和（偏移 24-27）
        uint32_t stored_checksum = read_le<uint32_t>(page_data.data(), 24);
        
        if (stored_checksum == 0 || stored_checksum == 0xFFFFFFFF) {
            // NONE 校验模式或未初始化的校验和
            return true;
        }
        
        // 计算当前校验和（使用 CRC32C）
        std::vector<uint8_t> page_copy = page_data;
        // 临时清零校验和字段以便计算
        for (int i = 0; i < 4; ++i) {
            page_copy[24 + i] = 0;
        }
    
        uint32_t computed_crc32c = compute_crc32c(page_copy);
        if (computed_crc32c == stored_checksum) {
            return true;
        }

        // 尝试 INNODB 校验算法（MySQL 5.6/5.7 兼容）
        uint32_t computed_innodb = compute_innodb_checksum(page_copy);
        if (computed_innodb == stored_checksum) {
            return true;
        }
    
        // 尝试 OLD 校验算法（旧版本兼容）
        uint32_t computed_old = compute_old_checksum(page_copy);
        if (computed_old == stored_checksum) {
            return true;
        }
    
        // 所有校验均失败
        return false;
    }

    /**
     * @brief 计算页的 INNODB 校验和（MySQL 5.6/5.7 兼容）
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return uint32_t INNODB 校验和值
     */
    static uint32_t compute_innodb_checksum(const std::vector<uint8_t>& page_data) {
        // 使用 MySQL 官方的 innobase_get_csum() 算法
        
        const uint32_t* data = reinterpret_cast<const uint32_t*>(page_data.data());
        size_t len = page_data.size() / 4;
        
        uint32_t checksum = 0xFFFFFFFF;
        for (size_t i = 0; i < len; ++i) {
            // 跳过校验和字段 (offset 24-27, which is 6th and 7th uint32_t)
            if (i == 6 || i == 7) continue;
            
            checksum ^= mach_read_from_4(data + i);
            checksum += (checksum >> 16);
            checksum &= 0xFFFFFFFF;
        }
        
        return (checksum >> 16) ^ (checksum & 0xFFFF);
    }

    /**
     * @brief 计算页的 CRC32C 校验和（MySQL 8.0 默认）
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return uint32_t CRC32C 值
     */
    static uint32_t compute_crc32c(const std::vector<uint8_t>& page_data) {
        // 软件实现的 CRC32C（使用正确的多项式）
        uint32_t crc = 0xFFFFFFFF;
        
        for (size_t i = 0; i < page_data.size(); ++i) {
            if (i >= 24 && i < 28) continue; // Skip checksum field at offset 24-27
            
            uint8_t byte = page_data[i];
            crc ^= byte;
            
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0x82F63B78; // CRC32C 多项式（Castagnoli）
                } else {
                    crc >>= 1;
                }
            }
        }
        
        return ~crc;
    }

    /**
     * @brief 计算页的标准 CRC32 (ISO/Hermann) 校验和
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return uint32_t CRC32 值
     */
    static uint32_t compute_crc32(const std::vector<uint8_t>& page_data) {
        // 使用标准 CRC32 算法（MySQL 5.6/5.7 可能使用）
        uint32_t crc = 0xFFFFFFFF;
        
        for (size_t i = 0; i < page_data.size(); ++i) {
            if (i >= 24 && i < 28) continue; // Skip checksum field
            
            uint8_t byte = page_data[i];
            crc ^= byte;
            
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320; // CRC32 多项式（ISO/Hermann）
                } else {
                    crc >>= 1;
                }
            }
        }
        
        return ~crc;
    }

    /**
     * @brief 计算页的 OLD 校验和（MySQL 5.5/5.6 兼容）
     *
     * @param page_data 页数据（包含 FIL_HEADER）
     * @return uint32_t OLD 校验和值
     */
    static uint32_t compute_old_checksum(const std::vector<uint8_t>& page_data) {
        // 使用 MySQL 5.5/5.6 的旧校验算法
        // 这是简单的累加和（非CRC）
        
        const uint32_t* data = reinterpret_cast<const uint32_t*>(page_data.data());
        size_t len = page_data.size() / 4;
        
        uint32_t checksum = 0;
        for (size_t i = 0; i < len; ++i) {
            // 跳过校验和字段 (offset 24-27, which is 6th and 7th uint32_t)
            if (i == 6 || i == 7) continue;
            
            checksum += mach_read_from_4(data + i);
        }
        
        return checksum;
    }

    uint64_t file_size_;
    uint32_t page_size_;
    uint32_t total_pages_;
    std::unique_ptr<FileMapper> mapper_;
    bool is_valid_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_READER_HPP
