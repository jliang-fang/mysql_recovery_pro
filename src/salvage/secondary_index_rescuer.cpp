/**
 * @file secondary_index_rescuer.cpp
 * @brief SecondaryIndexRescuer (salvage namespace) implementation
 */

#include "dbrecovery/salvage/secondary_index_rescuer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/btree_node_parser.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <set>

namespace dbrecovery {
namespace salvage {

SecondaryIndexRescuer::SecondaryIndexRescuer(size_t page_size)
    : page_size_(page_size), inner_(page_size) {}

// ---- Core extraction (delegates to innodb) ----

std::map<std::string, std::map<std::string, std::string>>
SecondaryIndexRescuer::extract_from_secondary_index(const std::string& sec_ibd_path) {
    return inner_.extract_from_secondary_index(sec_ibd_path);
}

std::vector<RescuedRow> SecondaryIndexRescuer::merge_indexes(
    const std::vector<std::map<std::string, std::map<std::string, std::string>>>& index_data_list) {
    return inner_.merge_indexes(index_data_list);
}

std::vector<RescuedRow> SecondaryIndexRescuer::rescue_from_indexes(
    const std::vector<std::string>& sec_ibd_paths) {
    return inner_.rescue_from_indexes(sec_ibd_paths);
}

// ---- Cross-index row reconstruction ----

SecondaryIndexRescuer::CrossIndexResult
SecondaryIndexRescuer::cross_index_reconstruct(
    const std::vector<std::string>& sec_ibd_paths) {
    CrossIndexResult result;
    result.total_indexes_available = static_cast<uint32_t>(sec_ibd_paths.size());

    std::vector<std::map<std::string, std::map<std::string, std::string>>> all_data;
    for (const auto& path : sec_ibd_paths) {
        auto data = inner_.extract_from_secondary_index(path);
        if (!data.empty()) {
            all_data.push_back(std::move(data));
            result.indexes_used++;
        }
    }

    auto merged = inner_.merge_indexes(all_data);
    result.rows = std::move(merged);

    // Calculate average completeness
    double total_completeness = 0.0;
    for (const auto& row : result.rows) {
        total_completeness += row.completeness;
    }
    result.avg_completeness = result.rows.empty() ? 0.0 :
        total_completeness / static_cast<double>(result.rows.size());

    // Generate warnings for low-coverage indexes
    if (result.indexes_used < result.total_indexes_available) {
        result.warnings.push_back(
            "Only " + std::to_string(result.indexes_used) + " of " +
            std::to_string(result.total_indexes_available) +
            " secondary indexes yielded data");
    }

    return result;
}

// ---- Partial index merging ----

SecondaryIndexRescuer::PartialIndexResult
SecondaryIndexRescuer::partial_index_merge(
    const std::vector<std::string>& sec_ibd_paths,
    const std::vector<std::string>& damaged_indexes) {
    PartialIndexResult result;

    // Build set of damaged indexes for reporting
    std::set<std::string> damaged_set(damaged_indexes.begin(), damaged_indexes.end());

    std::vector<std::string> usable_paths;
    for (const auto& path : sec_ibd_paths) {
        // Check if this path corresponds to a damaged index
        bool is_damaged = false;
        for (const auto& d : damaged_set) {
            if (path.find(d) != std::string::npos) {
                is_damaged = true;
                break;
            }
        }
        // Still try to extract from damaged indexes — may get partial data
        usable_paths.push_back(path);
    }

    auto rescued = inner_.rescue_from_indexes(usable_paths);
    result.rows = std::move(rescued);

    // Calculate per-column completeness
    if (!result.rows.empty()) {
        std::map<std::string, uint32_t> col_filled;
        std::map<std::string, uint32_t> col_total;

        for (const auto& row : result.rows) {
            for (const auto& [col_name, value] : row.fields) {
                col_total[col_name]++;
                if (value != "NULL") {
                    col_filled[col_name]++;
                }
            }
        }

        for (const auto& [col_name, total] : col_total) {
            auto it = col_filled.find(col_name);
            result.column_completeness[col_name] =
                (it != col_filled.end()) ?
                    static_cast<double>(it->second) / static_cast<double>(total) : 0.0;
        }

        // Overall completeness: average of per-column completeness
        double sum = 0.0;
        for (const auto& [name, ratio] : result.column_completeness) {
            sum += ratio;
        }
        result.overall_completeness = result.column_completeness.empty() ? 0.0 :
            sum / static_cast<double>(result.column_completeness.size());
    }

    return result;
}

// ---- Confidence scoring ----

SecondaryIndexRescuer::ConfidenceScore
SecondaryIndexRescuer::score_row(const RescuedRow& row,
                                  const std::vector<std::string>& all_indexes) const {
    ConfidenceScore score;

    // Row completeness: fraction of non-null columns
    uint32_t total_cols = static_cast<uint32_t>(row.fields.size());
    uint32_t non_null = total_cols - static_cast<uint32_t>(row.missing_fields.size());
    score.row_completeness = total_cols > 0 ?
        static_cast<double>(non_null) / static_cast<double>(total_cols) : 0.0;

    // Index coverage: placeholder — requires knowing which indexes contain this PK
    // For now, derived from the row source
    score.index_coverage = (row.source == "merged") ? 0.8 : 0.4;

    // Source reliability
    score.source_reliability = (row.source == "merged") ? 1.0 : 0.5;

    // Weighted overall score
    score.overall = score.row_completeness * 0.5 +
                    score.index_coverage * 0.3 +
                    score.source_reliability * 0.2;

    // Clamp to [0, 1]
    if (score.overall < 0.0) score.overall = 0.0;
    if (score.overall > 1.0) score.overall = 1.0;

    // Assessment
    if (score.overall >= 0.8) {
        score.assessment = "HIGH";
    } else if (score.overall >= 0.5) {
        score.assessment = "MEDIUM";
    } else {
        score.assessment = "LOW";
    }

    return score;
}

// ---- Clear ----

void SecondaryIndexRescuer::clear() {
    inner_ = innodb::SecondaryIndexRescuer(page_size_);
    if (!schema_.columns.empty()) {
        inner_.set_schema(schema_);
    }
    if (target_name_.has_value()) {
        inner_.set_target_secondary_index(*target_name_);
    }
}

} // namespace salvage
} // namespace dbrecovery