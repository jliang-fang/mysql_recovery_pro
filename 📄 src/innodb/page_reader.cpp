#include "dbrecovery/innodb/page_reader.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>

namespace dbrecovery {
namespace innodb {

PageReader::PageReader(const std::string& file_path) 
    : fd_(-1), file_size_(0), page_size_(16384), total_pages_(0) {
    
    fd_ = open(file_path.c_str(), O_RDONLY);
    if (fd_ < 0) return;
    
    struct stat st;
    if (fstat(fd_, &st) == 0) {
        file_size_ = st.st_size;
        
        // Step 1: Read first page to determine actual page size
        std::vector<uint8_t> first_page(16384); // max possible header read
        ssize_t bytes_read = pread(fd_, first_page.data(), 16384, 0);
        if (bytes_read >= 32) {
            // Decode page size from offset 28-29
            uint16_t encoded_size = *reinterpret_cast<uint16_t*>(first_page.data() + 28);
            page_size_ = static_cast<uint32_t>(1 << (encoded_size & 0x3FFF));
        }
        
        // Step 2: Calculate total pages
        if (page_size_ > 0) {
            total_pages_ = static_cast<uint32_t>(file_size_ / page_size_);
        } else {
            close(fd_);
            fd_ = -1;
        }
    }
}

PageReader::~PageReader() {
    if (fd_ >= 0) close(fd_);
}

std::vector<uint8_t> PageReader::read_page(uint32_t page_no) {
    if (fd_ < 0 || page_no >= total_pages_) return {};
    
    std::vector<uint8_t> buffer(page_size_);
    off_t offset = static_cast<off_t>(page_no) * page_size_;
    
    if (pread(fd_, buffer.data(), page_size_, offset) != static_cast<ssize_t>(page_size_)) {
        return {};
    }
    
    return buffer;
}

} // namespace innodb
} // namespace dbrecovery
