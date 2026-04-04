#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <algorithm>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// ==================== UndoLogParser ====================

UndoLogParser::UndoLogParser() : page_size_(16384), mode_(ParseMode::PHYSICAL_FLASHBACK) {
    std::memset(&stats_, 0, sizeof(stats_));
}

UndoLogParser::~UndoLogParser() = default;

void UndoLogParser::set_page_size(uint32_t page_size) {
    if (page_size == 4096 || page_size == 8192 ||
        page_size == 16384 || page_size == 32768 || page_size == 65536) {
        page_size_ = page_size;
    }
}

void UndoLogParser::set_mode(ParseMode mode) {
    mode_ = mode;
}

std::optional<UndoLogPage> UndoLogParser::parse_page(const uint8_t* data, size_t size, uint32_t page_no) {
    if (!data || size < 38) {
        return std::nullopt;
    }

    UndoLogPage page;
    page.page_no = page_no;

    if (!parse_page_header(data, size, page)) {
        return std::nullopt;
    }

    // Parse records - skip FIL header (38 bytes) + undo log page header (40 bytes)
    // Start at offset 78 (38 + 40)
    size_t pos = 78;
    
    // For undo log pages, heap_top indicates where the next record would be placed
    // heap_top is at offset 60 (38 + 22) in the page header
    // Read it from the page header to determine where records end
    size_t heap_top = dbrecovery::innodb::read_le<uint16_t>(data, 60);
    if (heap_top == 0 || heap_top < pos) {
        heap_top = size;  // Fallback if heap_top is invalid
    }
    
    // Parse records until we reach heap_top
    while (pos + 20 <= heap_top && pos + 20 <= size) {
        auto record = parse_record(data, size, pos);
        if (record.has_value()) {
            page.records.push_back(record.value());
            stats_.total_records++;
        } else {
            break;
        }
    }

    page.used_space = static_cast<uint32_t>(pos);
    page.free_space = static_cast<uint32_t>(size - pos);

    pages_.push_back(page);
    stats_.total_pages++;

    return page;
}

std::vector<UndoLogPage> UndoLogParser::parse_file(const std::string& path) {
    std::vector<UndoLogPage> result;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "[ERROR] Failed to open undo log file: " << path << std::endl;
        return result;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 38) {
        std::cerr << "[ERROR] Undo log file too small" << std::endl;
        return result;
    }

    // Read entire file
    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    file.close();

    // Parse pages
    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        uint64_t page_offset = static_cast<uint64_t>(page_no) * page_size_;
        const uint8_t* page_data = buffer.data() + page_offset;

        auto page = parse_page(page_data, page_size_, page_no);
        if (page.has_value()) {
            result.push_back(page.value());
        }
    }

    return result;
}

std::vector<UndoLogRecord> UndoLogParser::extract_old_versions(
    const std::vector<UndoLogPage>& pages,
    uint64_t table_id) {
    std::vector<UndoLogRecord> old_versions;

    for (const auto& page : pages) {
        for (const auto& record : page.records) {
            // Filter by table ID if specified
            if (table_id == 0 || (record.u_data.size() >= 8 &&
                dbrecovery::innodb::read_le<uint64_t>(record.u_data.data(), 0) == table_id)) {
                old_versions.push_back(record);
            }
        }
    }

    return old_versions;
}

std::optional<RollbackInfo> UndoLogParser::build_rollback_info(uint64_t trx_id,
                                                                  const std::vector<UndoLogPage>& pages) {
    RollbackInfo info;
    info.trx_id = trx_id;
    info.is_committed = false;
    info.is_rolled_back = false;
    info.total_records = 0;

    for (const auto& page : pages) {
        if (page.trx_id == trx_id) {
            info.undo_records.insert(info.undo_records.end(),
                                     page.records.begin(), page.records.end());
        }
    }

    if (info.undo_records.empty()) {
        return std::nullopt;
    }

    info.total_records = static_cast<uint32_t>(info.undo_records.size());
    return info;
}

const std::vector<UndoLogPage>& UndoLogParser::get_pages() const {
    return pages_;
}

std::optional<UndoLogChain> UndoLogParser::get_chain(uint64_t trx_id) {
    auto it = chains_.find(trx_id);
    if (it == chains_.end()) {
        return std::nullopt;
    }

    UndoLogChain chain;
    chain.trx_id = trx_id;
    chain.pages = it->second;
    chain.total_records = 0;
    chain.is_complete = true;

    for (const auto& page : chain.pages) {
        chain.total_records += static_cast<uint32_t>(page.records.size());
    }

    return chain;
}

UndoLogParser::Stats UndoLogParser::get_stats() const {
    return stats_;
}

