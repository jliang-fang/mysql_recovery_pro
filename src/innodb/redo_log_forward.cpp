#include "dbrecovery/innodb/redo_log_forward.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// ==================== RedoLogRollforward 实现 ====================

RedoLogRollforward::RedoLogRollforward(size_t page_size)
    : page_size_(page_size), apply_recovery_(true) {
    std::memset(&stats_, 0, sizeof(stats_));
}

RedoLogRollforward::~RedoLogRollforward() = default;

void RedoLogRollforward::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

void RedoLogRollforward::set_redo_log_path(const std::string& path) {
    redo_log_path_ = path;
}

void RedoLogRollforward::set_lsn_range(uint64_t start_lsn, uint64_t end_lsn) {
    start_lsn_ = start_lsn;
    end_lsn_ = end_lsn;
}

void RedoLogRollforward::set_checkpoint_lsn(uint64_t checkpoint_lsn) {
    checkpoint_lsn_ = checkpoint_lsn;
}

void RedoLogRollforward::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

void RedoLogRollforward::set_apply_recovery(bool apply) {
    apply_recovery_ = apply;
}

void RedoLogRollforward::clear() {
    operations_.clear();
    modified_pages_.clear();
    pages_.clear();
    analyzer_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

bool RedoLogRollforward::scan_data_files() {
    if (data_dir_.empty()) {
        return false;
    }

    std::filesystem::path directory(data_dir_);

    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        return false;
    }

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();

                if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
                    std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
                    if (!file) continue;

                    uint64_t file_size = file.tellg();
                    file.seekg(0, std::ios::beg);

                    if (file_size < page_size_) {
                        continue;
                    }

                    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
                    std::vector<uint8_t> page_data(page_size_);

                    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
                        file.seekg(page_no * page_size_, std::ios::beg);
                        file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

                        uint16_t page_type = read_le<uint16_t>(page_data.data(), 16);
                        if (page_type != FIL_PAGE_INDEX) {
                            continue;
                        }

                        pages_[page_no] = page_data;
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // 忽略错误
    }

    return !pages_.empty();
}

std::vector<RedoLogRecord> RedoLogRollforward::parse_redo_log() {
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<RedoLogRecord> records;

    if (redo_log_path_.empty()) {
        return records;
    }

    std::ifstream file(redo_log_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return records;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 5) {
        return records;
    }

    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);

    // 解析 Redo Log 记录
    size_t pos = 0;
    uint64_t lsn = 0;

    while (pos + 5 <= buffer.size()) {
        RedoLogRecord record;
        record.r_type = read_le<uint8_t>(buffer.data(), pos);
        pos += sizeof(uint8_t);

        if (pos + 8 <= buffer.size()) {
            record.r_space_id = read_le<uint32_t>(buffer.data(), pos);
            pos += sizeof(uint32_t);
            record.r_page_no = read_le<uint32_t>(buffer.data(), pos);
            pos += sizeof(uint32_t);
        }

        if (pos < buffer.size()) {
            record.r_data.assign(buffer.begin() + pos, buffer.end());
        }

        record.r_lsn = lsn;
        records.push_back(record);

        // 简化：假设每个记录至少 5 字节
        pos += record.r_data.size() + 1;
        lsn += record.r_data.size() + 1;
    }

    stats_.total_records = records.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.parse_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return records;
}

std::optional<RedoOperation> RedoLogRollforward::parse_redo_record(const RedoLogRecord& record, uint64_t lsn) {
    RedoOperation op;
    op.lsn = lsn;
    op.space_id = record.r_space_id;
    op.page_no = record.r_page_no;
    op.data = record.r_data;

    // 根据类型解析
    switch (record.r_type) {
        case MLOG_1BYTE:
        case MLOG_2BYTES:
        case MLOG_4BYTES:
        case MLOG_8BYTES:
            op.type = RedoOperationType::UPDATE;
            break;
        case MLOG_REC_INSERT:
            op.type = RedoOperationType::INSERT;
            break;
        case MLOG_REC_DELETE:
            op.type = RedoOperationType::DELETE;
            break;
        case MLOG_PAGE_CREATE:
            op.type = RedoOperationType::PAGE_CREATE;
            break;
        case MLOG_PAGE_REINIT:
            op.type = RedoOperationType::PAGE_REINIT;
            break;
        default:
            op.type = RedoOperationType::UNKNOWN;
            break;
    }

    return op;
}

