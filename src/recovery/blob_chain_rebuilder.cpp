#include "dbrecovery/recovery/blob_chain_rebuilder.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <iostream>
#include <set>
#include <algorithm>
#include <fstream>
#include <cstring>

namespace dbrecovery {
namespace recovery {

// ==================== BlobChainRebuilder ====================

BlobChainRebuilder::BlobChainRebuilder() : page_size_(16384) {
    std::memset(&stats_, 0, sizeof(stats_));
}

BlobChainRebuilder::~BlobChainRebuilder() = default;

void BlobChainRebuilder::set_page_size(uint32_t page_size) {
    if (page_size == 4096 || page_size == 8192 ||
        page_size == 16384 || page_size == 32768 || page_size == 65536) {
        page_size_ = page_size;
    }
}

bool BlobChainRebuilder::add_blob_page(uint32_t space_id, uint32_t page_no,
                                        std::vector<uint8_t> page_data) {
    if (page_data.size() < BLOB_PAGE_HEADER_SIZE) {
        return false;
    }

    BlobPage page;
    page.space_id = space_id;
    page.page_no = page_no;
    page.data = std::move(page_data);

    if (!parse_blob_page_header(page.data.data(), page.data.size(), page_no, page)) {
        return false;
    }

    pages_[space_id][page_no] = page;
    stats_.total_pages++;
    stats_.total_bytes += page.data.size();

    return true;
}

std::optional<BlobPointer> BlobChainRebuilder::parse_blob_pointer(const uint8_t* data, size_t len) {
    if (!data || len < 20) {
        return std::nullopt;
    }

    BlobPointer pointer;
    pointer.space_id = dbrecovery::innodb::read_le<uint32_t>(data, 0);
    pointer.page_no = dbrecovery::innodb::read_le<uint32_t>(data, 4);
    pointer.offset = dbrecovery::innodb::read_le<uint32_t>(data, 8);
    pointer.len = dbrecovery::innodb::read_le<uint32_t>(data, 12);
    pointer.total_len = dbrecovery::innodb::read_le<uint32_t>(data, 16);

    if (pointer.total_len == 0 || pointer.page_no == 0xFFFFFFFF) {
        return std::nullopt;
    }

    // Build chain if multi-page
    if (pointer.total_len > pointer.len) {
        // For simplicity, assume single page for now
        // In production, would follow the chain
        pointer.chain_pages.push_back(pointer.page_no);
    }

    return pointer;
}

std::optional<std::vector<uint8_t>> BlobChainRebuilder::reconstruct_from_pointer(
    const BlobPointer& pointer) {
    if (pointer.chain_pages.empty()) {
        return std::nullopt;
    }

    std::vector<uint8_t> result(pointer.total_len, 0);
    size_t offset = 0;

    for (uint32_t page_no : pointer.chain_pages) {
        auto space_it = pages_.find(pointer.space_id);
        if (space_it == pages_.end()) continue;

        auto page_it = space_it->second.find(page_no);
        if (page_it == space_it->second.end()) continue;

        const BlobPage& page = page_it->second;

        // Copy data from page
        size_t copy_size = std::min(pointer.total_len - offset, page.data.size() - BLOB_PAGE_DATA_OFFSET);
        if (copy_size > 0) {
            std::memcpy(result.data() + offset,
                       page.data.data() + BLOB_PAGE_DATA_OFFSET,
                       copy_size);
            offset += copy_size;
        }
    }

    if (offset < pointer.total_len) {
        // Partial reconstruction
        stats_.partial_chains++;
        return std::nullopt;
    }

    stats_.complete_chains++;
    return result;
}

std::optional<std::vector<uint8_t>> BlobChainRebuilder::reconstruct_from_file(
    const std::string& file_path, const BlobPointer& pointer) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Calculate total pages needed
    uint32_t total_pages = (pointer.total_len + page_size_ - BLOB_PAGE_DATA_OFFSET - 1) / 
                           (page_size_ - BLOB_PAGE_DATA_OFFSET);

    std::vector<uint8_t> result(pointer.total_len, 0);
    size_t offset = 0;

    for (uint32_t i = 0; i < total_pages && offset < pointer.total_len; ++i) {
        uint32_t current_page_no = pointer.page_no + i;
        uint64_t page_offset = static_cast<uint64_t>(current_page_no) * page_size_;

        if (page_offset + page_size_ > file_size) {
            break;
        }

        file.seekg(page_offset, std::ios::beg);
        std::vector<uint8_t> page_data(page_size_);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        // Extract BLOB data from page
        size_t copy_size = std::min(pointer.total_len - offset, 
                                     page_size_ - BLOB_PAGE_DATA_OFFSET);
        if (copy_size > 0) {
            std::memcpy(result.data() + offset,
                       page_data.data() + BLOB_PAGE_DATA_OFFSET,
                       copy_size);
            offset += copy_size;
        }
    }

