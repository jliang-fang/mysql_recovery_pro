#include "dbrecovery/innodb/redo_log_replayer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// ==================== RedoLogReplayer ====================

RedoLogReplayer::RedoLogReplayer() : page_size_(16384) {
    std::memset(&stats_, 0, sizeof(stats_));
}

RedoLogReplayer::~RedoLogReplayer() = default;

void RedoLogReplayer::set_page_size(uint32_t page_size) {
    if (page_size == 4096 || page_size == 8192 ||
        page_size == 16384 || page_size == 32768 || page_size == 65536) {
        page_size_ = page_size;
    }
}

bool RedoLogReplayer::add_page(uint32_t page_no, std::vector<uint8_t> page_data) {
    if (page_data.size() < 38) {
        return false;
    }

    pages_[page_no] = std::move(page_data);
    return true;
}

void RedoLogReplayer::add_redo_records(const std::vector<RedoLogRecord>& records) {
    redo_records_.insert(redo_records_.end(), records.begin(), records.end());
}

RedoReplayResult RedoLogReplayer::replay(const catalog::TableSchema& schema) {
    RedoReplayResult result;
    result.success = true;
    result.records_processed = 0;
    result.records_applied = 0;
    result.records_failed = 0;

    if (redo_records_.empty()) {
        result.message = "No redo records to replay";
        return result;
    }

    for (const auto& record : redo_records_) {
        result.records_processed++;

        if (apply_redo_record(record.r_page_no, record)) {
            result.records_applied++;
        } else {
            result.records_failed++;
            errors_.push_back("Failed to apply record: " + record.to_string());
        }
    }

    stats_.records_processed = result.records_processed;
    stats_.records_applied = result.records_applied;
    stats_.records_failed = result.records_failed;

    if (result.records_failed > 0) {
        result.success = false;
        result.message = "Replay completed with " + std::to_string(result.records_failed) + " failures";
    } else {
        result.message = "Replay completed successfully";
    }

    return result;
}

bool RedoLogReplayer::apply_redo_record(uint32_t page_no, const RedoLogRecord& record) {
    auto it = pages_.find(page_no);
    if (it == pages_.end()) {
        std::cerr << "[WARN] RedoLogReplayer: Page " << page_no << " not found" << std::endl;
        return false;
    }

    auto& page_data = it->second;

    // Check page type at offset 24 (FIL_PAGE_TYPE_ABS, big-endian)
    uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX &&
        page_type != dbrecovery::innodb::FIL_PAGE_SDI &&
        page_type != dbrecovery::innodb::FIL_PAGE_UNDO_LOG) {
        std::cerr << "[WARN] RedoLogReplayer: Page " << page_no << " is not a valid page type (type=" << page_type << ")" << std::endl;
        return false;
    }

    // Apply based on record type
    switch (record.r_type) {
        case MLOG_1BYTE:
        case MLOG_2BYTES:
        case MLOG_4BYTES:
        case MLOG_8BYTES: {
            // Simple data updates
            if (record.r_data.size() < 2) {
                return false;
            }

            uint16_t offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), 0);
            if (offset + 8 > page_data.size()) {
                return false;
            }

            // Apply data update
            if (record.r_data.size() > 2) {
                size_t data_size = std::min(record.r_data.size() - 2, page_data.size() - offset);
                std::memcpy(page_data.data() + offset, record.r_data.data() + 2, data_size);
            }
            break;
        }

        case MLOG_REC_INSERT:
        case MLOG_COMP_REC_INSERT: {
            // Record insert - parse record offset and data, place into page
            // Format: [2B] record_offset [variable] record_data
            if (record.r_data.size() < 4) {
                break;
            }
            size_t pos = 0;
            uint8_t rec_type = record.r_data[pos++];
            if (record.r_data.size() < pos + 2) break;
            uint16_t insert_offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), pos);
            pos += 2;

            size_t rec_len = record.r_data.size() - pos;
            if (rec_len > 0 && insert_offset + rec_len <= page_data.size()) {
                std::memcpy(page_data.data() + insert_offset, record.r_data.data() + pos, rec_len);
                // Update PAGE_HEAP_TOP and PAGE_N_RECS
                uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 38 + 20);
                if (insert_offset + rec_len > heap_top) {
                    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 38 + 20,
                        static_cast<uint16_t>(insert_offset + rec_len));
                }
                uint16_t n_recs = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 38 + 16);
                dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 38 + 16, n_recs + 1);
            }
            break;
        }

        case MLOG_REC_UPDATE:
        case MLOG_COMP_REC_UPDATE: {
            // Record update - parse update vector and apply to page
            // Format: [2B] record_offset [1B] n_fields [for each: 2B field_off 2B len data]
            if (record.r_data.size() < 5) {
                break;
            }
            size_t pos = 0;
            uint8_t rec_type = record.r_data[pos++];
            if (record.r_data.size() < pos + 2) break;
            uint16_t rec_offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), pos);
            pos += 2;

            if (pos < record.r_data.size()) {
                uint8_t n_fields = record.r_data[pos++];
                for (uint8_t i = 0; i < n_fields && pos + 4 <= record.r_data.size(); i++) {
                    uint16_t field_offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), pos);
                    pos += 2;
                    uint16_t data_len = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), pos);
                    pos += 2;
                    if (pos + data_len <= record.r_data.size() &&
                        rec_offset + field_offset + data_len <= page_data.size()) {
                        std::memcpy(page_data.data() + rec_offset + field_offset,
                                    record.r_data.data() + pos, data_len);
                    }
                    pos += data_len;
                }
            }
            break;
        }

        case MLOG_REC_DELETE: {
            // Record delete - mark as deleted
            if (record.r_data.size() >= 2) {
                uint16_t offset = dbrecovery::innodb::read_le<uint16_t>(record.r_data.data(), 0);
                if (offset < page_data.size()) {
                    page_data[offset] |= REC_INFO_DELETED_FLAG;
                }
            }
            break;
        }

        case MLOG_PAGE_CREATE: {
            // Page create - clear page
            std::memset(page_data.data(), 0, page_data.size());
            break;
        }

        case MLOG_PAGE_REINIT: {
            // Page reinit - clear page header + records
            if (page_data.size() > 38) {
                std::memset(page_data.data() + 38, 0, page_data.size() - 38);
            }
            break;
        }

        default: {
            std::cerr << "[WARN] RedoLogReplayer: Unsupported log type " 
                      << static_cast<int>(record.r_type) << std::endl;
            return false;
        }
    }

    stats_.records_applied++;
    return true;
}

