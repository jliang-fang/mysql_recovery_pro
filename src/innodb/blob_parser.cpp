#include "dbrecovery/innodb/blob_pointer.hpp"
#include <iostream>
#include <vector>

namespace dbrecovery {
namespace innodb {

// ✅ 新增：从 BLOB 页数据中读取实际数据长度
size_t read_blob_page_data_length(const std::vector<uint8_t>& page_data) {
    if (page_data.size() < BlobPageChain::BTR_BPAGE_LEN + 4) {
        return 0;
    }
    
    // 读取数据长度字段（偏移 8）
    return static_cast<size_t>(dbrecovery::innodb::read_le<uint32_t>(
        page_data.data(), BlobPageChain::BTR_BPAGE_LEN
    ));
}

// ✅ 新增：从 BLOB 页链中读取完整数据
std::vector<uint8_t> read_blob_data_from_chain(
    const BlobPageChain& chain,
    uint32_t total_len,
    const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader
) {
    if (!page_reader || chain.pages.empty()) {
        return {};
    }
    
    std::vector<uint8_t> blob_data;
    blob_data.reserve(total_len);
    
    for (const auto& [space_id, page_no] : chain.pages) {
        auto page_data = page_reader(space_id, page_no);
        if (!page_data || page_data->size() < 16) {
            std::cerr << "[ERROR] Failed to read BLOB page (" << space_id << ", " << page_no << ")" << std::endl;
            continue;
        }
        
        // 读取数据长度
        size_t data_len = read_blob_page_data_length(*page_data);
        if (data_len == 0 || data_len > page_data->size() - 16) {
            std::cerr << "[WARN] Invalid BLOB page data length: " << data_len << std::endl;
            continue;
        }
        
        // 添加数据到结果（跳过页头16字节）
        size_t data_offset = 16; // BLOB 页头大小
        blob_data.insert(blob_data.end(), page_data->begin() + data_offset, 
                        page_data->begin() + data_offset + data_len);
    }
    
    // 截断到总长度（防止溢出）
    if (blob_data.size() > total_len) {
        blob_data.resize(total_len);
    }
    
    return blob_data;
}

// ✅ 新增：BLOB 页链验证与校验和检查
bool validate_blob_page_chain(const BlobPageChain& chain,
                              const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader) {
    if (chain.pages.empty()) {
        return false;
    }
    
    for (size_t i = 0; i < chain.pages.size(); ++i) {
        const auto& [space_id, page_no] = chain.pages[i];
        
        auto page_data = page_reader(space_id, page_no);
        if (!page_data || page_data->size() < 62) {
            std::cerr << "[ERROR] BLOB page (" << space_id << ", " << page_no 
                      << ") too small for validation" << std::endl;
            return false;
        }
        
        // 校验和验证
        uint32_t stored_checksum = dbrecovery::innodb::read_le<uint32_t>(page_data->data(), 24);
        if (stored_checksum != 0 && stored_checksum != 0xFFFFFFFF) {
            std::vector<uint8_t> page_copy = *page_data;
            for (int j = 0; j < 4; ++j) {
                page_copy[24 + j] = 0;
            }
            
            uint32_t computed_crc32c = dbrecovery::innodb::PageReader::compute_crc32c(page_copy);
            if (computed_crc32c != stored_checksum) {
                std::cerr << "[WARN] BLOB page checksum mismatch at (" 
                          << space_id << ", " << page_no << ")" << std::endl;
                return false;
            }
        }
    }
    
    return true;
}

} // namespace innodb
} // namespace dbrecovery