bool RedoLogRollforward::apply_redo_operations(const std::vector<RedoLogRecord>& records) {
    auto start = std::chrono::high_resolution_clock::now();

    operations_.clear();
    modified_pages_.clear();

    for (const auto& record : records) {
        auto op_opt = parse_redo_record(record, record.r_lsn);
        if (!op_opt.has_value()) {
            stats_.records_skipped++;
            continue;
        }

        auto op = op_opt.value();
        operations_.push_back(op);

        if (apply_recovery_) {
            bool success = false;
            switch (op.type) {
                case RedoOperationType::INSERT:
                    success = apply_insert(op);
                    break;
                case RedoOperationType::UPDATE:
                    success = apply_update(op);
                    break;
                case RedoOperationType::DELETE:
                    success = apply_delete(op);
                    break;
                default:
                    success = true;
                    break;
            }

            if (success) {
                stats_.records_applied++;
                modified_pages_.insert(op.page_no);
            } else {
                stats_.records_skipped++;
            }
        } else {
            stats_.records_applied++;
        }
    }

    stats_.pages_modified = modified_pages_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.apply_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_time_ms = stats_.parse_time_ms + stats_.apply_time_ms;

    return !operations_.empty();
}

bool RedoLogRollforward::apply_insert(const RedoOperation& op) {
    if (op.data.size() < 5) {
        return false;
    }

    auto page_opt = get_page(op.page_no);
    if (!page_opt) {
        return false;
    }

    auto& page_data = pages_[op.page_no];

    // Redo log insert record format (simplified):
    // [2B] record offset within page (where to insert)
    // [1B] info_bits + n_owned
    // [2B] next_offset (heap_no in REDUNDANT)
    // [variable] record body
    //
    // For MLOG_COMP_REC_INSERT (type 30), the format is:
    // [1B] record type/flags
    // [2B] record offset (little-endian)
    // [variable] record data including offset table, null bitmap, fields
    //
    // For MLOG_REC_INSERT (type 10, REDUNDANT format):
    // [1B] record type/flags
    // [variable] record data

    size_t pos = 0;
    uint8_t rec_type = op.data[pos++];

    if (rec_type == MLOG_COMP_REC_INSERT || rec_type == MLOG_REC_INSERT) {
        // COMPACT/DYNAMIC insert: [2B offset] [variable record data]
        if (op.data.size() < pos + 2) {
            return false;
        }
        uint16_t insert_offset = read_le<uint16_t>(op.data.data(), pos);
        pos += 2;

        size_t rec_len = op.data.size() - pos;
        if (rec_len == 0 || insert_offset + rec_len > page_data.size()) {
            return false;
        }

        // Copy record data into page at specified offset
        std::memcpy(page_data.data() + insert_offset, op.data.data() + pos, rec_len);

        // Update PAGE_HEAP_TOP if record extends beyond current heap top
        uint16_t heap_top = read_le<uint16_t>(page_data.data(),
            FIL_HEADER_SIZE + 20); // PAGE_HEAP_TOP at FIL_HEADER + 20
        if (insert_offset + rec_len > heap_top) {
            write_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 20,
                static_cast<uint16_t>(insert_offset + rec_len));
        }

        // Increment PAGE_N_RECS
        uint16_t n_recs = read_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 16);
        write_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 16, n_recs + 1);

        // Update page directory: append new slot for this record
        // PAGE_N_DIR_SLOTS is at FIL_HEADER + 4, low 15 bits
        uint16_t dir_field = read_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 4);
        uint16_t n_dir_slots = dir_field & 0x7FFF;
        uint16_t dir_flags = dir_field & 0x8000;
        uint16_t last_dir_offset = FIL_HEADER_SIZE + 38; // PAGE_HEADER size

        // Page directory grows downward from PAGE_DIR_END (near page trailer)
        // Each slot is 2 bytes pointing to a record offset
        uint16_t dir_end = page_data.size() - 8; // before FIL trailer
        uint16_t new_slot_pos = dir_end - (n_dir_slots * 2);

        if (new_slot_pos >= heap_top + 4) {
            write_le<uint16_t>(page_data.data(), new_slot_pos, insert_offset);
            write_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 4,
                ((n_dir_slots + 1) & 0x7FFF) | dir_flags);
        }

        // Recompute checksum
        uint32_t cksum = compute_crc32c_checksum(page_data.data(), page_data.size());
        write_le<uint32_t>(page_data.data(), 0, cksum);
        write_le<uint32_t>(page_data.data(), page_data.size() - 4, cksum);

        return true;
    }

    // Generic insert: append record data to page heap area
    uint16_t heap_top = read_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 20);
    size_t rec_len = op.data.size() - pos;

    if (heap_top + rec_len > page_data.size() - 8) {
        return false; // no space for record + trailer
    }

    std::memcpy(page_data.data() + heap_top, op.data.data() + pos, rec_len);
    write_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 20,
        static_cast<uint16_t>(heap_top + rec_len));

    uint16_t n_recs = read_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 16);
    write_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 16, n_recs + 1);

    uint32_t cksum = compute_crc32c_checksum(page_data.data(), page_data.size());
    write_le<uint32_t>(page_data.data(), 0, cksum);
    write_le<uint32_t>(page_data.data(), page_data.size() - 4, cksum);

    return true;
}