const std::map<uint32_t, std::vector<uint8_t>>& RedoLogReplayer::get_updated_pages() const {
    return pages_;
}

RedoLogReplayer::ReplayStats RedoLogReplayer::get_stats() const {
    return stats_;
}

void RedoLogReplayer::clear() {
    pages_.clear();
    redo_records_.clear();
    errors_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

// ==================== RedoLogChainBuilder ====================

RedoLogChainBuilder::RedoLogChainBuilder() {
    std::memset(&stats_, 0, sizeof(stats_));
}

RedoLogChainBuilder::~RedoLogChainBuilder() = default;

void RedoLogChainBuilder::add_record(const RedoLogRecord& record) {
    records_.push_back(record);
    stats_.total_records++;

    // Track LSN range
    if (stats_.start_lsn == 0) {
        stats_.start_lsn = record.r_lsn;
    }
    stats_.end_lsn = record.r_lsn;
}

bool RedoLogChainBuilder::build_chains() {
    if (records_.empty()) {
        return false;
    }

    // Group by space_id (simplified chain building)
    for (const auto& record : records_) {
        chains_[record.r_space_id].push_back(record);
        stats_.total_chains = std::max(stats_.total_chains, 
            static_cast<uint32_t>(chains_.size()));
    }

    return true;
}

std::vector<RedoLogRecord> RedoLogChainBuilder::get_records(uint64_t start_lsn, uint64_t end_lsn) {
    std::vector<RedoLogRecord> result;

    for (const auto& record : records_) {
        if (record.r_lsn >= start_lsn && record.r_lsn <= end_lsn) {
            result.push_back(record);
        }
    }

    return result;
}

const std::vector<RedoLogRecord>& RedoLogChainBuilder::get_records() const {
    return records_;
}

RedoLogChainBuilder::ChainStats RedoLogChainBuilder::get_stats() const {
    return stats_;
}

void RedoLogChainBuilder::clear() {
    records_.clear();
    chains_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

// ==================== RedoLogFilter ====================

RedoLogFilter::RedoLogFilter() = default;

RedoLogFilter::~RedoLogFilter() = default;

void RedoLogFilter::set_space_id(uint32_t space_id) {
    space_id_ = space_id;
}

void RedoLogFilter::set_page_no(uint32_t page_no) {
    page_no_ = page_no;
}

void RedoLogFilter::set_record_type(uint8_t type) {
    record_type_ = type;
}

void RedoLogFilter::set_lsn_range(uint64_t start_lsn, uint64_t end_lsn) {
    start_lsn_ = start_lsn;
    end_lsn_ = end_lsn;
}

std::vector<RedoLogRecord> RedoLogFilter::filter(const std::vector<RedoLogRecord>& records) {
    std::vector<RedoLogRecord> result;

    for (const auto& record : records) {
        if (passes_filter(record)) {
            result.push_back(record);
        }
    }

    return result;
}

bool RedoLogFilter::passes_filter(const RedoLogRecord& record) {
    if (space_id_.has_value() && record.r_space_id != space_id_.value()) {
        return false;
    }

    if (page_no_.has_value() && record.r_page_no != page_no_.value()) {
        return false;
    }

    if (record_type_.has_value() && record.r_type != record_type_.value()) {
        return false;
    }

    // LSN filter
    if (start_lsn_.has_value() || end_lsn_.has_value()) {
        if (start_lsn_.has_value() && record.r_lsn < start_lsn_.value()) {
            return false;
        }
        if (end_lsn_.has_value() && record.r_lsn > end_lsn_.value()) {
            return false;
        }
    }

    return true;
}

void RedoLogFilter::clear_filters() {
    space_id_.reset();
    page_no_.reset();
    record_type_.reset();
    start_lsn_.reset();
    end_lsn_.reset();
}

// ==================== RedoLogAnalyzer ====================

RedoLogAnalyzer::RedoLogAnalyzer() {
    std::memset(&analysis_, 0, sizeof(analysis_));
}

RedoLogAnalyzer::~RedoLogAnalyzer() = default;

bool RedoLogAnalyzer::analyze_file(const std::string& path) {
    RedoLogParser parser;
    auto records = parser.parse_file(path);

    for (const auto& record : records) {
        analyze_record(record);
    }

    return !records.empty();
}

RedoLogAnalyzer::AnalysisResult RedoLogAnalyzer::get_analysis() const {
    return analysis_;
}

std::map<uint8_t, uint32_t> RedoLogAnalyzer::get_type_distribution() const {
    return type_distribution_;
}

std::vector<std::pair<uint32_t, uint32_t>> RedoLogAnalyzer::get_top_pages(uint32_t count) const {
    std::vector<std::pair<uint32_t, uint32_t>> sorted_pages(page_update_count_.begin(), 
                                                              page_update_count_.end());

    std::sort(sorted_pages.begin(), sorted_pages.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    if (sorted_pages.size() > count) {
        sorted_pages.resize(count);
    }

    return sorted_pages;
}

void RedoLogAnalyzer::clear() {
    analysis_ = {};
    type_distribution_.clear();
    page_update_count_.clear();
}

void RedoLogAnalyzer::analyze_record(const RedoLogRecord& record) {
    analysis_.total_records++;

    // Count by type
    type_distribution_[record.r_type]++;
    analysis_.total_data_size += record.r_data.size();

    // Count by page
    page_update_count_[record.r_page_no]++;

    // Count specific types
    switch (record.r_type) {
        case MLOG_REC_INSERT:
        case MLOG_COMP_REC_INSERT:
            analysis_.insert_records++;
            break;
        case MLOG_REC_UPDATE:
        case MLOG_COMP_REC_UPDATE:
            analysis_.update_records++;
            break;
        case MLOG_REC_DELETE:
        case MLOG_COMP_REC_DELETE:
            analysis_.delete_records++;
            break;
        case MLOG_PAGE_CREATE:
        case MLOG_COMP_PAGE_CREATE:
            analysis_.page_create_records++;
            break;
        case MLOG_PAGE_REINIT:
        case MLOG_COMP_PAGE_REINIT:
            analysis_.page_reinit_records++;
            break;
        case MLOG_BLOB_WRITE:
            analysis_.blob_write_records++;
            break;
    }

    // Track unique space IDs and page numbers
    analysis_.unique_space_ids = std::max(analysis_.unique_space_ids,
        static_cast<uint32_t>(page_update_count_.size()));
    analysis_.unique_page_numbers = analysis_.unique_space_ids;
}

} // namespace innodb
} // namespace dbrecovery
