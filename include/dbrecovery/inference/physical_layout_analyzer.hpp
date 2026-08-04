#ifndef DBRECOVERY_INFERENCE_PHYSICAL_LAYOUT_ANALYZER_HPP
#define DBRECOVERY_INFERENCE_PHYSICAL_LAOUT_ANALYZER_HPP

/**
 * @file physical_layout_analyzer.hpp
 * @brief Physical layout analysis for InnoDB pages (v1 backward compat)
 *
 * PhysicalLayoutAnalyzer and LayoutInfo are now defined in
 * statistical_classifier.hpp (v2). This header re-exports them.
 * InferenceResult remains here for v1 backward compatibility.
 */

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <optional>

#include "dbrecovery/inference/statistical_classifier.hpp"

namespace dbrecovery {
namespace inference {

// Re-export v2 types for backward compatibility
using PhysicalLayoutAnalyzer = dbrecovery::inference::PhysicalLayoutAnalyzer;
using LayoutInfo = PhysicalLayoutAnalyzer::LayoutInfo;

struct InferenceResult {
    std::vector<std::vector<TypeCandidate>> column_candidates;
    std::string proposed_ddl;
    double overall_confidence = 0.0;
    std::vector<std::string> warnings;
    uint32_t detected_column_count = 0;
};

} // namespace inference
} // namespace dbrecovery

#endif // DBRECOVERY_INFERENCE_PHYSICAL_LAYOUT_ANALYZER_HPP