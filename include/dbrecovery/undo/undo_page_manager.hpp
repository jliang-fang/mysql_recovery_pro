#ifndef DBRECOVERY_UNDO_UNDO_PAGE_MANAGER_HPP
#define DBRECOVERY_UNDO_UNDO_PAGE_MANAGER_HPP

// UndoPageManager orchestrates undo page allocation, lookup, and lifecycle
// management for flashback and MVCC version chain traversal.
// It is a unified facade over the fully-implemented innodb undo modules:
//   - innodb::UndoPageIndex  : O(1) undo page lookup
//   - innodb::UndoLogParser  : undo log page/segment parsing
//   - innodb::UndoLogRecovery: MVCC chain reconstruction + rollback
#include "dbrecovery/innodb/deep_undo_analyzer.hpp"
#include "dbrecovery/innodb/undo_log_parser.hpp"
#include "dbrecovery/innodb/undo_log_recovery.hpp"
#include "dbrecovery/innodb/undo_page_index.hpp"
#include "dbrecovery/undo/undo_record_v2.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>

namespace dbrecovery {
namespace undo {

// Re-export the innodb-level undo types under the undo namespace.
using UndoLogType   = innodb::UndoLogType;
using UndoLogRecord = innodb::UndoLogRecord;
using UndoLogPage   = innodb::UndoLogPage;
using UndoLogChain  = innodb::UndoLogChain;
using UndoLogEntry  = innodb::UndoLogEntry;
using UndoLogField  = innodb::UndoLogField;
using UndoLogRecoveryStats = innodb::UndoLogRecoveryStats;
using FlashbackFilter = innodb::FlashbackFilter;

/**
 * @brief Unified undo page manager — index, parse, and recover undo logs
 *
 * Combines undo page indexing (fast lookup), undo log page parsing, and
 * MVCC chain recovery under one facade. Typical usage:
 *
 *   UndoPageManager mgr;
 *   mgr.set_data_file("ibdata1");
 *   mgr.build_index();                 // index all undo pages
 *   mgr.set_schema(schema);
 *   auto records = mgr.recover();      // parse + build chains + rollback
 */
class UndoPageManager {
public:
    UndoPageManager() = default;
    ~UndoPageManager() = default;

    // ---- Data source configuration ----
    void set_data_file(const std::string& path) {
        data_file_ = path;
        recovery_.set_file_path(path);
        analyzer_.set_ibdata_path(path);
    }
    void set_page_size(size_t page_size) {
        page_size_ = page_size;
        recovery_ = innodb::UndoLogRecovery(page_size);
        recovery_.set_file_path(data_file_);
        analyzer_.set_page_size(page_size);
        if (!schema_.columns.empty()) recovery_.set_schema(schema_);
    }
    void set_schema(const catalog::TableSchema& schema) {
        schema_ = schema;
        recovery_.set_schema(schema);
    }

    // ---- Undo page index (innodb::UndoPageIndex) ----
    bool build_index() {
        index_.build(data_file_, page_size_);
        return index_.is_built();
    }
    bool is_undo_page(uint32_t page_no) const { return index_.is_undo_page(page_no); }
    std::vector<uint32_t> undo_pages() const { return index_.undo_pages(); }
    size_t index_size() const { return index_.size(); }
    const innodb::UndoPageIndex::BuildStats& index_stats() const { return index_.stats(); }

    // ---- Authoritative V2 analysis ----
    void set_undo_format_profile(UndoFormatProfile profile) {
        analyzer_.set_undo_format_profile(profile);
    }
    UndoFormatProfile undo_format_profile() const {
        return analyzer_.get_undo_format_profile();
    }
    void set_tablespace_id(uint32_t space_id) {
        analyzer_.set_tablespace_id(space_id);
    }
    void set_max_chain_depth(uint32_t depth) {
        analyzer_.set_max_chain_depth(depth);
    }
    std::vector<UndoRecordV2> scan_records_v2(
        const catalog::TableSchema& schema,
        const FlashbackFilter& filter = {},
        std::initializer_list<UndoRecordV2::Type> types = {}) {
        return analyzer_.scan_records(schema, filter, types);
    }
    RowVersionHistory trace_version_chain_v2(
        const std::vector<uint8_t>& current_record,
        uint64_t roll_ptr,
        const catalog::TableSchema& schema,
        uint64_t index_id) {
        return analyzer_.trace_version_chain(current_record, roll_ptr, schema, index_id);
    }
    const innodb::DeepUndoAnalyzer::AnalyzerStats& analysis_stats() const {
        return analyzer_.get_stats();
    }

    // ---- Legacy/forensic compatibility path ----
    // These methods expose the pre-V2 heuristic parser and are not safe for
    // authoritative export. Use the V2 methods above for verified histories.
    void set_lsn_range(std::optional<uint64_t> start, std::optional<uint64_t> end) {
        recovery_.set_lsn_range(start.value_or(0), end.value_or(0));
    }
    void set_apply_rollback(bool apply) { recovery_.set_apply_rollback(apply); }
    const UndoLogRecoveryStats& recovery_stats() const { return recovery_.get_stats(); }
    const std::vector<UndoLogRecord>& records() const { return recovery_.get_records(); }
    const std::vector<UndoLogChain>& chains() const { return recovery_.get_chains(); }
    std::vector<innodb::HeuristicRecord> recover_table(uint32_t space_id, uint32_t index_id) {
        return parser_.recover_from_undo_logs(data_file_, std::to_string(space_id), index_id);
    }

    bool recover_legacy() {
        auto result = recovery_.recover();
        return !result.empty();
    }
    std::vector<UndoLogEntry> parse_page_legacy(const uint8_t* page_data, size_t page_size,
                                                 uint16_t page_no) {
        return parser_.parse_undo_log_page(page_data, page_size, page_no);
    }

    // Retained as a source-compatible alias for callers of the old facade.
    bool recover() { return recover_legacy(); }
    std::vector<UndoLogEntry> parse_page(const uint8_t* page_data, size_t page_size,
                                         uint16_t page_no) {
        return parse_page_legacy(page_data, page_size, page_no);
    }

    void clear() {
        index_.reset();
        recovery_.clear();
        analyzer_.reset_stats();
    }

private:
    std::string data_file_;
    size_t page_size_ = 16384;
    catalog::TableSchema schema_;
    innodb::UndoPageIndex index_;
    innodb::UndoLogRecovery recovery_;
    innodb::UndoLogParser parser_;
    innodb::DeepUndoAnalyzer analyzer_;
};

} // namespace undo
} // namespace dbrecovery

#endif // DBRECOVERY_UNDO_UNDO_PAGE_MANAGER_HPP