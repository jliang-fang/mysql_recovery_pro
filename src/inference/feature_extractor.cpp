/**
 * @file feature_extractor.cpp
 * @brief MultiPageFeatureExtractor implementation
 *
 * Orchestrates StatisticalClassifier::FeatureExtractor over multiple pages,
 * aggregates per-page column features, and computes cross-page consistency.
 */

#include "dbrecovery/inference/feature_extractor.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace dbrecovery {
namespace inference {

// ==================== MultiPageFeatures ====================

std::string MultiPageFeatures::to_string() const {
    std::ostringstream oss;
    oss << "MultiPageFeatures{pages=" << page_count
        << ", consistency=" << std::fixed << std::setprecision(2)
        << cross_page_consistency
        << ", cols=" << aggregated.sample_count << "}";
    return oss.str();
}

// ==================== MultiPageFeatureExtractor ====================

ColumnFeatures MultiPageFeatureExtractor::extract(
    const std::vector<std::vector<uint8_t>>& raw_values) {
    return FeatureExtractor::extract(raw_values);
}

MultiPageFeatures MultiPageFeatureExtractor::extract_multi_page(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages,
    const PhysicalLayoutAnalyzer::LayoutInfo& layout,
    uint32_t max_samples) {

    MultiPageFeatures result;
    result.page_count = static_cast<uint32_t>(pages.size());

    // Extract raw column values across all pages, then aggregate cumulative
    // features per page for consistency measurement.
    auto column_values = PhysicalLayoutAnalyzer::extract_column_values(
        pages, layout, max_samples);

    uint32_t col_count = layout.total_columns;

    // Per-page feature extraction requires re-walking records page by page.
    // We sample each page's column values independently and cache them.
    clear_cache();

    uint32_t samples_used = 0;
    for (const auto& [data, size] : pages) {
        if (samples_used >= max_samples) break;
        if (size < 120) continue;

        uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
        if (page_type != innodb::FIL_PAGE_INDEX) continue;

        // Build a single-page value set for this page.
        std::vector<std::vector<std::vector<uint8_t>>> page_col_values(col_count);
        uint16_t rec_off = (static_cast<uint16_t>(data[101]) << 8) | data[100];
        uint16_t heap_top = (static_cast<uint16_t>(data[41]) << 8) | data[40];
        uint16_t n_recs = (static_cast<uint16_t>(data[55]) << 8) | data[54];

        for (uint16_t rec_idx = 0; rec_idx < n_recs && samples_used < max_samples; rec_idx++) {
            if (rec_off < 120 || rec_off >= heap_top) break;

            std::vector<uint16_t> field_offsets;
            uint16_t pos = rec_off;
            for (uint32_t i = 0; i < col_count && pos + 2 <= size; i++) {
                uint16_t val = (static_cast<uint16_t>(data[pos + 1]) << 8) | data[pos];
                if (val == 0 || val > size) break;
                field_offsets.push_back(val);
                pos += 2;
            }
            if (field_offsets.size() < col_count) break;

            uint16_t data_start = rec_off + col_count * 2 + 1 + layout.null_bitmap_bytes;
            for (uint32_t i = 0; i < col_count && i < page_col_values.size(); i++) {
                uint16_t field_start, field_end;
                if (i + 1 < field_offsets.size()) {
                    field_start = rec_off + field_offsets[i + 1];
                    field_end = rec_off + field_offsets[i];
                } else {
                    field_start = data_start;
                    field_end = rec_off + field_offsets[i];
                }
                if (field_end > field_start && field_end <= size && field_start >= data_start) {
                    page_col_values[i].emplace_back(
                        data + field_start, data + field_end);
                }
            }

            samples_used++;

            uint16_t next_rec = (static_cast<uint16_t>(data[rec_off + 1]) << 8)
                              | data[rec_off];
            if (next_rec <= rec_off || next_rec >= heap_top) break;
            rec_off = next_rec;
        }

        // Extract features for this page (only for columns with data).
        ColumnFeatures page_feat;
        bool has_any = false;
        for (uint32_t i = 0; i < col_count; i++) {
            if (page_col_values[i].empty()) continue;
            has_any = true;
            break;
        }
        if (has_any) {
            // Aggregate all columns' values into a composite feature set for
            // the page-level consistency metric.
            std::vector<std::vector<uint8_t>> all;
            for (uint32_t i = 0; i < col_count; i++) {
                all.insert(all.end(), page_col_values[i].begin(),
                           page_col_values[i].end());
            }
            page_feat = FeatureExtractor::extract(all);
            cached_pages_.push_back(page_feat);
        }
    }

    // Aggregate across all columns using the cumulative value set.
    result.aggregated = FeatureExtractor::extract(
        flatten_columns(column_values));
    result.cross_page_consistency =
        compute_cross_page_consistency(cached_pages_);

    return result;
}

void MultiPageFeatureExtractor::clear_cache() {
    cached_pages_.clear();
}

double MultiPageFeatureExtractor::compute_cross_page_consistency(
    const std::vector<ColumnFeatures>& page_features) {

    if (page_features.size() < 2) return 1.0;

    double sum = 0.0;
    double mean = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < page_features.size(); i++) {
        for (size_t j = i + 1; j < page_features.size(); j++) {
            const auto& a = page_features[i];
            const auto& b = page_features[j];
            double diff = 0.0;
            if (a.len_mean > 0 || b.len_mean > 0) {
                diff += std::fabs(a.len_mean - b.len_mean) /
                        std::max(a.len_mean, b.len_mean);
            }
            diff += std::fabs(a.null_ratio - b.null_ratio);
            diff += std::fabs(a.ascii_ratio - b.ascii_ratio);
            diff += std::fabs(a.integer_ratio - b.integer_ratio);
            double sim = std::max(0.0, 1.0 - diff);
            sum += sim;
            n++;
        }
    }

    return n > 0 ? sum / static_cast<double>(n) : 1.0;
}

std::vector<std::vector<uint8_t>> MultiPageFeatureExtractor::flatten_columns(
    const std::vector<std::vector<std::vector<uint8_t>>>& column_values) {

    std::vector<std::vector<uint8_t>> flat;
    for (const auto& col : column_values) {
        flat.insert(flat.end(), col.begin(), col.end());
    }
    return flat;
}

} // namespace inference
} // namespace dbrecovery