void UndoLogParser::clear() {
    pages_.clear();
    chains_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

// Public parse_record for testing
std::optional<UndoLogRecord> UndoLogParser::parse_record(const uint8_t* data, size_t size, size_t& pos) {
    return parse_record_impl(data, size, pos);
}

// Private implementation
std::optional<UndoLogRecord> UndoLogParser::parse_record_impl(const uint8_t* data, size_t size, size_t& pos) {
    if (pos + 20 > size) {
        return std::nullopt;
    }

    UndoLogRecord record;
    record.u_page_no = 0;  // Will be set by caller
    record.u_offset = static_cast<uint32_t>(pos);

    // Parse transaction ID
    record.u_trx_id = dbrecovery::innodb::read_le<uint64_t>(data, pos);
    pos += 8;

    // Parse rollback pointer
    record.u_roll_ptr = dbrecovery::innodb::read_le<uint64_t>(data, pos);
    pos += 8;

    // Parse type
    record.u_type = dbrecovery::innodb::read_le<uint16_t>(data, pos);
    pos += 2;

    // Parse data length
    record.u_data_len = dbrecovery::innodb::read_le<uint16_t>(data, pos);
    pos += 2;

    // Parse data
    if (pos + record.u_data_len > size) {
        return std::nullopt;
    }

    record.u_data.assign(data + pos, data + pos + record.u_data_len);
    pos += record.u_data_len;

    return record;
}

bool UndoLogParser::parse_page_header(const uint8_t* data, size_t size, UndoLogPage& page) {
    if (size < 38) {
        return false;
    }

    // Read page type
    page.page_type = dbrecovery::innodb::read_le<uint16_t>(data, 16);

    // Check if it's an undo log page
    if (page.page_type != dbrecovery::innodb::FIL_PAGE_UNDO_LOG) {
        return false;
    }

    // Read page header (offset 38)
    if (size < 78) {
        return false;
    }

    const uint8_t* page_header = data + 38;

    // Read transaction ID (8 bytes at offset 38)
    page.trx_id = dbrecovery::innodb::read_le<uint64_t>(page_header, 0);
    // Read rollback pointer (8 bytes at offset 46)
    page.roll_ptr = dbrecovery::innodb::read_le<uint64_t>(page_header, 8);
    // Read page type (2 bytes at offset 54)
    // Read flags (2 bytes at offset 56)
    // Read next record offset (2 bytes at offset 58)
    // Read heap top (2 bytes at offset 60)
    // Read number of records (2 bytes at offset 62)
    // Read free pointer (2 bytes at offset 64)
    // Read free size (2 bytes at offset 66)
    page.free_space = dbrecovery::innodb::read_le<uint16_t>(page_header, 28);  // offset 66

    // Determine if first/last page
    page.is_first_page = (page.page_no == 0);
    page.is_last_page = (page.free_space > page_size_ - 100);  // Simplified

    return true;
}

bool UndoLogParser::follow_chain(uint32_t first_page_no, uint64_t trx_id,
                                  std::vector<UndoLogPage>& chain) {
    std::set<uint32_t> visited;
    uint32_t current_page_no = first_page_no;

    while (current_page_no != 0xFFFFFFFF) {
        if (visited.find(current_page_no) != visited.end()) {
            return false;  // Circular reference
        }
        visited.insert(current_page_no);

        auto it = std::find_if(pages_.begin(), pages_.end(),
            [current_page_no](const UndoLogPage& page) {
                return page.page_no == current_page_no;
            });

        if (it == pages_.end()) {
            break;
        }

        if (it->trx_id != trx_id) {
            break;
        }

        chain.push_back(*it);
        current_page_no++;
    }

    return true;
}

// ==================== UndoLogReplayer ====================

UndoLogReplayer::UndoLogReplayer() : page_size_(16384) {
    std::memset(&stats_, 0, sizeof(stats_));
}

UndoLogReplayer::~UndoLogReplayer() = default;

void UndoLogReplayer::set_page_size(uint32_t page_size) {
    if (page_size == 4096 || page_size == 8192 ||
        page_size == 16384 || page_size == 32768 || page_size == 65536) {
        page_size_ = page_size;
    }
}

bool UndoLogReplayer::add_page(uint32_t page_no, std::vector<uint8_t> page_data) {
    if (page_data.size() < 38) {
        return false;
    }

    UndoLogPage page;
    page.page_no = page_no;
    page.page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 16);

    if (page.page_type != dbrecovery::innodb::FIL_PAGE_UNDO_LOG) {
        return false;
    }

    // Parse records
    size_t pos = 38;
    while (pos + 20 <= page_data.size()) {
        UndoLogRecord record;
        record.u_page_no = page_no;
        record.u_offset = static_cast<uint32_t>(pos);

        record.u_trx_id = dbrecovery::innodb::read_le<uint64_t>(page_data.data(), pos);
        pos += 8;
        record.u_roll_ptr = dbrecovery::innodb::read_le<uint64_t>(page_data.data(), pos);
        pos += 8;
        record.u_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), pos);
        pos += 2;
        record.u_data_len = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), pos);
        pos += 2;

        if (pos + record.u_data_len > page_data.size()) {
            break;
        }

        record.u_data.assign(page_data.data() + pos, page_data.data() + pos + record.u_data_len);
        pos += record.u_data_len;

        page.records.push_back(record);
        stats_.total_records++;
    }

    pages_.push_back(page);
    return true;
}

