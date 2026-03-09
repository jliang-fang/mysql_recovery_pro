#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/utils/file_mapper.hpp"
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
    
    // InnoDB 默认页大小为 16KB (UNIV_PAGE_SIZE)
    // 实际页大小需通过其他方式确定（如用户指定或从 SDI 页解析）
    // 此处暂不尝试自动检测，避免错误推断
    if (page_size_ == 0 || (page_size_ & (page_size_ - 1)) != 0) {
        page_size_ = 16384; // fallback to default
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
    
    // 校验页校验和
    if (!validate_checksum(buffer)) {
        return {}; // 校验失败，返回空
    }
    
    return buffer;
}

bool PageReader::validate_checksum(const std::vector<uint8_t>& page_data) {
    if (page_data.size() < 40) return false;

    // InnoDB 校验和位于 FIL_PAGE_CHECKSUM (偏移 24-27)
    uint32_t stored_csum = dbrecovery::innodb::MachRead::read_le<uint32_t>(
        page_data.data(), 24
    );
    
    // 如果校验和为 0 或 0xFFFFFFFF，可能表示未启用校验或使用其他算法
    if (stored_csum == 0 || stored_csum == 0xFFFFFFFF) {
        return true; // 允许 NONE 校验和
    }
    
    uint32_t new_csum = compute_crc32c(page_data);

    return stored_csum == new_csum;
}

uint32_t PageReader::compute_crc32c(const std::vector<uint8_t>& page_data) {
    // 使用标准 CRC32C (Castagnoli) 算法
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
}

} // namespace innodb
} // namespace dbrecovery
