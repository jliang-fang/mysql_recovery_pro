#ifndef DBRECOVERY_INFERENCE_LAYOUT_ANALYZER_HPP
#define DBRECOVERY_INFERENCE_LAYOUT_ANALYZER_HPP

/**
 * @file layout_analyzer.hpp
 * @brief LayoutAnalyzer — unified page layout analysis
 *
 * v2.0 architecture: combines PhysicalLayoutAnalyzer with StatisticalClassifier
 * to produce unified layout descriptions with confidence scores across
 * multiple pages. Handles page-level deduplication and conflict resolution.
 */

#include "dbrecovery/inference/physical_layout_analyzer.hpp"
#include "dbrecovery/inference/statistical_classifier.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>

namespace dbrecovery {
namespace inference {

/**
 * @brief Unified layout description with confidence
 */
struct UnifiedLayout {
    uint32_t total_columns = 0;
    uint32_t user_columns = 0;
    uint32_t null_bitmap_bytes = 0;
    uint32_t fixed_size_total = 0;
    std::vector<uint32_t> var_len_offsets;
    bool is_compact_format = true;
    uint32_t row_format = 0;

    // Confidence score [0.0, 1.0]
    double confidence = 0.0;

    // Per-column type inferences
    std::vector<InferredColumnV2> inferred_columns;

    std::string to_string() const;
};

/**
 * @brief LayoutAnalyzer — combines layout detection with type inference
 *
 * Orchestrates PhysicalLayoutAnalyzer and StatisticalTypeClassifier
 * to produce a unified layout description with confidence scores.
 * Handles conflict resolution when multiple pages disagree.
 */
class LayoutAnalyzer {
public:
    LayoutAnalyzer() = default;

    /**
     * @brief Analyze a single page — detect layout + infer types
     */
    std::optional<UnifiedLayout> analyze_page(
        const uint8_t* page_data, size_t page_size);

    /**
     * @brief Analyze multiple pages — cross-validate layout + aggregate types
     */
    UnifiedLayout analyze_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);

    /**
     * @brief Get the StatisticalTypeClassifier for custom inference
     */
    StatisticalTypeClassifier& classifier() { return classifier_; }

private:
    StatisticalTypeClassifier classifier_;

    /**
     * @brief Resolve layout conflicts across multiple pages (majority vote)
     */
    UnifiedLayout resolve_layouts(
        const std::vector<PhysicalLayoutAnalyzer::LayoutInfo>& layouts);

    /**
     * @brief Infer column types from extracted values
     */
    std::vector<InferredColumnV2> infer_column_types(
        const std::vector<std::vector<std::vector<uint8_t>>>& column_values,
        uint32_t column_count);
};

} // namespace inference
} // namespace dbrecovery

#endif // DBRECOVERY_INFERENCE_LAYOUT_ANALYZER_HPP