bool RedoLogRollforward::apply_update(const RedoOperation& op) {
    if (op.data.size() < 5) {
        return false;
    }

    auto page_opt = get_page(op.page_no);
    if (!page_opt) {
        return false;
    }

    auto& page_data = pages_[op.page_no];

    // Redo log update record format:
    // [2B] record offset within page (target record)
    // [2B] field_no + len (update spec)
    // [variable] replacement field data
    //
    // MLOG_COMP_REC_UPDATE (type 31):
    // [1B] record type/flags
    // [2B] record offset (little-endian)
    // [2B] update vector offset within record
    // [2B] update data length
    // [variable] update data

    size_t pos = 0;
    uint8_t rec_type = op.data[pos++];

    if (rec_type == MLOG_COMP_REC_UPDATE || rec_type == MLOG_REC_UPDATE) {
        if (op.data.size() < pos + 2) {
            return false;
        }
        uint16_t rec_offset = read_le<uint16_t>(op.data.data(), pos);
        pos += 2;

        if (rec_offset >= page_data.size()) {
            return false;
        }

        // Parse the update vector
        // Format: n_fields (1B), then for each field: field_no(2B) + len(2B) + data(len)
        if (pos >= op.data.size()) {
            return false;
        }
        uint8_t n_fields = op.data[pos++];

        for (uint8_t i = 0; i < n_fields && pos + 4 <= op.data.size(); i++) {
            uint16_t field_offset = read_le<uint16_t>(op.data.data(), pos);
            pos += 2;
            uint16_t data_len = read_le<uint16_t>(op.data.data(), pos);
            pos += 2;

            if (pos + data_len > op.data.size()) {
                return false;
            }

            uint32_t target_offset = rec_offset + field_offset;
            if (target_offset + data_len > page_data.size()) {
                continue;
            }

            std::memcpy(page_data.data() + target_offset, op.data.data() + pos, data_len);
            pos += data_len;
        }

        // Recompute checksum after update
        uint32_t cksum = compute_crc32c_checksum(page_data.data(), page_data.size());
        write_le<uint32_t>(page_data.data(), 0, cksum);
        write_le<uint32_t>(page_data.data(), page_data.size() - 4, cksum);

        return true;
    }

    // Generic field-level byte update
    if (op.data.size() < pos + 4) {
        return false;
    }
    uint16_t update_offset = read_le<uint16_t>(op.data.data(), pos);
    pos += 2;
    size_t update_len = op.data.size() - pos;

    if (update_offset + update_len > page_data.size()) {
        return false;
    }

    std::memcpy(page_data.data() + update_offset, op.data.data() + pos, update_len);

    uint32_t cksum = compute_crc32c_checksum(page_data.data(), page_data.size());
    write_le<uint32_t>(page_data.data(), 0, cksum);
    write_le<uint32_t>(page_data.data(), page_data.size() - 4, cksum);

    return true;
}

