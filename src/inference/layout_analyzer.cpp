/**
 * @file layout_analyzer.cpp
 * @brief LayoutAnalyzer implementation
 *
 * Orchestrates PhysicalLayoutAnalyzer and StatisticalTypeClassifier to
 * produce a unified layout description with confidence scores. Resolves
 * conflicts across multiple pages via majority vote.
 */

#include "dbrecovery/inference/layout_analyzer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <iomanip>

namespace dbrecovery {
namespace inference {

namespace {

double mean_confidence(const std::vector<InferredColumnV2>& cols) {
    if (cols.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& c : cols) sum += c.overall_confidence;
    return sum / static_cast<double>(cols.size());
}

} // namespace

// ==================== UnifiedLayout ====================

std::string UnifiedLayout::to_string() const {
    std::ostringstream oss;
    oss << "UnifiedLayout{cols=" << total_columns
        << ", user=" << user_columns
        << ", null_bmp=" << null_bitmap_bytes
        << ", fixed=" << fixed_size_total
        << ", format=" << (is_compact_format ? "compact" : "row")
        << ", conf=" << std::fixed << std::setprecision(2) << confidence
        << "}";
    return oss.str();
}

// ==================== LayoutAnalyzer ====================

std::optional<UnifiedLayout> LayoutAnalyzer::analyze_page(
    const uint8_t* page_data, size_t page_size) {

    auto layout_opt = PhysicalLayoutAnalyzer::analyze_page(page_data, page_size);
    if (!layout_opt.has_value()) return std::nullopt;

    const auto& layout = layout_opt.value();

    UnifiedLayout result;
    result.total_columns = layout.total_columns;
    result.user_columns = layout.user_columns;
    result.null_bitmap_bytes = layout.null_bitmap_bytes;
    result.fixed_size_total = layout.fixed_size_total;
    result.var_len_offsets = layout.var_len_offsets;
    result.is_compact_format = layout.is_compact_format;
    result.row_format = layout.row_format;

    // Extract column values from this single page and infer types.
    std::vector<std::pair<const uint8_t*, size_t>> pages = {
        {page_data, page_size}};
    auto column_values = PhysicalLayoutAnalyzer::extract_column_values(
        pages, layout, 2000);

    result.inferred_columns = infer_column_types(
        column_values, layout.total_columns);

    result.confidence = mean_confidence(result.inferred_columns);
    if (result.inferred_columns.empty()) result.confidence = 0.3;

    return result;
}

UnifiedLayout LayoutAnalyzer::analyze_pages(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages) {

    if (pages.empty()) return UnifiedLayout{};

    // Collect per-page layout votes.
    std::vector<PhysicalLayoutAnalyzer::LayoutInfo> layouts;
    for (const auto& [data, size] : pages) {
        auto opt = PhysicalLayoutAnalyzer::analyze_page(data, size);
        if (opt.has_value()) layouts.push_back(opt.value());
    }

    UnifiedLayout result;
    if (!layouts.empty()) {
        result = resolve_layouts(layouts);
    } else {
        result = UnifiedLayout{};
    }

    // Extract column values using the resolved layout and infer types.
    auto column_values = PhysicalLayoutAnalyzer::extract_column_values(
        pages, PhysicalLayoutAnalyzer::LayoutInfo{
            result.total_columns, result.user_columns,
            result.null_bitmap_bytes, result.fixed_size_total,
            result.var_len_offsets, result.is_compact_format, result.row_format},
        5000);

    result.inferred_columns = infer_column_types(
        column_values, result.total_columns);

    result.confidence = mean_confidence(result.inferred_columns);
    if (result.inferred_columns.empty()) result.confidence = 0.3;

    return result;
}

UnifiedLayout LayoutAnalyzer::resolve_layouts(
    const std::vector<PhysicalLayoutAnalyzer::LayoutInfo>& layouts) {

    if (layouts.empty()) return UnifiedLayout{};

    // Majority vote on column count.
    std::map<uint32_t, uint32_t> votes;
    for (const auto& l : layouts) votes[l.total_columns]++;

    uint32_t best_cols = 0, best_votes = 0;
    for (const auto& [cols, count] : votes) {
        if (count > best_votes) {
            best_votes = count;
            best_cols = cols;
        }
    }

    // Pick the representative layout with the winning column count.
    const PhysicalLayoutAnalyzer::LayoutInfo* selected = nullptr;
    for (const auto& l : layouts) {
        if (l.total_columns == best_cols) {
            selected = &l;
            break;
        }
    }
    if (!selected) selected = &layouts[0];

    UnifiedLayout result;
    result.total_columns = selected->total_columns;
    result.user_columns = selected->user_columns;
    result.null_bitmap_bytes = selected->null_bitmap_bytes;
    result.fixed_size_total = selected->fixed_size_total;
    result.var_len_offsets = selected->var_len_offsets;
    result.is_compact_format = selected->is_compact_format;
    result.row_format = selected->row_format;

    // Confidence: agreement ratio across pages + base layout confidence.
    double agreement = static_cast<double>(best_votes) /
                       static_cast<double>(layouts.size());
    result.confidence = 0.5 + 0.5 * agreement;

    return result;
}

std::vector<InferredColumnV2> LayoutAnalyzer::infer_column_types(
    const std::vector<std::vector<std::vector<uint8_t>>>& column_values,
    uint32_t column_count) {

    std::vector<InferredColumnV2> result;
    result.reserve(column_values.size());

    for (uint32_t i = 0; i < column_values.size() && i < column_count; i++) {
        const auto& values = column_values[i];

        InferredColumnV2 col;
        col.ordinal_position = i + 1;
        col.suggested_name = "column_" + std::to_string(i + 1);

        if (values.empty()) {
            col.best_guess = {"UNKNOWN", 0, 0.0, "no samples"};
            col.overall_confidence = 0.0;
        } else {
            auto features = FeatureExtractor::extract(values);
            auto candidates = classifier_.classify(features, 3);

            bool non_null = (features.null_ratio < 0.05);
            bool all_distinct = (features.distinct_count == features.sample_count);
            bool increasing = features.numeric_min.has_value() &&
                features.numeric_max.has_value() &&
                features.numeric_max.value() == features.sample_count;

            col.is_nullable = !non_null;
            col.is_primary_key_candidate = (non_null && all_distinct);
            col.is_auto_increment_candidate = (non_null && increasing);

            col.candidates = candidates;
            if (!candidates.empty()) {
                col.best_guess = candidates[0];
            } else {
                col.best_guess = {"UNKNOWN", 0, 0.0, "no confident type"};
            }
            col.overall_confidence =
                col.best_guess.confidence * (0.8 + 0.2 * (1.0 - features.null_ratio));
            if (col.overall_confidence > 1.0) col.overall_confidence = 1.0;
        }

        result.push_back(std::move(col));
    }

    return result;
}

} // namespace inference
} // namespace dbrecovery