/**
 * @file data_correlation.hpp
 * @brief DataCorrelationEngine — correlates data across fragments,
 *        deduplicates, and merges overlapping records during salvage recovery
 *
 * v2.0 architecture: analyzes field-level correlations to identify
 * fragments belonging to the same row, merges partial records,
 * and deduplicates across recovery passes.
 */

#ifndef DBRECOVERY_SALVAGE_DATA_CORRELATION_HPP
#define DBRECOVERY_SALVAGE_DATA_CORRELATION_HPP

#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <optional>
#include <functional>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <limits>

#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace salvage {

/**
 * @brief FieldFragment — a single field value from a recovered fragment
 */
struct FieldFragment {
    uint32_t field_index = 0;       // Field position within the row
    std::vector<uint8_t> data;      // Raw field data
    double confidence = 0.0;        // How confident we are in this value
    bool is_complete = false;       // Whether the field is fully recovered
    std::string source;             // Source identifier (e.g., "page:42", "undo:15")

    std::string to_hex() const {
        std::ostringstream oss;
        for (auto b : data) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        }
        return oss.str();
    }
};

/**
 * @brief RowFragment — a partial or complete row from a recovery pass
 */
struct RowFragment {
    uint64_t fragment_id = 0;
    uint32_t table_id = 0;
    std::vector<uint8_t> primary_key;       // PK bytes for matching
    std::string primary_key_str;            // PK as string for comparison
    std::vector<FieldFragment> fields;      // Recovered fields
    double overall_confidence = 0.0;
    std::string recovery_method;            // "page_scan", "undo_log", "carved", "sdi"
    uint32_t page_no = 0;
    uint64_t timestamp = 0;

    bool has_pk() const { return !primary_key.empty(); }
    size_t field_count() const { return fields.size(); }
};

/**
 * @brief CorrelationConfig — configuration for data correlation
 */
struct CorrelationConfig {
    double min_confidence = 0.3;            // Minimum confidence to include
    double merge_threshold = 0.7;           // Confidence threshold for merging
    bool deduplicate_by_pk = true;          // Deduplicate rows with same PK
    bool prefer_higher_confidence = true;   // When merging, prefer higher confidence
    bool fill_missing_fields = true;        // Fill missing fields from other fragments
    uint32_t max_merge_sources = 10;        // Max sources to merge for one row
};

/**
 * @brief CorrelationResult — result of correlation/merge
 */
struct CorrelationResult {
    std::vector<RowFragment> merged_rows;   // Final merged rows
    uint32_t total_fragments = 0;
    uint32_t fragments_merged = 0;
    uint32_t fragments_deduped = 0;
    uint32_t rows_with_complete_fields = 0;
    uint32_t total_duplicates_removed = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "CorrelationResult{total_fragments=" << total_fragments
            << ", merged=" << fragments_merged
            << ", deduped=" << fragments_deduped
            << ", complete_rows=" << rows_with_complete_fields
            << ", duplicates_removed=" << total_duplicates_removed << "}";
        return oss.str();
    }
};

/**
 * @brief DataCorrelationEngine — correlates and merges row fragments
 *
 * Collects fragments from multiple recovery passes (page scan, undo log,
 * disk carving), correlates them by primary key, merges partial records,
 * and deduplicates.
 */
class DataCorrelationEngine {
public:
    DataCorrelationEngine() = default;

    /**
     * @brief Set correlation configuration
     */
    void set_config(const CorrelationConfig& config) { config_ = config; }

    /**
     * @brief Add a batch of fragments from a recovery pass
     * @param fragments Fragments to add
     * @param source_name Source identifier (e.g., "page_scan", "undo_log")
     */
    void add_fragments(const std::vector<RowFragment>& fragments,
                       const std::string& source_name);

    /**
     * @brief Correlate all collected fragments — merge and deduplicate
     * @return Correlation result
     */
    CorrelationResult correlate();