bool RedoLogRollforward::apply_delete(const RedoOperation& op) {
    if (op.data.size() < 2) {
        return false;
    }

    auto page_opt = get_page(op.page_no);
    if (!page_opt) {
        return false;
    }

    auto& page_data = pages_[op.page_no];

    // Redo log delete record format:
    // [2B] record offset within page
    //
    // MLOG_COMP_REC_DELETE (type 32):
    // [1B] record type/flags
    // [2B] record offset (little-endian)
    // Optional: [2B] next record offset to fix chain

    size_t pos = 0;
    uint8_t rec_type = op.data[pos++];

    uint16_t rec_offset;
    if (rec_type == MLOG_COMP_REC_DELETE || rec_type == MLOG_REC_DELETE) {
        if (op.data.size() < pos + 2) {
            return false;
        }
        rec_offset = read_le<uint16_t>(op.data.data(), pos);
        pos += 2;
    } else {
        // Direct offset as first 2 bytes
        rec_offset = read_le<uint16_t>(op.data.data(), 0);
    }

    if (rec_offset >= page_data.size() || rec_offset < FIL_HEADER_SIZE + 38) {
        return false;
    }

    // Set delete-mark flag on the record
    // In COMPACT/DYNAMIC format: info_bits is at rec_offset - 1 (last byte of offset table)
    // The delete-mark bit is REC_INFO_DELETED_FLAG (0x20)
    // Record header: [info_bits(1B)] [next_offset(2B)]
    // For COMPACT: info_bits is at the byte before the record data starts

    // Try to locate info_bits:
    // - In COMPACT/DYNAMIC, the record's header starts after the offset table entries
    // - We need to find the offset table first, then the record data follows
    // - info_bits is the first byte of the record body (not the offset table)
    //
    // Simplified approach: try to find and set delete flag at the record header
    // The record header starts with info_bits at the record offset

    // Check if we're dealing with a COMPACT-style record by looking at the page format
    uint16_t page_type = read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);
    if (page_type != FIL_PAGE_INDEX) {
        return false;
    }

    // For COMPACT/DYNAMIC: info_bits is the first byte after the offset table entries
    // The offset table entries (2 bytes each, stored in reverse) end at the record start
    // So we need to walk back from the record offset to find where data begins
    //
    // Since we don't have the full schema here, use a heuristic:
    // The record offset from the redo log typically points to the beginning of the
    // record body (after offset table). info_bits is the first byte there.

    uint8_t info_bits = page_data[rec_offset];
    page_data[rec_offset] = info_bits | REC_INFO_DELETED_FLAG;  // Set delete-mark flag

    // Decrement PAGE_N_RECS
    uint16_t n_recs = read_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 16);
    if (n_recs > 0) {
        write_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 16, n_recs - 1);
    }

    // Recompute checksum
    uint32_t cksum = compute_crc32c_checksum(page_data.data(), page_data.size());
    write_le<uint32_t>(page_data.data(), 0, cksum);
    write_le<uint32_t>(page_data.data(), page_data.size() - 4, cksum);

    return true;
}

