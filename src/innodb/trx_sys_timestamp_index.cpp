/**
 * @file trx_sys_timestamp_index.cpp
 * @brief TrxSysTimestampIndex implementation
 *
 * Reads the TRX_SYS page (page 5) from ibdata1, iterates rollback segment
 * slots, and extracts trx_id → commit_lsn mappings from undo segment headers.
 */

#include "dbrecovery/innodb/trx_sys_timestamp_index.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <cstring>
#include <chrono>
#include <set>
#include <filesystem>

namespace dbrecovery {
namespace innodb {

// ==================== Constants ====================

// TRX_SYS page is always page 5 in ibdata1
constexpr uint32_t TRX_SYS_PAGE_NO = 5;

// Rollback segment array on the TRX_SYS page
constexpr uint32_t TRX_SYS_RSEG_ARRAY = 108;
constexpr uint32_t TRX_SYS_RSEG_SLOT_SIZE = 8;
constexpr uint32_t TRX_SYS_N_RSEGS = 128;

// Undo segment header: starts at FIL_PAGE_DATA + TRX_UNDO_SEG_HDR
// From MySQL trx0undo.h: TRX_UNDO_SEG_HDR = 32 (offset from FIL_PAGE_DATA)
// FIL_PAGE_DATA = 38 (offset from page start)
constexpr uint32_t UNDO_SEG_HDR_PAGE_OFFSET = 38 + 32;  // = 70

// Offsets within undo segment header (MySQL 8.0+ layout):
//   TRX_UNDO_STATE       = 0  (2 bytes)
//   TRX_UNDO_LAST_COMMIT = 4  (8 bytes, LE) — MySQL 8.0+
//   TRX_UNDO_TRX_ID      = 12 (6 bytes, LE) — MySQL 8.0+
//
// MySQL 5.6/5.7 layout (swapped):
//   TRX_UNDO_TRX_ID      = 4  (6 bytes, LE)
//   TRX_UNDO_LAST_COMMIT = 12 (8 bytes, LE)
constexpr uint32_t TRX_UNDO_LAST_COMMIT_OFFSET_80 = 4;
constexpr uint32_t TRX_UNDO_TRX_ID_OFFSET_80 = 12;
constexpr uint32_t TRX_UNDO_TRX_ID_OFFSET_56 = 4;
constexpr uint32_t TRX_UNDO_LAST_COMMIT_OFFSET_56 = 12;

// Maximum trx_id value that fits in 6 bytes
constexpr uint64_t MAX_6BYTE_TRX_ID = 0xFFFFFFFFFFFFULL;

// ==================== Build ====================

bool TrxSysTimestampIndex::build(const std::string& data_file_path, size_t page_size) {
    auto start = std::chrono::high_resolution_clock::now();
    reset();

    if (!std::filesystem::exists(data_file_path)) {
        std::cerr << "[TrxSysTimestampIndex] File not found: " << data_file_path << std::endl;
        return false;
    }

    // Open the data file
    std::ifstream file(data_file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[TrxSysTimestampIndex] Cannot open: " << data_file_path << std::endl;
        return false;
    }

    // Calculate file size and page count
    const auto end_pos = file.tellg();
    if (end_pos < 0) {
        file.close();
        return false;
    }

    const uint64_t file_size = static_cast<uint64_t>(end_pos);
    const uint64_t page_count = file_size / page_size;
    if (page_count <= TRX_SYS_PAGE_NO) {
        std::cerr << "[TrxSysTimestampIndex] File too small: " << page_count << " pages" << std::endl;
        file.close();
        return false;
    }

    stats_.total_pages_scanned = page_count;

    // Read TRX_SYS page
    std::vector<uint8_t> trx_sys_page(page_size);
    file.seekg(static_cast<std::streamoff>(TRX_SYS_PAGE_NO * page_size), std::ios::beg);
    file.read(reinterpret_cast<char*>(trx_sys_page.data()),
              static_cast<std::streamsize>(page_size));
    if (file.gcount() != static_cast<std::streamsize>(page_size)) {
        file.close();
        return false;
    }

    file.close();

    // Build from the buffer
    return build_from_buffer(trx_sys_page, data_file_path, page_size);
}

bool TrxSysTimestampIndex::build_from_buffer(
    const std::vector<uint8_t>& trx_sys_page,
    const std::string& data_file_path,
    size_t page_size) {

    auto start = std::chrono::high_resolution_clock::now();
    reset();

    if (trx_sys_page.empty()) {
        return false;
    }

    const uint8_t* page_data = trx_sys_page.data();
    size_t page_data_size = trx_sys_page.size();

    // Iterate all 128 rseg slots
    for (uint32_t slot_id = 0; slot_id < TRX_SYS_N_RSEGS; slot_id++) {
        uint32_t slot_offset = TRX_SYS_RSEG_ARRAY + slot_id * TRX_SYS_RSEG_SLOT_SIZE;

        if (slot_offset + TRX_SYS_RSEG_SLOT_SIZE > page_data_size) {
            break;
        }

        // Read rseg slot: space_id (4 bytes LE) + page_no (4 bytes LE)
        uint32_t space_id = read_le<uint32_t>(page_data, slot_offset);
        uint32_t page_no = read_le<uint32_t>(page_data, slot_offset + 4);

        // Skip empty slots (0:0)
        if (space_id == 0 && page_no == 0) {
            continue;
        }

        stats_.rseg_slots_found++;

        // Read the rseg header page
        std::ifstream file(data_file_path, std::ios::binary);
        if (!file) {
            continue;
        }

        // Only support space_id == 0 (system tablespace) for now
        if (space_id != 0) {
            file.close();
            continue;
        }

        uint64_t page_offset = static_cast<uint64_t>(page_no) * page_size;
        file.seekg(static_cast<std::streamoff>(page_offset), std::ios::beg);
        std::vector<uint8_t> rseg_page(page_size);
        file.read(reinterpret_cast<char*>(rseg_page.data()),
                  static_cast<std::streamsize>(page_size));
        file.close();

        if (file.gcount() != static_cast<std::streamsize>(page_size)) {
            continue;
        }

        // Parse the undo segment header from the rseg header page
        auto entry_opt = read_rseg_undo_seg_header(rseg_page.data(), page_size, slot_id);
        if (entry_opt && entry_opt->is_valid) {
            entries_[entry_opt->trx_id] = *entry_opt;
            stats_.entries_found++;
        }
    }

    built_ = true;

    auto end = std::chrono::high_resolution_clock::now();
    stats_.build_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return true;
}

// ==================== Lookup ====================

std::optional<TrxTimestampEntry> TrxSysTimestampIndex::lookup(uint64_t trx_id) const {
    auto it = entries_.find(trx_id);
    if (it != entries_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::map<uint64_t, TrxTimestampEntry> TrxSysTimestampIndex::lookup_bulk(
    const std::vector<uint64_t>& trx_ids) const {

    std::map<uint64_t, TrxTimestampEntry> results;
    for (auto trx_id : trx_ids) {
        auto it = entries_.find(trx_id);
        if (it != entries_.end()) {
            results[trx_id] = it->second;
        }
    }
    return results;
}

// ==================== Management ====================

void TrxSysTimestampIndex::reset() {
    entries_.clear();
    built_ = false;
    stats_ = BuildStats{};
}

void TrxSysTimestampIndex::log_summary() const {
    std::cerr << "[TrxSysTimestampIndex] " << entries_.size()
              << " trx_id entries from " << stats_.rseg_slots_found
              << " rseg slots (" << stats_.build_time_ms << " ms)"
              << std::endl;
}

// ==================== Private Helpers ====================

std::optional<TrxTimestampEntry> TrxSysTimestampIndex::read_rseg_undo_seg_header(
    const uint8_t* page_data, size_t page_size, uint32_t rseg_id) {

    if (!page_data || page_size < UNDO_SEG_HDR_PAGE_OFFSET + 18) {
        return std::nullopt;
    }

    TrxTimestampEntry entry;
    entry.rseg_id = rseg_id;

    // Try MySQL 8.0+ layout first: trx_id at +12, commit_lsn at +4
    {
        uint32_t trx_id_offset = UNDO_SEG_HDR_PAGE_OFFSET + TRX_UNDO_TRX_ID_OFFSET_80;
        uint32_t lsn_offset = UNDO_SEG_HDR_PAGE_OFFSET + TRX_UNDO_LAST_COMMIT_OFFSET_80;

        if (trx_id_offset + 6 <= page_size) {
            uint64_t trx_id = 0;
            for (int i = 0; i < 6; i++) {
                trx_id |= static_cast<uint64_t>(page_data[trx_id_offset + i]) << (i * 8);
            }

            // Valid trx_id should be > 0 and fit in 6 bytes
            if (trx_id > 0 && trx_id <= MAX_6BYTE_TRX_ID) {
                entry.trx_id = trx_id;
                entry.undo_page_no = static_cast<uint32_t>(trx_id_offset);

                // Read commit_lsn
                if (lsn_offset + 8 <= page_size) {
                    entry.commit_lsn = read_le<uint64_t>(page_data, lsn_offset);
                }

                entry.is_valid = true;
                return entry;
            }
        }
    }

    // Fallback to MySQL 5.6/5.7 layout: trx_id at +4, commit_lsn at +12
    {
        uint32_t trx_id_offset = UNDO_SEG_HDR_PAGE_OFFSET + TRX_UNDO_TRX_ID_OFFSET_56;
        uint32_t lsn_offset = UNDO_SEG_HDR_PAGE_OFFSET + TRX_UNDO_LAST_COMMIT_OFFSET_56;

        if (trx_id_offset + 6 <= page_size) {
            uint64_t trx_id = 0;
            for (int i = 0; i < 6; i++) {
                trx_id |= static_cast<uint64_t>(page_data[trx_id_offset + i]) << (i * 8);
            }

            if (trx_id > 0 && trx_id <= MAX_6BYTE_TRX_ID) {
                entry.trx_id = trx_id;
                entry.undo_page_no = static_cast<uint32_t>(trx_id_offset);

                if (lsn_offset + 8 <= page_size) {
                    entry.commit_lsn = read_le<uint64_t>(page_data, lsn_offset);
                }

                entry.is_valid = true;
                return entry;
            }
        }
    }

    return std::nullopt;
}

} // namespace innodb
} // namespace dbrecovery