std::vector<std::map<std::string, std::string>> UndoLogReplayer::replay(
    const std::string& table_name,
    const std::vector<UndoLogRecord>& undo_records) {
    std::vector<std::map<std::string, std::string>> result;

    for (const auto& record : undo_records) {
        // For flashback, we need to reconstruct the old row
        // This is a simplified implementation
        std::map<std::string, std::string> row;
        row["__undo_trx_id"] = std::to_string(record.u_trx_id);
        row["__undo_type"] = std::to_string(record.u_type);
        row["__undo_data_len"] = std::to_string(record.u_data_len);

        // Extract simple values from data
        if (record.u_data.size() >= 8) {
            uint64_t row_id = dbrecovery::innodb::read_le<uint64_t>(record.u_data.data(), 0);
            row["__row_id"] = std::to_string(row_id);
        }

        if (!record.u_data.empty()) {
            row["__data"] = std::string(record.u_data.begin(), record.u_data.end());
        }

        result.push_back(row);
        stats_.replayed_records++;
    }

    return result;
}

UndoLogReplayer::ReplayStats UndoLogReplayer::get_stats() const {
    return stats_;
}

// ==================== UndoLogChainBuilder ====================

UndoLogChainBuilder::UndoLogChainBuilder() : page_size_(16384) {
    std::memset(&stats_, 0, sizeof(stats_));
}

UndoLogChainBuilder::~UndoLogChainBuilder() = default;

bool UndoLogChainBuilder::add_page(uint32_t page_no, uint64_t trx_id, std::vector<uint8_t> page_data) {
    if (page_data.size() < 38) {
        return false;
    }

    UndoLogPage page;
    page.page_no = page_no;
    page.page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 16);
    page.trx_id = trx_id;

    // Parse records
    size_t pos = 38;
    while (pos + 20 <= page_data.size()) {
        UndoLogRecord record;
        record.u_page_no = page_no;
        record.u_offset = static_cast<uint32_t>(pos);

        record.u_trx_id = dbrecovery::innodb::read_le<uint64_t>(page_data.data(), pos);
        pos += 8;
        record.u_roll_ptr = dbrecovery::innodb::read_le<uint64_t>(page_data.data(), pos);
        pos += 8;
        record.u_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), pos);
        pos += 2;
        record.u_data_len = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), pos);
        pos += 2;

        if (pos + record.u_data_len > page_data.size()) {
            break;
        }

        record.u_data.assign(page_data.data() + pos, page_data.data() + pos + record.u_data_len);
        pos += record.u_data_len;

        page.records.push_back(record);
    }

    pages_[page_no] = page;
    stats_.total_pages++;

    return true;
}

bool UndoLogChainBuilder::build_chains() {
    if (pages_.empty()) {
        return false;
    }

    // Group pages by transaction ID
    std::map<uint64_t, std::vector<UndoLogPage>> trx_pages;

    for (const auto& [page_no, page] : pages_) {
        trx_pages[page.trx_id].push_back(page);
    }

    // Build chains
    for (auto& [trx_id, pages] : trx_pages) {
        // Sort pages by page number
        std::sort(pages.begin(), pages.end(),
            [](const UndoLogPage& a, const UndoLogPage& b) {
                return a.page_no < b.page_no;
            });

        chains_[trx_id] = pages;
        stats_.total_chains++;

        // Check if chain is complete
        bool is_complete = true;
        for (size_t i = 0; i < pages.size() - 1; ++i) {
            if (pages[i].page_no + 1 != pages[i + 1].page_no) {
                is_complete = false;
                break;
            }
        }

        if (is_complete) {
            stats_.complete_chains++;
        } else {
            stats_.partial_chains++;
        }
    }

    return true;
}

const std::map<uint64_t, std::vector<UndoLogPage>>& UndoLogChainBuilder::get_chains() const {
    return chains_;
}

std::optional<std::vector<UndoLogPage>> UndoLogChainBuilder::get_chain(uint64_t trx_id) {
    auto it = chains_.find(trx_id);
    if (it == chains_.end()) {
        return std::nullopt;
    }
    return it->second;
}

UndoLogChainBuilder::ChainStats UndoLogChainBuilder::get_stats() const {
    return stats_;
}

void UndoLogChainBuilder::clear() {
    pages_.clear();
    chains_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

} // namespace innodb
} // namespace dbrecovery