    /**
     * @brief Correlate a single batch of fragments (convenience method)
     * @param fragments Fragments to correlate
     * @return Correlated (merged + deduplicated) rows
     */
    std::vector<RowFragment> correlate_fragments(const std::vector<RowFragment>& fragments);

    /**
     * @brief Merge two partial fragments for the same row
     * @param a First fragment
     * @param b Second fragment
     * @return Merged fragment, or nullopt if incompatible
     */
    std::optional<RowFragment> merge_fragments(const RowFragment& a, const RowFragment& b);

    /**
     * @brief Deduplicate rows by primary key
     * @param rows Rows to deduplicate
     * @return Deduplicated rows
     */
    std::vector<RowFragment> deduplicate(const std::vector<RowFragment>& rows);

    /**
     * @brief Check if two fragments are likely the same row
     * @param a First fragment
     * @param b Second fragment
     * @return true if they match
     */
    bool is_same_row(const RowFragment& a, const RowFragment& b);

    /**
     * @brief Clear all collected fragments
     */
    void clear();

    /**
     * @brief Get total fragment count
     */
    size_t fragment_count() const { return all_fragments_.size(); }

private:
    CorrelationConfig config_;
    std::vector<RowFragment> all_fragments_;
    std::map<std::string, std::vector<RowFragment>> fragments_by_source_;

    /**
     * @brief Compute similarity between two byte vectors
     * @return Similarity score 0.0 - 1.0
     */
    double compute_similarity(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);

    /**
     * @brief Extract PK string from a fragment for grouping
     */
    std::string extract_pk_key(const RowFragment& fragment);
};

// ==================== Inline Implementation ====================

inline void DataCorrelationEngine::clear() {
    all_fragments_.clear();
    fragments_by_source_.clear();
}

inline std::string DataCorrelationEngine::extract_pk_key(const RowFragment& fragment) {
    if (!fragment.primary_key_str.empty()) return fragment.primary_key_str;
    if (fragment.has_pk()) {
        std::ostringstream oss;
        for (auto b : fragment.primary_key) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        }
        return oss.str();
    }
    return "";
}

inline double DataCorrelationEngine::compute_similarity(
    const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.empty() || b.empty()) return 0.0;

    size_t min_len = std::min(a.size(), b.size());
    size_t max_len = std::max(a.size(), b.size());
    if (min_len == 0) return 0.0;

    size_t matching_bytes = 0;
    for (size_t i = 0; i < min_len; i++) {
        if (a[i] == b[i]) matching_bytes++;
    }

    // Length difference penalty
    double len_ratio = static_cast<double>(min_len) / max_len;

    return (static_cast<double>(matching_bytes) / min_len) * len_ratio;
}

inline bool DataCorrelationEngine::is_same_row(const RowFragment& a, const RowFragment& b) {
    // If both have PKs, compare them
    if (a.has_pk() && b.has_pk()) {
        return extract_pk_key(a) == extract_pk_key(b);
    }

    // If only one has a PK, check if the other's fields match
    if (a.has_pk() != b.has_pk()) {
        const auto& with_pk = a.has_pk() ? a : b;
        const auto& without_pk = a.has_pk() ? b : a;

        // Check if any field values match
        for (const auto& f1 : with_pk.fields) {
            for (const auto& f2 : without_pk.fields) {
                if (f1.field_index == f2.field_index &&
                    compute_similarity(f1.data, f2.data) > 0.8) {
                    return true;
                }
            }
        }
        return false;
    }

    // Neither has PK — compare by field similarity
    if (a.fields.empty() || b.fields.empty()) return false;

    double total_sim = 0.0;
    size_t matched_fields = 0;

    for (const auto& f1 : a.fields) {
        for (const auto& f2 : b.fields) {
            if (f1.field_index == f2.field_index) {
                total_sim += compute_similarity(f1.data, f2.data);
                matched_fields++;
                break;
            }
        }
    }

    if (matched_fields == 0) return false;
    return (total_sim / matched_fields) > 0.75;
}

