#include "dbrecovery/innodb/page_reader.hpp"
#include <iostream>

namespace dbrecovery {
namespace innodb {

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

} // namespace innodb
} // namespace dbrecovery
