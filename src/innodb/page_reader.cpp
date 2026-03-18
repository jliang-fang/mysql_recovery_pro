#include "dbrecovery/innodb/page_reader.hpp"
#include <iostream>
#include <array>

namespace dbrecovery {
namespace innodb {

// ==================== PageReader 静态工具类实现 ====================

bool PageReader::validate_checksum(const std::vector<uint8_t>& page_data, uint32_t* out_stored, uint32_t* out_computed) {
    // ✅ 增强：严格边界检查
    if (page_data.size() < 38 + 16) { // FIL header (38) + minimal record space
        return false;
    }
    
    // ✅ 新增：验证页大小是标准 InnoDB 页大小的倍数
    static constexpr std::array<size_t, 5> valid_page_sizes = {4096, 8192, 16384, 32768, 65536};
    bool is_valid_size = false;
    for (size_t ps : valid_page_sizes) {
        if (page_data.size() == ps) {
            is_valid_size = true;
            break;
        }
    }
    if (!is_valid_size) {
        std::cerr << "[WARN] Invalid page size: " << page_data.size() << std::endl;
        return false;
    }
    
    // 读取页类型（偏移 38+12=50）
    uint16_t page_type = read_le<uint16_t>(page_data.data(), 38 + 12);
    
    // ✅ 新增：SDI 页通常不启用校验和，直接返回 true
    if (page_type == dbrecovery::innodb::FIL_PAGE_SDI) {
        return true;
    }
    
    // 读取存储的校验和（偏移 24-27）
    uint32_t stored_checksum = read_le<uint32_t>(page_data.data(), 24);
    if (out_stored) *out_stored = stored_checksum;

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
        if (out_computed) *out_computed = computed_crc32c;
        return true;
    }

    // 尝试 INNODB 校验算法（MySQL 5.6/5.7 兼容）
    uint32_t computed_innodb = compute_innodb_checksum(page_copy);
    if (computed_innodb == stored_checksum) {
        if (out_computed) *out_computed = computed_innodb;
        return true;
    }

    // 尝试 OLD 校验算法（旧版本兼容）
    uint32_t computed_old = compute_old_checksum(page_copy);
    if (computed_old == stored_checksum) {
        if (out_computed) *out_computed = computed_old;
        return true;
    }
    
    // 尝试 CRC32 标准校验算法
    uint32_t computed_crc32 = compute_crc32(page_copy);
    if (computed_crc32 == stored_checksum) {
        if (out_computed) *out_computed = computed_crc32;
        return true;
    }

    // 所有校验均失败，输出详细错误信息
    std::cerr << "[ERROR] Page checksum mismatch. Stored=" << stored_checksum 
              << ", CRC32C=" << computed_crc32c 
              << ", INNODB=" << computed_innodb 
              << ", OLD=" << computed_old 
              << ", CRC32=" << computed_crc32 << std::endl;
    if (out_computed) *out_computed = computed_crc32c; // 默认返回 CRC32C
    return false;
}

uint32_t PageReader::compute_innodb_checksum(const std::vector<uint8_t>& page_data) {
    // 使用 MySQL 官方的 innobase_get_csum() 算法
    
    const uint32_t* data = reinterpret_cast<const uint32_t*>(page_data.data());
    size_t len = page_data.size() / 4;
    
    uint32_t checksum = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        // 跳过校验和字段 (offset 24-27, which is 6th and 7th uint32_t)
        if (i == 6 || i == 7) continue;
        
        checksum ^= dbrecovery::innodb::read_le<uint32_t>(page_data.data(), i * 4);
        checksum += (checksum >> 16);
        checksum &= 0xFFFFFFFF;
    }
    
    return (checksum >> 16) ^ (checksum & 0xFFFF);
}

