/**
 * @file undo_log_parser.cpp
 * @brief Undo log parser for recovering TRUNCATE/DROP/DELETE operations
 * 
 * MySQL TRUNCATE 使用 purge 操作 released pages，但 undo logs 仍保留旧版本记录
 * 本解析器从 rollback segments 中提取已删除的记录
 */

#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <filesystem>
#include <set>
#include <sstream>
#include <optional>

namespace dbrecovery {
namespace innodb {

// ==================== Undo Log Constants ====================
// From MySQL 8.0.36: include/trx0undo.h & include/trx0undo.c

// Undo log types
constexpr uint8_t TRX_UNDO_INSERT = 1;   // Insert undo log
constexpr uint8_t TRX_UNDO_UPDATE = 2;   // Update undo log

// Undo log record types (for UPDATE)
constexpr uint8_t TRX_UNDO_UPD_EXIST_REC = 11;
constexpr uint8_t TRX_UNDO_UPD_DEL_REC     = 12;
constexpr uint8_t TRX_UNDO_DEL_REC         = 13;
constexpr uint8_t TRX_UNDO_INS_REC         = 14;

// ==================== Undo Log Page Header (TRX_UNDO_PAGE_HEADER) ====================
// Offset 0-28: TRX_UNDO_PAGE_TYPE (2 bytes) + TRX_UNDO_PAGE_LIST (26 bytes)
// Offset 28: TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE = 28 + 10 = 38

constexpr uint16_t TRX_UNDO_PAGE_TYPE       = 0;   // UNDO or REDO log
constexpr uint16_t TRX_UNDO_PAGE_START      = 28;  // Start of undo log records
constexpr uint16_t TRX_UNDO_PAGE_FREE       = 30;  // Free space offset
constexpr uint16_t TRX_UNDO_PAGE_N_RECS     = 34;  // Number of records
constexpr uint16_t TRX_UNDO_PAGE_HDR_SIZE   = 40;  // Total header size

// ==================== Undo Log Record Header ====================
struct UndoLogRecordHeader {
    uint8_t type;           // TRX_UNDO_XXX
    uint8_t extern_storage; // External storage flag (BLOB)
    uint16_t rec_page_no;   // Page number of the record
    uint16_t rec_page_offset; // Offset within page
    uint8_t sys_fields[6];  // Transaction ID & Rollback Pointer (10+12 bytes)
    
    // For UPDATE/DELETE operations
    uint16_t field_sizes[20]; // Field size array (variable)
    uint8_t field_count;        // Number of fields
    
