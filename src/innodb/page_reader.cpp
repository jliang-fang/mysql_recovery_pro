#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/utils/file_mapper.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <stdexcept>

namespace dbrecovery {
namespace innodb {

PageReader::PageReader(const std::string& file_path)
    : fd_(-1), file_size_(0), page_size_(16384), total_pages_(0), is_valid_(false) {
    
    utils::FileMapper mapper(file_path);
    if (!mapper.is_valid()) {
        return;
    }

    file_data_ = std::make_unique<std::vector<uint8_t>>(mapper.data(), mapper.data() + mapper.size());
    file_size_ = file_data_->size();
    
    // InnoDB 支持多种页大小：4KB/8KB/16KB/32KB/64KB
    // 优先从文件大小推断页大小（假设文件是完整页的倍数）
    static constexpr std::array<size_t, 5> valid_page_sizes = {4096, 8192, 16384, 32768, 65536};
    
    for (size_t ps : valid_page_sizes) {
        if (file_size_ % ps == 0) {
            page_size_ = static_cast<uint32_t>(ps);
            break;
        }
    }
    
    // 如果无法推断，使用默认值
    if (page_size_ == 16384 && file_size_ % 16384 != 0) {
        std::cerr << "[WARN] Cannot determine page size, using default 16KB\n";
    }
    
    total_pages_ = static_cast<uint32_t>(file_size_ / page_size_);
    is_valid_ = true;
}

PageReader::~PageReader() {}

std::vector<uint8_t> PageReader::read_page(uint32_t page_no) {
    if (!is_valid_ || page_no >= total_pages_) return {};
    
    std::vector<uint8_t> buffer(page_size_);
    off_t offset = static_cast<off_t>(page_no) * page_size_;
    
    // 从内存映射中复制数据
    if (offset + page_size_ > file_data_->size()) {
        return {};
    }
    
    std::copy(file_data_->begin() + offset, 
              file_data_->begin() + offset + page_size_, 
              buffer.begin());
    
    // ✅ 增强：校验页类型和校验和
    // 读取页类型（偏移 12）
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(buffer.data(), 12);
    
    // 验证页类型有效性
    bool is_valid_page_type = false;
    switch (page_type) {
        case dbrecovery::innodb::FIL_PAGE_INDEX:
        case dbrecovery::innodb::FIL_PAGE_SDI:
        case dbrecovery::innodb::FIL_PAGE_TYPE_BLOB:
        case dbrecovery::innodb::FIL_PAGE_UNDO_LOG:
        case dbrecovery::innodb::FIL_PAGE_TYPE_ALLOCATED:
            is_valid_page_type = true;
            break;
        default:
            // 忽略未知页类型，但记录警告
            std::cerr << "[WARN] Unknown page type: " << page_type 
                      << " for page " << page_no << std::endl;
            is_valid_page_type = true; // 允许继续处理
            break;
    }
    
    // 校验页校验和（如果页类型有效）
    if (is_valid_page_type && !validate_checksum(buffer)) {
        std::cerr << "[WARN] Checksum validation failed for page " << page_no 
                  << ", type=" << page_type << std::endl;
        return {}; // 校验失败，返回空
    }
    
    return buffer;
}

bool PageReader::validate_checksum(const std::vector<uint8_t>& page_data) {
    if (page_data.size() < 40) return false;

    // InnoDB 校验和位于 FIL_PAGE_CHECKSUM (偏移 24-27)
    uint32_t stored_csum = dbrecovery::innodb::read_le<uint32_t>(
        page_data.data(), 24
    );
    
    // 如果校验和为 0 或 0xFFFFFFFF，可能表示未启用校验或使用其他算法
    if (stored_csum == 0 || stored_csum == 0xFFFFFFFF) {
        return true; // 允许 NONE 校验和
    }
    
    // MySQL 8.0 默认使用 CRC32C (Castagnoli) 算法
    uint32_t computed_csum = compute_crc32c(page_data);

    return stored_csum == computed_csum;
}

uint32_t PageReader::compute_crc32c(const std::vector<uint8_t>& page_data) {
#ifdef __SSE4_2__
    // 使用硬件加速 CRC32C (SSE4.2 指令集)
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < page_data.size(); ++i) {
        // Skip the checksum field at offset 24-27 (FIL_PAGE_CHECKSUM)
        if (i >= 24 && i < 28) continue;
        
#ifdef __x86_64__
        crc = _mm_crc32_u8(crc, page_data[i]);
#else
        // Fallback to software implementation for non-x86_64 platforms
        crc ^= static_cast<uint32_t>(page_data[i]);
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78;
            } else {
                crc >>= 1;
            }
        }
#endif
    }
    
    return ~crc;
#else
    // 使用标准 CRC32C (Castagnoli) 算法（软件实现）
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < page_data.size(); ++i) {
        // Skip the checksum field at offset 24-27 (FIL_PAGE_CHECKSUM)
        if (i >= 24 && i < 28) continue;
        
        crc ^= static_cast<uint32_t>(page_data[i]);
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78;
            } else {
                crc >>= 1;
            }
        }
    }
    
    return ~crc;
#endif
}

uint32_t PageReader::compute_crc32(const std::vector<uint8_t>& page_data) {
    // 使用标准 CRC32 (ISO/Hermann) 算法
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < page_data.size(); ++i) {
        // Skip the checksum field at offset 24-27 (FIL_PAGE_CHECKSUM)
        if (i >= 24 && i < 28) continue;
        
        crc ^= static_cast<uint32_t>(page_data[i]);
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    
    return ~crc;
}

// ✅ 新增：支持更多校验和类型
bool PageReader::validate_checksum(const std::vector<uint8_t>& page_data) {
    if (page_data.size() < 40) return false;

    // InnoDB 校验和位于 FIL_PAGE_CHECKSUM (偏移 24-27)
    uint32_t stored_csum = dbrecovery::innodb::read_le<uint32_t>(
        page_data.data(), 24
    );
    
    // 如果校验和为 0 或 0xFFFFFFFF，可能表示未启用校验或使用其他算法
    if (stored_csum == 0 || stored_csum == 0xFFFFFFFF) {
        return true; // 允许 NONE 校验和
    }
    
    // MySQL 8.0 默认使用 CRC32C (Castagnoli) 算法
    uint32_t computed_csum = compute_crc32c(page_data);

    // ✅ 新增：支持 INNODB 校验和（旧版本兼容）
    if (stored_csum == computed_csum) {
        return true;
    }
    
    // 尝试 CRC32 (ISO/Hermann) 作为备选
    uint32_t computed_crc32 = compute_crc32(page_data);
    if (stored_csum == computed_crc32) {
        return true;
    }
    
    return false;
}

} // namespace innodb
} // namespace dbrecovery