uint32_t PageReader::compute_crc32c(const std::vector<uint8_t>& page_data) {
    // 预计算 CRC32C 查表（仅首次调用时执行）
    static const std::array<uint32_t, 256> crc_table = []() -> std::array<uint32_t, 256> {
        std::array<uint32_t, 256> table;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0x82F63B78; // CRC32C 多项式（Castagnoli）
                } else {
                    crc >>= 1;
                }
            }
            table[i] = crc;
        }
        return table;
    }();
    
    uint32_t crc = 0xFFFFFFFF;
    
    const size_t checksum_start = 24;
    const size_t checksum_end = 28;
    
    for (size_t i = 0; i < page_data.size(); ++i) {
        // 跳过校验和字段 (offset 24-27)
        if (i >= checksum_start && i < checksum_end) continue;
        
        uint8_t byte = page_data[i];
        crc = crc_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    
    return ~crc;
}

uint32_t PageReader::compute_crc32(const std::vector<uint8_t>& page_data) {
    // 使用标准 CRC32 算法（MySQL 5.6/5.7 可能使用）
    uint32_t crc = 0xFFFFFFFF;
    
    const size_t checksum_start = 24;
    const size_t checksum_end = 28;
    
    for (size_t i = 0; i < page_data.size(); ++i) {
        // 跳过校验和字段 (offset 24-27)
        if (i >= checksum_start && i < checksum_end) continue;
        
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

uint32_t PageReader::compute_old_checksum(const std::vector<uint8_t>& page_data) {
    // 使用 MySQL 5.5/5.6 的旧校验算法
    // 这是简单的累加和（非CRC）
    
    const size_t checksum_start = 24;
    const size_t checksum_end = 28;
    
    uint32_t checksum = 0;
    for (size_t i = 0; i < page_data.size(); ++i) {
        // 跳过校验和字段 (offset 24-27)
        if (i >= checksum_start && i < checksum_end) continue;
        
        checksum += page_data[i];
    }
    
    return checksum;
}

// ==================== InnoDBFileReader 实现 ====================

InnoDBFileReader::InnoDBFileReader(const std::string& file_path)
    : page_size_(16384), total_pages_(0), is_valid_(false) {
    
    mapper_ = std::make_unique<FileMapper>(file_path);
    if (!mapper_->is_valid()) {
        return;
    }

    // 计算总页数
    file_size_ = mapper_->size();
    
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
    is_valid_ = (total_pages_ > 0);
}

InnoDBFileReader::~InnoDBFileReader() = default;

InnoDBFileReader::InnoDBFileReader(InnoDBFileReader&& other) noexcept
    : file_size_(other.file_size_), page_size_(other.page_size_), total_pages_(other.total_pages_),
      mapper_(std::move(other.mapper_)), is_valid_(other.is_valid_) {
    other.file_size_ = 0;
    other.page_size_ = 16384;
    other.total_pages_ = 0;
    other.is_valid_ = false;
}

InnoDBFileReader& InnoDBFileReader::operator=(InnoDBFileReader&& other) noexcept {
    if (this != &other) {
        file_size_ = other.file_size_;
        page_size_ = other.page_size_;
        total_pages_ = other.total_pages_;
        mapper_ = std::move(other.mapper_);
        is_valid_ = other.is_valid_;

        other.file_size_ = 0;
        other.page_size_ = 16384;
        other.total_pages_ = 0;
        other.is_valid_ = false;
    }
    return *this;
}

std::vector<uint8_t> InnoDBFileReader::read_page(uint32_t page_no) {
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
    if (!PageReader::validate_checksum(page_data)) {
        std::cerr << "[WARN] Page " << page_no << " checksum validation failed" << std::endl;
        return {};
    }
    
    return page_data;
}

std::vector<uint8_t> InnoDBFileReader::read_page_raw(uint32_t page_no) {
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

std::span<const uint8_t> InnoDBFileReader::get_page_view(uint32_t page_no) {
    if (!is_valid_ || total_pages_ == 0) {
        return {};
    }
    
    // 计算页在文件中的偏移
    uint64_t offset = static_cast<uint64_t>(page_no) * page_size_;
    if (offset >= file_size_) {
        return {};
    }
    
    const uint8_t* data = mapper_->data() + offset;
    return std::span<const uint8_t>(data, page_size_);
}

} // namespace innodb
} // namespace dbrecovery