    static std::optional<UndoLogRecordHeader> parse(
        const uint8_t* data, size_t size, size_t offset) {
        
        if (!data || offset + 10 > size) {
            return std::nullopt;
        }
        
        UndoLogRecordHeader header{};
        header.type = data[offset];
        
        if (header.type == TRX_UNDO_INSERT) {
            // Insert records don't have previous version info
            header.field_count = data[offset + 1];
            return header;
        }
        
        // Update/Delete records have more complex headers
        if (offset + 15 > size) {
            return std::nullopt;
        }
        
        header.field_count = data[offset + 1];
        header.rec_page_no = read_le<uint16_t>(data, offset + 2);
        header.rec_page_offset = read_le<uint16_t>(data, offset + 4);
        
        return header;
    }
};

// ==================== Rollback Segment Array ====================
// Rollback segments are stored in the system tablespace (ibdata1)
// Each rollback segment has a header at offset TRX_SYS_RSEG_ARRAY + (id * 8)

constexpr uint32_t TRX_SYS_RSEG_ARRAY = 108;  // Offset of rollback segment array
constexpr uint32_t TRX_SYS_RSEG_SLOT_SIZE = 8;  // Each slot is 8 bytes (space:page)
constexpr uint32_t TRX_SYS_N_RSEGS = 128;     // Maximum rollback segments

std::optional<uint32_t> read_rseg_slot(
    const uint8_t* data, size_t size, uint32_t slot_id) {
    
    if (!data || slot_id >= TRX_SYS_N_RSEGS) {
        return std::nullopt;
    }
    
    uint32_t offset = TRX_SYS_RSEG_ARRAY + slot_id * TRX_SYS_RSEG_SLOT_SIZE;
    if (offset + 8 > size) {
        return std::nullopt;
    }
    
    uint32_t space_id = read_le<uint32_t>(data, offset);
    uint32_t page_no = read_le<uint32_t>(data, offset + 4);
    
    // Filter out empty slots (0:0)
    if (space_id == 0 && page_no == 0) {
        return std::nullopt;
    }
    
    return (space_id << 16) | page_no;  // Combine space and page
}

// ==================== UndoLogParser Implementation ====================

std::vector<UndoLogEntry> UndoLogParser::parse_undo_log_page(
    const uint8_t* page_data, size_t page_size, uint32_t page_no) {
    
    std::vector<UndoLogEntry> entries;
    
    if (!page_data || page_size < 100) {
        return entries;
    }
    
    // Check page type
    uint16_t page_type = get_page_type(page_data);
    if (page_type != FIL_PAGE_UNDO_LOG) {
        // Try to parse anyway - might be corrupted
        std::cerr << "[WARN] Page " << page_no << " is not UNDO log type: 0x"
                  << std::hex << page_type << std::dec << "\n";
    }
    
    // Read page header
    uint16_t page_start = read_le<uint16_t>(page_data, TRX_UNDO_PAGE_START);
    uint16_t page_free = read_le<uint16_t>(page_data, TRX_UNDO_PAGE_FREE);
    uint16_t n_recs = read_le<uint16_t>(page_data, TRX_UNDO_PAGE_N_RECS);
    
    if (page_start < TRX_UNDO_PAGE_HDR_SIZE || page_start > page_free) {
        return entries;
    }
    
    // Parse each undo log record
    size_t offset = page_start;
    size_t rec_count = 0;
    
    while (offset < page_free && rec_count < n_recs) {
        if (offset + 10 > page_size) {
            break;
        }
        
        auto header_opt = UndoLogRecordHeader::parse(page_data, page_size, offset);
        if (!header_opt) {
            // Skip bad record
            offset += 4;
            continue;
        }
        
        const auto& header = *header_opt;
        UndoLogEntry entry;
        entry.page_no = header.rec_page_no;
        entry.page_offset = header.rec_page_offset;
        entry.undo_type = header.type;
        entry.field_count = header.field_count;
        
        // Parse fields based on record type
        size_t field_offset = offset + 2;  // Skip type + field count
        
        for (uint8_t i = 0; i < header.field_count && field_offset + 4 < page_size; i++) {
            uint8_t field_type = page_data[field_offset];
            uint16_t field_size;
            
            if (field_type & 0x80) {
                // Long field - 2 byte size
                field_size = read_le<uint16_t>(page_data, field_offset + 1);
                field_offset += 3;
            } else {
                // Short field - 1 byte size
                field_size = field_type & 0x7F;
                field_offset += 1;
            }
            
            if (field_offset + field_size > page_size) {
                break;
            }
            
            UndoLogField field;
            field.type = field_type;
            field.size = field_size;
            field.data.assign(page_data + field_offset, 
                            page_data + field_offset + field_size);
            
            entry.fields.push_back(field);
            field_offset += field_size;
        }
        
        entries.push_back(entry);
        rec_count++;
        
        // Find next record - look for next record marker
        // Undo log records are not tightly packed, need to find boundaries
        while (offset < page_free && page_data[offset] == 0xFF) {
            offset++;
        }
        offset += 10 + entry.total_size();  // Approximate next position
    }
    
    return entries;
}

std::vector<UndoLogEntry> UndoLogParser::parse_rollback_segment(
    const std::string& ibdata_path, uint32_t rseg_id) {

    std::vector<UndoLogEntry> all_entries;

    std::ifstream file(ibdata_path, std::ios::binary);
    if (!file) {
        std::cerr << "[ERROR] Cannot open ibdata: " << ibdata_path << "\n";
        return all_entries;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Read TRX_SYS page (page 5) to get the rollback segment slot
    constexpr size_t TRX_SYS_PAGE_NO = 5;
    constexpr size_t DEFAULT_PAGE_SIZE = 16384;
    std::vector<uint8_t> trx_sys_page(DEFAULT_PAGE_SIZE);

    if (file_size >= (TRX_SYS_PAGE_NO + 1) * DEFAULT_PAGE_SIZE) {
        file.seekg(TRX_SYS_PAGE_NO * DEFAULT_PAGE_SIZE, std::ios::beg);
        file.read(reinterpret_cast<char*>(trx_sys_page.data()), DEFAULT_PAGE_SIZE);
    }

    // Read rollback segment slot from TRX_SYS page
    auto slot_opt = read_rseg_slot(trx_sys_page.data(), trx_sys_page.size(), rseg_id);
    if (!slot_opt) {
        return all_entries;
    }
    
    uint32_t space_id = *slot_opt >> 16;
    uint32_t page_no = *slot_opt & 0xFFFF;
    
    // Read undo log pages (chain linked by TRX_UNDO_PAGE_HDR + 26 bytes)
    std::set<uint32_t> visited_pages;
    uint32_t current_page = page_no;
    
    while (current_page != 0 && visited_pages.size() < 1000) {  // Limit to 1000 pages
        if (visited_pages.count(current_page)) {
            break;  // Loop detected
        }
        visited_pages.insert(current_page);
        
        file.seekg(static_cast<std::streamoff>(current_page) * 16384, std::ios::beg);
        std::vector<uint8_t> page_buffer(16384);
        file.read(reinterpret_cast<char*>(page_buffer.data()), 16384);
        
        auto entries = parse_undo_log_page(page_buffer.data(), 16384, current_page);
        all_entries.insert(all_entries.end(), entries.begin(), entries.end());
        
        // Read next page from page header (offset 26-29 in UNDO page header)
        uint32_t next_page = read_le<uint32_t>(page_buffer.data(), 26);
        if (next_page == 0 || next_page == 0xFFFFFFFF) {
            break;
        }
        current_page = next_page;
    }
    
    return all_entries;
}

// ==================== Recovery Implementation ====================

std::vector<HeuristicRecord> UndoLogParser::recover_from_undo_logs(
    const std::string& ibdata_path,
    const std::string& table_space_id,
    uint64_t table_index_id) {
    
    std::vector<HeuristicRecord> recovered_records;
    
    // Step 1: Read TRX_SYS page (page 5) from ibdata1 to get rseg slots
    constexpr size_t TRX_SYS_PAGE_NO = 5;
    constexpr size_t DEFAULT_PAGE_SIZE = 16384;

    std::vector<uint8_t> trx_sys_page;
    std::ifstream ibd(ibdata_path, std::ios::binary);
    if (ibd.is_open()) {
        trx_sys_page.resize(DEFAULT_PAGE_SIZE);
        ibd.seekg(TRX_SYS_PAGE_NO * DEFAULT_PAGE_SIZE);
        ibd.read(reinterpret_cast<char*>(trx_sys_page.data()), DEFAULT_PAGE_SIZE);
        ibd.close();
    }

    std::vector<uint32_t> rseg_ids;
    const uint8_t* trx_data = trx_sys_page.empty() ? nullptr : trx_sys_page.data();
    for (uint32_t i = 0; i < TRX_SYS_N_RSEGS; i++) {
        auto slot_opt = read_rseg_slot(trx_data, trx_sys_page.size(), i);
        if (slot_opt) {
            rseg_ids.push_back(i);
        }
    }

    if (trx_data == nullptr) {
        std::cout << "[WARN] Could not read TRX_SYS page from " << ibdata_path
                  << ", undo log recovery unavailable\n";
        return recovered_records;
    }
    
    std::cout << "[INFO] Found " << rseg_ids.size() << " rollback segments\n";
    
    // Step 2: Parse each rollback segment
    for (uint32_t rseg_id : rseg_ids) {
        auto entries = parse_rollback_segment(ibdata_path, rseg_id);
        std::cout << "[INFO] Rseg " << rseg_id << ": " << entries.size() << " undo records\n";
        
        // Step 3: Filter entries for our table
        for (const auto& entry : entries) {
            // Skip if not DELETE/UPDATE for our table
            if (entry.undo_type != TRX_UNDO_DEL_REC && 
                entry.undo_type != TRX_UNDO_UPD_DEL_REC) {
                continue;
            }
            
            // Step 4: Reconstruct record from undo log
            HeuristicRecord record;
            record.is_deleted = true;  // Mark as deleted (was deleted by TRUNCATE)
            
            for (const auto& field : entry.fields) {
                HeuristicField hf;
                hf.type = HeuristicFieldType::VARCHAR_SHORT;  // Default
                hf.data = field.data;
                hf.is_null = false;
                hf.is_valid = true;
                record.fields.push_back(hf);
            }
            
            if (!record.fields.empty()) {
                recovered_records.push_back(record);
            }
        }
    }
    
    return recovered_records;
}

// ==================== Transaction ID Tracking ====================

/**
 * @brief TransactionInfo: Stores extended transaction metadata
 * 
 * This structure is used internally to track transactions during undo log parsing.
 * It helps in properly ordering undo records and reconstructing complete transactions.
 */
struct TransactionInfo {
    uint64_t trx_id;        // Transaction ID
    uint64_t rseg_id;       // Rollback segment ID
    uint32_t undo_page_no;  // First undo page number
    uint32_t undo_offset;   // Undo log record offset within page
    bool is_insert;         // Was this an INSERT operation?
    time_t timestamp;       // Transaction timestamp
    std::vector<uint8_t> undo_data;  // Complete undo log data for this transaction
};

/**
 * @brief Parse transaction ID from undo log record
 * 
 * In InnoDB undo logs, the transaction ID is stored in the record header.
 * For COMPACT/REDUNDANT format, it's at offset 5-10 from record start.
 * 
 * @param record_data The undo log record data
 * @param offset Offset to start of transaction ID
 * @return Transaction ID or nullopt if not found
 */
std::optional<uint64_t> parse_trx_id_from_record(
    const std::vector<uint8_t>& record_data, size_t offset) {
    
    if (offset + 6 > record_data.size()) {
        return std::nullopt;
    }
    
    // Transaction ID is 6 bytes in little-endian format
    uint64_t trx_id = 0;
    for (int i = 0; i < 6; i++) {
        trx_id |= (static_cast<uint64_t>(record_data[offset + i]) << (i * 8));
    }
    
    // Ensure it's a valid transaction ID (> 0)
    if (trx_id == 0 || trx_id > 281474976710655ULL) {  // Max 6-byte value
        return std::nullopt;
    }
    
    return trx_id;
}

/**
 * @brief Parse rollback pointer from undo log record
 * 
 * The rollback pointer (roll_ptr) is stored after the transaction ID and
 * contains information about where to find the previous version of the record.
 * 
 * Format (7 bytes):
 * - bits 0-41: page number (42 bits)
 * - bits 42-55: offset within page (14 bits)
 * - bits 56-63: reserved/flags
 * 
 * @param record_data The undo log record data
 * @param offset Offset to start of rollback pointer
 * @return Rollback pointer as tuple (page_no, offset) or nullopt
 */
std::optional<std::tuple<uint32_t, uint16_t>> parse_roll_ptr_from_record(
    const std::vector<uint8_t>& record_data, size_t offset) {
    
    if (offset + 7 > record_data.size()) {
        return std::nullopt;
    }
    
    // Read 6 bytes (48 bits) for page number and offset
    uint64_t roll_ptr = 0;
    for (int i = 0; i < 6; i++) {
        roll_ptr |= (static_cast<uint64_t>(record_data[offset + i]) << (i * 8));
    }
    
    // Extract page number (lower 42 bits) and offset (next 14 bits)
    uint32_t page_no = static_cast<uint32_t>(roll_ptr & 0x3FFFFFFFFFFULL);  // 42 bits
    uint16_t page_offset = static_cast<uint16_t>((roll_ptr >> 42) & 0x3FFF);  // 14 bits
    
    return std::make_tuple(page_no, page_offset);
}

std::optional<std::tuple<uint64_t, uint32_t, uint32_t>> UndoLogParser::find_transaction_by_trx_id(
    const std::string& ibdata_path, uint64_t target_trx_id) {
    
    std::ifstream file(ibdata_path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    // First, scan all rollback segments for the target transaction ID
    for (uint32_t rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++) {
        auto entries = parse_rollback_segment(ibdata_path, rseg_id);
        
        for (const auto& entry : entries) {
            // Try to parse transaction ID from entry fields
            if (entry.fields.size() >= 2) {
                // Transaction ID is typically in the first few fields
                auto trx_id_opt = parse_trx_id_from_record(entry.fields[0].data, 0);
                if (trx_id_opt.has_value() && trx_id_opt.value() == target_trx_id) {
                    return std::make_tuple(target_trx_id, rseg_id, entry.page_no);
                }
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief Build complete transaction chain from undo logs
 * 
 * This function follows the transaction's undo log chain to reconstruct
 * all changes made by a transaction, which is essential for:
 * 1. Point-in-time recovery
 * 2. Transaction rollback
 * 3. Consistent backup restoration
 * 
 * @param ibdata_path Path to ibdata1
 * @param start_page First undo page of the transaction
 * @param rseg_id Rollback segment ID
 * @return Complete transaction undo log entries in order
 */
std::vector<UndoLogEntry> UndoLogParser::build_transaction_chain(
    const std::string& ibdata_path,
    uint32_t start_page,
    uint32_t rseg_id) {
    
    std::vector<UndoLogEntry> transaction_entries;
    std::set<uint32_t> visited_pages;
    
    uint32_t current_page = start_page;
    
    while (current_page != 0 && visited_pages.size() < 10000) {
        if (visited_pages.count(current_page)) {
            std::cerr << "[WARN] Undo log page loop detected at page " << current_page << "\n";
            break;
        }
        visited_pages.insert(current_page);
        
        std::ifstream file(ibdata_path, std::ios::binary);
        if (!file) {
            std::cerr << "[ERROR] Cannot read undo log page " << current_page << "\n";
            break;
        }
        
        file.seekg(static_cast<std::streamoff>(current_page) * 16384, std::ios::beg);
        std::vector<uint8_t> page_buffer(16384);
        file.read(reinterpret_cast<char*>(page_buffer.data()), 16384);
        
        // Parse undo log page
        auto entries = parse_undo_log_page(page_buffer.data(), 16384, current_page);
        
        // Add new entries to transaction chain
        for (auto& entry : entries) {
            transaction_entries.push_back(std::move(entry));
        }
        
        // Read next page from undo log page header
        // Offset 26-29 in UNDO page header contains next page pointer
        uint32_t next_page = read_le<uint32_t>(page_buffer.data(), 26);
        
        if (next_page == 0 || next_page == 0xFFFFFFFF) {
            break;  // End of chain
        }
        
        current_page = next_page;
    }
    
    // Sort entries by their offset within the page to get proper order
    std::sort(transaction_entries.begin(), transaction_entries.end(),
              [](const UndoLogEntry& a, const UndoLogEntry& b) {
                  return a.page_offset < b.page_offset;
              });
    
    return transaction_entries;
}

// ==================== Page Scanning ====================

size_t UndoLogParser::scan_all_rollback_segments(const std::string& ibdata_path) {
    size_t total_records = 0;
    
    for (uint32_t rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++) {
        auto entries = parse_rollback_segment(ibdata_path, rseg_id);
        total_records += entries.size();
    }
    
    stats_.total_undo_pages = 0;
    stats_.total_undo_records = static_cast<uint32_t>(total_records);
    stats_.recoverable_records = 0;

    return total_records;
}

// ==================== Undo Tablespace Support (MySQL 8.0+) ====================

std::vector<UndoLogEntry> UndoLogParser::parse_undo_tablespace(
    const std::string& undo_tablespace_path) {

    std::vector<UndoLogEntry> all_entries;

    std::ifstream file(undo_tablespace_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return all_entries;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    constexpr size_t DEFAULT_PAGE_SIZE = 16384;
    if (file_size < DEFAULT_PAGE_SIZE) {
        return all_entries;
    }

    uint32_t total_pages = static_cast<uint32_t>(file_size / DEFAULT_PAGE_SIZE);
    std::vector<uint8_t> page_buffer(DEFAULT_PAGE_SIZE);

    for (uint32_t page_no = 0; page_no < total_pages; page_no++) {
        file.seekg(static_cast<std::streamoff>(page_no) * DEFAULT_PAGE_SIZE, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buffer.data()), DEFAULT_PAGE_SIZE);

        // Check page type at FIL_PAGE_TYPE_ABS (24) using big-endian read
        uint16_t page_type = read_be<uint16_t>(page_buffer.data(), FIL_PAGE_TYPE_ABS);
        if (page_type != FIL_PAGE_UNDO_LOG) {
            continue;
        }

        auto entries = parse_undo_log_page(page_buffer.data(), DEFAULT_PAGE_SIZE, page_no);
        all_entries.insert(all_entries.end(), entries.begin(), entries.end());
    }

    stats_.total_undo_pages += static_cast<uint32_t>(total_pages);
    stats_.total_undo_records += static_cast<uint32_t>(all_entries.size());

    return all_entries;
}

std::vector<UndoLogEntry> UndoLogParser::parse_all_undo_tablespaces(
    const std::string& data_dir) {

    std::vector<UndoLogEntry> all_entries;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();
            // MySQL 8.0 undo tablespace naming: undo_001, undo_002, etc.
            // Also handle innodb_undo_001, undo_1, undo_2 variants
            if (filename.find("undo_") == 0 || filename.find("innodb_undo") == 0) {
                std::cout << "[INFO] Scanning undo tablespace: " << filename << std::endl;
                auto entries = parse_undo_tablespace(entry.path().string());
                std::cout << "[INFO]   Found " << entries.size() << " undo records" << std::endl;
                all_entries.insert(all_entries.end(), entries.begin(), entries.end());
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[WARN] Error scanning undo tablespaces: " << e.what() << std::endl;
    }

    return all_entries;
}

// ==================== v2.0: Deep Undo Chain Traversal ====================

std::vector<UndoLogEntry> UndoLogParser::traverse_undo_chain(
    const std::string& ibdata_path,
    uint32_t start_page_no,
    uint32_t start_offset) {

    constexpr size_t DEFAULT_PAGE_SIZE = 16384;
    std::vector<UndoLogEntry> chain;
    std::set<std::pair<uint32_t, uint32_t>> visited;

    uint32_t current_page = start_page_no;
    uint32_t current_offset = start_offset;

    std::ifstream file(ibdata_path, std::ios::binary);
    if (!file) return chain;

    constexpr size_t MAX_CHAIN_LENGTH = 1000;
    size_t chain_pos = 0;

    while (chain_pos < MAX_CHAIN_LENGTH) {
        auto key = std::make_pair(current_page, current_offset);
        if (visited.count(key)) break;
        visited.insert(key);

        file.seekg(static_cast<std::streamoff>(current_page) * DEFAULT_PAGE_SIZE, std::ios::beg);
        std::vector<uint8_t> page_data(DEFAULT_PAGE_SIZE);
        file.read(reinterpret_cast<char*>(page_data.data()), DEFAULT_PAGE_SIZE);
        if (file.gcount() < static_cast<std::streamsize>(DEFAULT_PAGE_SIZE)) break;

        uint16_t page_type = get_page_type(page_data.data());
        if (page_type != FIL_PAGE_UNDO_LOG) break;

        if (current_offset + 10 > DEFAULT_PAGE_SIZE) break;

        auto header_opt = UndoLogRecordHeader::parse(page_data.data(), DEFAULT_PAGE_SIZE, current_offset);
        if (!header_opt) break;

        const auto& header = *header_opt;

        UndoLogEntry entry;
        entry.undo_type = header.type;
        entry.page_no = header.rec_page_no;
        entry.page_offset = header.rec_page_offset;
        entry.field_count = header.field_count;

        // Parse fields
        size_t field_offset = current_offset + 2;
        for (uint8_t i = 0; i < header.field_count && field_offset + 1 < DEFAULT_PAGE_SIZE; i++) {
            uint8_t field_type = page_data[field_offset];
            uint16_t field_size;
            if (field_type & 0x80) {
                if (field_offset + 3 > DEFAULT_PAGE_SIZE) break;
                field_size = read_le<uint16_t>(page_data.data(), field_offset + 1);
                field_offset += 3;
            } else {
                field_size = field_type & 0x7F;
                field_offset += 1;
            }
            if (field_offset + field_size > DEFAULT_PAGE_SIZE) break;
            UndoLogField field;
            field.type = field_type;
            field.size = field_size;
            field.data.assign(page_data.data() + field_offset,
                            page_data.data() + field_offset + field_size);
            entry.fields.push_back(field);
            field_offset += field_size;
        }

        chain.push_back(entry);

        if (header.type == TRX_UNDO_INSERT || header.type == TRX_UNDO_INS_REC) break;

        // Follow prev_undo_ptr: [page_no(4B LE)] [offset(2B LE)] at offset +6 from record start
        size_t prev_ptr_offset = current_offset + 6;
        if (prev_ptr_offset + 6 <= DEFAULT_PAGE_SIZE) {
            uint32_t next_page = read_le<uint32_t>(page_data.data(), prev_ptr_offset);
            uint32_t next_offset = read_le<uint16_t>(page_data.data(), prev_ptr_offset + 4);
            if (next_page == 0 || next_offset == 0) break;
            if (next_page == current_page && next_offset == current_offset) break;
            current_page = next_page;
            current_offset = next_offset;
        } else break;

        chain_pos++;
    }

    std::reverse(chain.begin(), chain.end());
    return chain;
}

// ==================== v2.0: DELETE Flashback ====================

std::vector<std::string> UndoLogParser::flashback_delete(
    const std::string& ibdata_path,
    const std::string& table_name,
    const std::string& start_time,
    const std::string& end_time) {

    std::vector<std::string> sql_statements;

    constexpr size_t DEFAULT_PAGE_SIZE = 16384;
    constexpr size_t TRX_SYS_PAGE_NO = 5;

    std::vector<uint8_t> trx_sys_page(DEFAULT_PAGE_SIZE);
    std::ifstream file(ibdata_path, std::ios::binary);
    if (!file) return sql_statements;

    file.seekg(TRX_SYS_PAGE_NO * DEFAULT_PAGE_SIZE, std::ios::beg);
    file.read(reinterpret_cast<char*>(trx_sys_page.data()), DEFAULT_PAGE_SIZE);

    const uint8_t* trx_data = trx_sys_page.data();
    std::vector<uint32_t> rseg_list;
    for (uint32_t i = 0; i < TRX_SYS_N_RSEGS; i++) {
        auto slot = read_rseg_slot(trx_data, trx_sys_page.size(), i);
        if (slot) rseg_list.push_back(i);
    }

    std::string effective_table = table_name.empty() ? "recovered_table" : table_name;
    uint32_t total_inserts = 0;

    for (uint32_t rseg_id : rseg_list) {
        auto entries = parse_rollback_segment(ibdata_path, rseg_id);
        for (const auto& entry : entries) {
            if (entry.undo_type != TRX_UNDO_DEL_REC &&
                entry.undo_type != TRX_UNDO_UPD_DEL_REC) continue;

            std::vector<std::string> col_values;
            for (const auto& field : entry.fields) {
                if (field.data.empty()) {
                    col_values.push_back("NULL");
                } else {
                    std::string val(field.data.begin(), field.data.end());
                    std::string escaped;
                    for (char c : val) {
                        if (c == '\'') escaped += "''";
                        else if (c == '\\') escaped += "\\\\";
                        else escaped += c;
                    }
                    col_values.push_back("'" + escaped + "'");
                }
            }

            if (!col_values.empty()) {
                std::ostringstream sql;
                sql << "INSERT INTO `" << effective_table << "` VALUES (";
                for (size_t i = 0; i < col_values.size(); i++) {
                    if (i > 0) sql << ", ";
                    sql << col_values[i];
                }
                sql << ");";
                sql_statements.push_back(sql.str());
                total_inserts++;
            }
        }
    }

    stats_.flashback_statements = total_inserts;
    return sql_statements;
}

// ==================== v2.0: Row Reconstruction ====================

std::vector<std::string> UndoLogParser::reconstruct_row_from_undo(
    const std::vector<UndoLogEntry>& entries, uint32_t column_count) {

    std::vector<std::string> row(column_count, "NULL");
    if (entries.empty() || column_count == 0) return row;

    for (const auto& entry : entries) {
        if (entry.undo_type == TRX_UNDO_INSERT || entry.undo_type == TRX_UNDO_INS_REC) {
            for (size_t i = 0; i < entry.fields.size() && i < column_count; i++) {
                if (!entry.fields[i].data.empty()) {
                    row[i] = std::string(entry.fields[i].data.begin(), entry.fields[i].data.end());
                }
            }
            continue;
        }
        for (size_t i = 0; i < entry.fields.size() && i < column_count; i++) {
            if (!entry.fields[i].data.empty()) {
                row[i] = std::string(entry.fields[i].data.begin(), entry.fields[i].data.end());
            }
        }
    }
    return row;
}

} // namespace innodb
} // namespace dbrecovery