std::optional<std::vector<uint8_t>> RedoLogRollforward::get_page(uint32_t page_no) {
    auto it = pages_.find(page_no);
    if (it == pages_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void RedoLogRollforward::set_page(uint32_t page_no, std::vector<uint8_t> page_data) {
    pages_[page_no] = std::move(page_data);
}

std::vector<std::map<std::string, std::string>> RedoLogRollforward::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 扫描数据文件
    scan_data_files();

    // 解析 Redo Log
    auto records = parse_redo_log();

    // 应用 Redo Log 操作
    apply_redo_operations(records);

    // 返回恢复的记录（从修改的页中解析）
    std::vector<std::map<std::string, std::string>> recovered_records;

    for (uint32_t page_no : modified_pages_) {
        auto page_opt = get_page(page_no);
        if (!page_opt.has_value()) {
            continue;
        }

        BPlusTreeParser parser(page_opt.value());
        if (!parser.parse()) {
            continue;
        }

        auto page_records = parser.parse_records(schema_);
        recovered_records.insert(recovered_records.end(), page_records.begin(), page_records.end());
    }

    stats_.end_lsn = end_lsn_;
    if (end_lsn_ == 0 && !records.empty()) {
        stats_.end_lsn = records.back().r_lsn;
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return recovered_records;
}

std::vector<std::map<std::string, std::string>> RedoLogRollforward::recover_from_redo_log(
    const std::string& redo_log_path) {
    set_redo_log_path(redo_log_path);
    return recover();
}

// ==================== RedoLogRollforwardScanner 实现 ====================

RedoLogRollforwardScanner::RedoLogRollforwardScanner(size_t page_size)
    : page_size_(page_size) {}

RedoLogRollforwardScanner::~RedoLogRollforwardScanner() = default;

void RedoLogRollforwardScanner::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

void RedoLogRollforwardScanner::set_redo_log_directory(const std::string& dir) {
    redo_log_dir_ = dir;
}

void RedoLogRollforwardScanner::clear() {
    redo_log_files_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

void RedoLogRollforwardScanner::scan_directory(const std::string& dir) {
    std::filesystem::path directory(dir);

    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        return;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();

                // 检查是否是 Redo Log 文件
                // MySQL 8.0: ib_logfile0, ib_logfile1, etc.
                // 或 ib_logfileN
                if (filename.find("ib_logfile") == 0) {
                    redo_log_files_.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        // 忽略错误
    }
}

std::vector<std::string> RedoLogRollforwardScanner::scan_redo_logs() {
    clear();

    if (redo_log_dir_.empty()) {
        return redo_log_files_;
    }

    scan_directory(redo_log_dir_);

    // 排序文件
    std::sort(redo_log_files_.begin(), redo_log_files_.end());

    stats_.total_records = redo_log_files_.size();

    return redo_log_files_;
}

std::optional<RedoLogRollforwardScanner::CheckpointInfo>
RedoLogRollforwardScanner::get_checkpoint(const std::string& redo_log_path) {
    std::ifstream file(redo_log_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 16384) {
        return std::nullopt;
    }

    std::vector<uint8_t> page_data(16384);
    file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

    // 检查点 1 位于偏移 76-115
    constexpr size_t CHECKPOINT1_OFFSET = 76;
    constexpr size_t CHECKPOINT2_OFFSET = 136;

    uint32_t cp1_lsn_low = read_le<uint32_t>(page_data.data(), CHECKPOINT1_OFFSET);
    uint32_t cp1_lsn_high = read_le<uint32_t>(page_data.data(), CHECKPOINT1_OFFSET + 4);
    uint32_t cp1_page_no = read_le<uint32_t>(page_data.data(), CHECKPOINT1_OFFSET + 8);

    uint32_t cp2_lsn_low = read_le<uint32_t>(page_data.data(), CHECKPOINT2_OFFSET);
    uint32_t cp2_lsn_high = read_le<uint32_t>(page_data.data(), CHECKPOINT2_OFFSET + 4);
    uint32_t cp2_page_no = read_le<uint32_t>(page_data.data(), CHECKPOINT2_OFFSET + 8);

    // 选择较新的检查点
    uint64_t lsn1 = (static_cast<uint64_t>(cp1_lsn_high) << 32) | cp1_lsn_low;
    uint64_t lsn2 = (static_cast<uint64_t>(cp2_lsn_high) << 32) | cp2_lsn_low;

    CheckpointInfo checkpoint;
    if (lsn1 >= lsn2) {
        checkpoint.lsn = lsn1;
        checkpoint.page_no = cp1_page_no;
    } else {
        checkpoint.lsn = lsn2;
        checkpoint.page_no = cp2_page_no;
    }

    return checkpoint;
}

} // namespace innodb
} // namespace dbrecovery