inline std::optional<RowFragment> DataCorrelationEngine::merge_fragments(
    const RowFragment& a, const RowFragment& b) {

    if (!is_same_row(a, b)) return std::nullopt;

    RowFragment merged = a.overall_confidence >= b.overall_confidence ? a : b;

    // Merge fields from both fragments
    std::map<uint32_t, const FieldFragment*> field_map;
    for (const auto& f : a.fields) {
        field_map[f.field_index] = &f;
    }
    for (const auto& f : b.fields) {
        auto it = field_map.find(f.field_index);
        if (it == field_map.end() || f.confidence > it->second->confidence) {
            field_map[f.field_index] = &f;
        }
    }

    merged.fields.clear();
    for (auto& [idx, fptr] : field_map) {
        merged.fields.push_back(*fptr);
    }

    // Sort fields by index
    std::sort(merged.fields.begin(), merged.fields.end(),
        [](const FieldFragment& a, const FieldFragment& b) {
            return a.field_index < b.field_index;
        });

    // Combined confidence
    merged.overall_confidence = std::max(a.overall_confidence, b.overall_confidence);

    return merged;
}

inline std::vector<RowFragment> DataCorrelationEngine::deduplicate(
    const std::vector<RowFragment>& rows) {

    std::vector<RowFragment> result;
    std::map<std::string, const RowFragment*> best_by_pk;

    for (const auto& row : rows) {
        if (!config_.deduplicate_by_pk || !row.has_pk()) {
            result.push_back(row);
            continue;
        }

        std::string pk_key = extract_pk_key(row);
        if (pk_key.empty()) {
            result.push_back(row);
            continue;
        }

        auto it = best_by_pk.find(pk_key);
        if (it == best_by_pk.end() ||
            (config_.prefer_higher_confidence && row.overall_confidence > it->second->overall_confidence)) {
            best_by_pk[pk_key] = &row;
        }
    }

    for (const auto& [pk, row_ptr] : best_by_pk) {
        result.push_back(*row_ptr);
    }

    return result;
}

inline void DataCorrelationEngine::add_fragments(
    const std::vector<RowFragment>& fragments, const std::string& source_name) {

    all_fragments_.insert(all_fragments_.end(), fragments.begin(), fragments.end());
    fragments_by_source_[source_name].insert(
        fragments_by_source_[source_name].end(), fragments.begin(), fragments.end());
}

inline std::vector<RowFragment> DataCorrelationEngine::correlate_fragments(
    const std::vector<RowFragment>& fragments) {

    std::map<std::string, RowFragment> merged_map;

    for (const auto& frag : fragments) {
        if (frag.overall_confidence < config_.min_confidence) continue;

        std::string key = extract_pk_key(frag);
        if (key.empty()) {
            // No PK — use source + field count as key
            key = "nopk_" + std::to_string(frag.page_no) + "_" + std::to_string(frag.field_count());
        }

        auto it = merged_map.find(key);
        if (it == merged_map.end()) {
            merged_map[key] = frag;
        } else {
            auto merged = merge_fragments(it->second, frag);
            if (merged.has_value()) {
                merged_map[key] = merged.value();
            }
        }
    }

    std::vector<RowFragment> result;
    for (auto& [key, row] : merged_map) {
        result.push_back(std::move(row));
    }

    return deduplicate(result);
}

inline CorrelationResult DataCorrelationEngine::correlate() {
    CorrelationResult result;
    result.total_fragments = all_fragments_.size();

    auto merged = correlate_fragments(all_fragments_);
    result.fragments_merged = merged.size();
    result.fragments_deduped = all_fragments_.size() - merged.size();

    // Count rows with complete fields
    for (const auto& row : merged) {
        if (row.overall_confidence >= 0.9) {
            result.rows_with_complete_fields++;
        }
    }

    result.merged_rows = std::move(merged);

    // Count duplicates removed
    result.total_duplicates_removed = result.total_fragments - result.merged_rows.size();

    return result;
}

} // namespace salvage
} // namespace dbrecovery

#endif // DBRECOVERY_SALVAGE_DATA_CORRELATION_HPP