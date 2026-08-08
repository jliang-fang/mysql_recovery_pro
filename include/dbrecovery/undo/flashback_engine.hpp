#ifndef DBRECOVERY_UNDO_FLASHBACK_ENGINE_HPP
#define DBRECOVERY_UNDO_FLASHBACK_ENGINE_HPP

// FlashbackEngine reconstructs row versions at any point in time
// using undo log version chains, and generates flashback SQL
// (UNDO DML statements) for point-in-time recovery.
// This is a convenience wrapper around the fully-implemented
// innodb::FlashbackGenerator, exposing the undo:: domain API.
#include "dbrecovery/innodb/flashback_generator.hpp"
#include "dbrecovery/innodb/deep_undo_analyzer.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <chrono>

namespace dbrecovery {
namespace undo {

// Re-export the innodb-level flashback types under the undo namespace so
// callers can use a single, stable public API without reaching into innodb.
using FlashbackMode       = innodb::FlashbackMode;
using FlashbackOutputFormat = innodb::FlashbackOutputFormat;
using FlashbackConfig     = innodb::FlashbackConfig;
using DecodedField        = innodb::DecodedField;
using FlashbackRecord     = innodb::FlashbackRecord;

/**
 * @brief High-level flashback engine for undo-log based point-in-time recovery
 *
 * Wraps innodb::FlashbackGenerator and adds orchestration conveniences:
 * path/page-size configuration, mode dispatch, and aggregate statistics.
 */
class FlashbackEngine {
public:
    FlashbackEngine() = default;
    ~FlashbackEngine() = default;

    // ---- Configuration ----
    void set_ibdata_path(const std::string& path) { generator_.set_ibdata_path(path); }
    void set_page_size(size_t page_size) { generator_.set_page_size(page_size); }
    void set_config(const FlashbackConfig& config) { generator_.set_config(config); }
    FlashbackConfig& config() { return generator_.config(); }
    const FlashbackConfig& config() const { return generator_.config(); }

    // ---- Core flashback operations ----
    std::vector<FlashbackRecord> flashback_deletes(const catalog::TableSchema& schema) {
        return generator_.flashback_deletes(schema);
    }
    std::vector<FlashbackRecord> flashback_updates(const catalog::TableSchema& schema) {
        return generator_.flashback_updates(schema);
    }
    std::vector<FlashbackRecord> flashback_transaction(uint64_t trx_id,
                                                       const catalog::TableSchema& schema) {
        return generator_.flashback_transaction(trx_id, schema);
    }
    std::vector<FlashbackRecord> flashback_point_in_time(
        std::chrono::system_clock::time_point point,
        const catalog::TableSchema& schema) {
        return generator_.flashback_point_in_time(point, schema);
    }
    std::vector<FlashbackRecord> flashback_full_chain(
        const std::vector<uint8_t>& current_record,
        uint64_t roll_ptr,
        const catalog::TableSchema& schema,
        uint64_t index_id) {
        return generator_.flashback_full_chain(current_record, roll_ptr, schema, index_id);
    }

    // ---- Output generation ----
    std::string generate_output(const std::vector<FlashbackRecord>& records,
                                const catalog::TableSchema& schema) {
        return generator_.generate_output(records, schema);
    }

    // ---- Statistics ----
    const innodb::FlashbackGenerator::Stats& get_stats() const { return generator_.get_stats(); }
    void reset_stats() { generator_.reset_stats(); }

    // ---- Static helpers (schema-aware value decoding) ----
    static std::string decode_field_value(const std::vector<uint8_t>& raw_bytes,
                                          const catalog::ColumnInfo& col_info,
                                          bool human_readable = true) {
        return innodb::FlashbackGenerator::decode_field_value(raw_bytes, col_info, human_readable);
    }
    static std::string bytes_to_hex(const std::vector<uint8_t>& data) {
        return innodb::FlashbackGenerator::bytes_to_hex(data);
    }

private:
    innodb::FlashbackGenerator generator_;
};

} // namespace undo
} // namespace dbrecovery

#endif // DBRECOVERY_UNDO_FLASHBACK_ENGINE_HPP