    if (offset < pointer.total_len) {
        stats_.partial_chains++;
        return std::nullopt;
    }

    stats_.complete_chains++;
    return result;
}

std::optional<BlobChain> BlobChainRebuilder::build_chain(uint64_t blob_id) {
    // For now, return empty chain
    // In production, would build chain from pages
    BlobChain chain;
    chain.blob_id = blob_id;
    chain.is_complete = false;
    return std::nullopt;
}

bool BlobChainRebuilder::has_circular_reference(const std::vector<uint32_t>& page_chain) {
    std::set<uint32_t> visited;
    for (uint32_t page_no : page_chain) {
        if (visited.find(page_no) != visited.end()) {
            return true;
        }
        visited.insert(page_no);
    }
    return false;
}

const std::map<uint64_t, std::vector<uint8_t>>& BlobChainRebuilder::get_reconstructed_blobs() const {
    return reconstructed_blobs_;
}

BlobChainRebuilder::ChainStats BlobChainRebuilder::get_stats() const {
    return stats_;
}

void BlobChainRebuilder::clear() {
    pages_.clear();
    reconstructed_blobs_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

bool BlobChainRebuilder::parse_blob_page_header(const uint8_t* data, size_t size,
                                                 uint32_t page_no, BlobPage& page) {
    if (size < BLOB_PAGE_HEADER_SIZE + 8) {
        return false;
    }

    // Read next page in chain (offset 38)
    page.next_page_no = dbrecovery::innodb::read_le<uint32_t>(data, 38);

    // Read total length (offset 42)
    page.total_length = dbrecovery::innodb::read_le<uint32_t>(data, 42);

    // Read current length (offset 46)
    page.current_length = dbrecovery::innodb::read_le<uint32_t>(data, 46);

    // Determine if first/last page
    page.is_first_page = (page_no == page.page_no);  // Simplified
    page.is_last_page = (page.next_page_no == 0xFFFFFFFF);

    return true;
}

std::vector<uint8_t> BlobChainRebuilder::extract_blob_data(const BlobPage& page) {
    if (page.data.size() <= BLOB_PAGE_DATA_OFFSET) {
        return {};
    }

    size_t data_size = std::min(page.current_length, 
                                 static_cast<uint32_t>(page.data.size() - BLOB_PAGE_DATA_OFFSET));
    return std::vector<uint8_t>(page.data.data() + BLOB_PAGE_DATA_OFFSET,
                                page.data.data() + BLOB_PAGE_DATA_OFFSET + data_size);
}

bool BlobChainRebuilder::follow_chain(uint32_t first_page_no, uint32_t space_id,
                                       uint32_t expected_total_len, std::vector<BlobPage>& chain) {
    std::set<uint32_t> visited;
    uint32_t current_page_no = first_page_no;
    uint32_t total_len = 0;

    while (current_page_no != 0xFFFFFFFF) {
        if (visited.find(current_page_no) != visited.end()) {
            return false;  // Circular reference
        }
        visited.insert(current_page_no);

        auto space_it = pages_.find(space_id);
        if (space_it == pages_.end()) break;

        auto page_it = space_it->second.find(current_page_no);
        if (page_it == space_it->second.end()) break;

        const BlobPage& page = page_it->second;
        chain.push_back(page);
        total_len += page.current_length;

        if (total_len >= expected_total_len) break;
        current_page_no = page.next_page_no;
    }

    return true;
}

// ==================== BlobPageParser ====================

BlobPageParser::BlobPageParser() = default;

BlobPageParser::~BlobPageParser() = default;

std::optional<BlobPage> BlobPageParser::parse_page(uint32_t space_id, uint32_t page_no,
                                                    const uint8_t* data, size_t size) {
    if (!data || size < BLOB_PAGE_HEADER_SIZE) {
        return std::nullopt;
    }

    BlobPage page;
    page.space_id = space_id;
    page.page_no = page_no;
    page.data = std::vector<uint8_t>(data, data + size);

    // Parse header
    if (size >= BLOB_PAGE_HEADER_SIZE + 8) {
        page.next_page_no = dbrecovery::innodb::read_le<uint32_t>(data, BLOB_PAGE_HEADER_SIZE);
        page.total_length = dbrecovery::innodb::read_le<uint32_t>(data, BLOB_PAGE_HEADER_SIZE + 4);
        page.current_length = dbrecovery::innodb::read_le<uint32_t>(data, BLOB_PAGE_HEADER_SIZE + 8);
        page.is_first_page = true;  // Simplified
        page.is_last_page = (page.next_page_no == 0xFFFFFFFF);
    }

    return page;
}

std::optional<uint32_t> BlobPageParser::get_next_page(const uint8_t* data, size_t size) {
    if (!data || size < BLOB_PAGE_HEADER_SIZE + 4) {
        return std::nullopt;
    }

    uint32_t next_page = dbrecovery::innodb::read_le<uint32_t>(data, BLOB_PAGE_HEADER_SIZE);
    if (next_page == 0xFFFFFFFF) {
        return std::nullopt;
    }

    return next_page;
}

uint32_t BlobPageParser::get_data_length(const uint8_t* data, size_t size) {
    if (!data || size < BLOB_PAGE_HEADER_SIZE + 8) {
        return 0;
    }

    return dbrecovery::innodb::read_le<uint32_t>(data, BLOB_PAGE_HEADER_SIZE + 8);
}

std::vector<uint8_t> BlobPageParser::extract_data(const uint8_t* data, size_t size) {
    if (!data || size <= BLOB_PAGE_DATA_OFFSET) {
        return {};
    }

    uint32_t data_len = get_data_length(data, size);
    size_t copy_size = std::min(static_cast<uint32_t>(size - BLOB_PAGE_DATA_OFFSET), data_len);

    return std::vector<uint8_t>(data + BLOB_PAGE_DATA_OFFSET,
                                data + BLOB_PAGE_DATA_OFFSET + copy_size);
}

// ==================== BlobRecoveryEngine ====================

BlobRecoveryEngine::BlobRecoveryEngine() : page_size_(16384) {}

BlobRecoveryEngine::~BlobRecoveryEngine() = default;

void BlobRecoveryEngine::set_page_size(uint32_t page_size) {
    if (page_size == 4096 || page_size == 8192 ||
        page_size == 16384 || page_size == 32768 || page_size == 65536) {
        page_size_ = page_size;
    }
    chain_rebuilder_.set_page_size(page_size);
}

bool BlobRecoveryEngine::scan_file(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return false;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);

    std::vector<uint8_t> page_data(page_size_);

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(static_cast<uint64_t>(page_no) * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        // Check if this is a BLOB page
        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 16);
        if (page_type == dbrecovery::innodb::FIL_PAGE_TYPE_BLOB) {
            // Extract BLOB page info
            uint32_t space_id = dbrecovery::innodb::read_le<uint32_t>(page_data.data(), 16);
            uint32_t page_offset = dbrecovery::innodb::read_le<uint32_t>(page_data.data(), 20);

            chain_rebuilder_.add_blob_page(space_id, page_no, std::move(page_data));
        }
    }

