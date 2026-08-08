#ifndef DBRECOVERY_INFERENCE_FEATURE_EXTRACTOR_HPP
#define DBRECOVERY_INFERENCE_FEATURE_EXTRACTOR_HPP

/**
 * @file feature_extractor.hpp
 * @brief FeatureExtractor — extracts column-level features from raw page data
 *
 * v2.0 architecture: provides cross-page feature extraction by orchestrating
 * the lower-level StatisticalClassifier::FeatureExtractor over multiple pages.
 * Features include: type heuristics, value distributions, null ratios,
 * byte-level statistics, and cross-column correlation metrics.
 */

#include "dbrecovery/inference/statistical_classifier.hpp"
#include "dbrecovery/inference/physical_layout_analyzer.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>

namespace dbrecovery {
namespace inference {

/**
 * @brief Cross-page feature extraction result
 */
struct MultiPageFeatures {
    uint32_t page_count = 0;
    std::vector<ColumnFeatures> per_page_features;
    ColumnFeatures aggregated;   // Statistics aggregated across all pages
    double cross_page_consistency = 0.0;

    std::string to_string() const;
};

/**
 * @brief MultiPageFeatureExtractor — orchestrates low-level feature extraction
 *
 * Wraps the StatisticalClassifier::FeatureExtractor and adds:
 * - Cross-page feature aggregation
 * - Caching of per-page features
 * - Consistency scoring across pages
 */
class MultiPageFeatureExtractor {
public:
    MultiPageFeatureExtractor() = default;

    /**
     * @brief Extract features from column values (single page)
     */
    ColumnFeatures extract(const std::vector<std::vector<uint8_t>>& raw_values);

    /**
     * @brief Extract features across multiple pages
     * @param pages  Vector of (page_data, page_size) pairs
     * @param layout  Physical layout info from PhysicalLayoutAnalyzer
     * @param max_samples  Maximum samples per column
     * @return Cross-page feature set
     */
    MultiPageFeatures extract_multi_page(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages,
        const PhysicalLayoutAnalyzer::LayoutInfo& layout,
        uint32_t max_samples = 5000);

    /**
     * @brief Clear cached features
     */
    void clear_cache();

    size_t cached_page_count() const { return cached_pages_.size(); }

private:
    std::vector<ColumnFeatures> cached_pages_;

    double compute_cross_page_consistency(
        const std::vector<ColumnFeatures>& page_features);

    static std::vector<std::vector<uint8_t>> flatten_columns(
        const std::vector<std::vector<std::vector<uint8_t>>>& column_values);
};

} // namespace inference
} // namespace dbrecovery

#endif // DBRECOVERY_INFERENCE_FEATURE_EXTRACTOR_HPP