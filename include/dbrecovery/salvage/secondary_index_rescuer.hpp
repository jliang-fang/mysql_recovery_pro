#ifndef DBRECOVERY_SALVAGE_SECONDARY_INDEX_RESCUER_HPP
#define DBRECOVERY_SALVAGE_SECONDARY_INDEX_RESCUER_HPP

// SecondaryIndexRescuer (salvage namespace) extends the innodb version
// with salvage-specific features: cross-index row reconstruction,
// partial index merging, and confidence scoring for secondary-index-only
// recovery when the primary index is unavailable.
#include "dbrecovery/innodb/secondary_index_rescuer.hpp"
#include "dbrecovery/innodb/sec_index_btree_repair.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <set>

namespace dbrecovery {
namespace salvage {

// Re-export innodb-level rescue types.
using RescuedRow              = innodb::RescuedRow;
using SecondaryIndexRescueStats = innodb::SecondaryIndexRescueStats;
using SecLeafRecord           = innodb::SecondaryIndexRescuer::SecLeafRecord;

/**
 * @brief Salvage-specific secondary index rescuer
 *
 * Wraps innodb::SecondaryIndexRescuer and adds salvage-specific features:
 * - Cross-index row reconstruction: merge data from multiple secondary indexes
 *   to reconstruct rows when the primary/clustered index is unavailable
 * - Partial index merging: combine data from damaged or incomplete indexes
 * - Confidence scoring: estimate completeness and reliability of rescued rows
 * - B-tree repair integration: fix corrupted secondary index pages before extraction
 */
class SecondaryIndexRescuer {
public:
    explicit SecondaryIndexRescuer(size_t page_size = 16384);

    // ---- Schema ----
    void set_schema(const catalog::TableSchema& schema) {
        schema_ = schema;
        target_name_.reset();
        inner_.set_schema(schema);
    }

    // ---- Target secondary index selection (Phase 5b.1) ----
    // Fails closed: without an explicit authoritative target, no structured
    // secondary-index recovery is possible.
    bool set_target_secondary_index(const std::string& name) {
        target_name_ = name;
        return inner_.set_target_secondary_index(name);
    }

    // ---- Core extraction (delegates to innodb) ----
    std::map<std::string, std::map<std::string, std::string>>
    extract_from_secondary_index(const std::string& sec_ibd_path);

    std::vector<RescuedRow> merge_indexes(
        const std::vector<std::map<std::string, std::map<std::string, std::string>>>& index_data_list);

    std::vector<RescuedRow> rescue_from_indexes(const std::vector<std::string>& sec_ibd_paths);

    // ---- Salvage-specific: cross-index row reconstruction ----
    struct CrossIndexResult {
        std::vector<RescuedRow> rows;
        uint32_t indexes_used = 0;
        uint32_t total_indexes_available = 0;
        double avg_completeness = 0.0;
        std::vector<std::string> warnings;
    };

    /**
     * @brief Reconstruct rows by cross-referencing multiple secondary indexes
     *
     * For each primary key value found in any secondary index, collects all
     * available column values from all indexes, then reports completeness.
     */
    CrossIndexResult cross_index_reconstruct(
        const std::vector<std::string>& sec_ibd_paths);

    // ---- Salvage-specific: partial index merging ----
    struct PartialIndexResult {
        std::vector<RescuedRow> rows;
        std::map<std::string, double> column_completeness;  // col_name → fill ratio
        double overall_completeness = 0.0;
    };

    /**
     * @brief Merge data from partial/corrupted indexes
     *
     * Handles the case where some secondary indexes are damaged:
     * extracts what's available from each, merges, and reports per-column
     * completeness.
     */
    PartialIndexResult partial_index_merge(
        const std::vector<std::string>& sec_ibd_paths,
        const std::vector<std::string>& damaged_indexes = {});

    // ---- Salvage-specific: confidence scoring ----
    struct ConfidenceScore {
        double row_completeness = 0.0;     // fraction of non-null columns
        double index_coverage = 0.0;       // fraction of indexes that have this PK
        double source_reliability = 0.0;   // 1.0 for merged, 0.5 for single-index
        double overall = 0.0;              // weighted combination
        std::string assessment;            // "HIGH", "MEDIUM", "LOW"
    };

    ConfidenceScore score_row(const RescuedRow& row,
                              const std::vector<std::string>& all_indexes) const;

    // ---- Stats ----
    const SecondaryIndexRescueStats& get_stats() const { return inner_.get_stats(); }

    void clear();

private:
    innodb::SecondaryIndexRescuer inner_;
    size_t page_size_;
    catalog::TableSchema schema_;
    std::optional<std::string> target_name_;
};

} // namespace salvage
} // namespace dbrecovery

#endif // DBRECOVERY_SALVAGE_SECONDARY_INDEX_RESCUER_HPP