    return true;
}

bool BlobRecoveryEngine::scan_directory(const std::string& dir_path) {
    // Simplified - would recursively scan directory for .ibd files
    // and call scan_file for each
    return false;
}

bool BlobRecoveryEngine::reconstruct_all() {
    // Reconstruct all BLOBs from collected pages
    // For now, just return true
    return true;
}

const std::map<uint64_t, std::vector<uint8_t>>& BlobRecoveryEngine::get_blobs() const {
    return chain_rebuilder_.get_reconstructed_blobs();
}

bool BlobRecoveryEngine::export_blob(uint64_t blob_id, const std::string& output_path) {
    auto it = chain_rebuilder_.get_reconstructed_blobs().find(blob_id);
    if (it == chain_rebuilder_.get_reconstructed_blobs().end()) {
        return false;
    }

    std::ofstream file(output_path, std::ios::binary);
    if (!file) {
        return false;
    }

    file.write(reinterpret_cast<const char*>(it->second.data()), it->second.size());
    return true;
}

BlobRecoveryEngine::ChainStats BlobRecoveryEngine::get_stats() const {
    BlobRecoveryEngine::ChainStats stats;
    auto builder_stats = chain_rebuilder_.get_stats();
    stats.total_chains = builder_stats.total_chains;
    stats.complete_chains = builder_stats.complete_chains;
    stats.partial_chains = builder_stats.partial_chains;
    stats.failed_chains = builder_stats.failed_chains;
    stats.total_pages = builder_stats.total_pages;
    stats.total_bytes = builder_stats.total_bytes;
    return stats;
}

void BlobRecoveryEngine::clear() {
    chain_rebuilder_.clear();
}

} // namespace recovery
} // namespace dbrecovery
