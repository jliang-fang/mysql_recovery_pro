#ifndef DBRECOVERY_INFERENCE_INTERACTIVE_BUILDER_HPP
#define DBRECOVERY_INFERENCE_INTERACTIVE_BUILDER_HPP

// InteractiveBuilder provides an interactive schema refinement workflow:
// users can confirm/reject inferred column types, adjust DDL, and
// re-run recovery with the updated schema.
#include "dbrecovery/inference/statistical_classifier.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <functional>

namespace dbrecovery {
namespace inference {

/**
 * @brief Interactive schema refinement builder
 *
 * Combines StatisticalTypeClassifier (feature extraction + type inference)
 * with PhysicalLayoutAnalyzer (page-level layout detection) to provide an
 * interactive workflow for schema inference:
 *
 * 1. Load raw page data → analyze layout → extract column features
 * 2. Infer column types with confidence scores
 * 3. Present candidates to user for confirmation/rejection
 * 4. Apply user corrections and regenerate DDL
 * 5. Export the final schema for use in recovery
 */
class InteractiveBuilder {
public:
    InteractiveBuilder() = default;
    ~InteractiveBuilder() = default;

    // ---- Page data loading ----
    /**
     * @brief Load raw page data from an IBD file for analysis
     * @return Number of pages loaded
     */
    size_t load_ibd_file(const std::string& path);

    /**
     * @brief Add raw page data directly
     */
    void add_page_data(const uint8_t* data, size_t size);

    /**
     * @brief Add a page's raw column values for inference
     */
    void add_column_samples(const std::vector<std::vector<uint8_t>>& raw_values);

    // ---- Layout analysis ----
    /**
     * @brief Analyze page layout to detect column count and structure
     */
    std::optional<PhysicalLayoutAnalyzer::LayoutInfo> analyze_layout();

    /**
     * @brief Extract column values from loaded pages using detected layout
     */
    std::vector<std::vector<std::vector<uint8_t>>> extract_column_values(
        const PhysicalLayoutAnalyzer::LayoutInfo& layout,
        uint32_t max_samples = 5000);

    // ---- Type inference ----
    /**
     * @brief Infer column types from extracted raw values
     * @return Inferred schema with confidence scores per column
     */
    SchemaInferenceResult infer_types(
        const std::vector<std::vector<std::vector<uint8_t>>>& column_values,
        const std::vector<std::string>& column_hints = {});

    // ---- Interactive refinement ----
    struct UserCorrection {
        uint32_t ordinal_position;
        std::string corrected_type_name;  // e.g. "INT", "VARCHAR(255)"
        std::optional<std::string> corrected_name;
        bool is_nullable = true;
    };

    /**
     * @brief Apply a user correction to the inferred schema
     */
    void apply_correction(const UserCorrection& correction);

    /**
     * @brief Confirm a column's inferred type (marks it as accepted)
     */
    void confirm_column(uint32_t ordinal_position);

    /**
     * @brief Reject a column's inferred type and re-run inference
     */
    void reject_column(uint32_t ordinal_position);

    /**
     * @brief Get current inference state (all columns with their acceptance status)
     */
    struct ColumnState {
        InferredColumnV2 inferred;
        bool accepted = false;
        bool rejected = false;
        std::optional<UserCorrection> correction;
    };

    const std::vector<ColumnState>& column_states() const { return column_states_; }

    // ---- DDL generation ----
    /**
     * @brief Generate CREATE TABLE DDL from the current (possibly corrected) schema
     */
    std::string generate_ddl(const std::string& table_name = "recovered_table") const;

    /**
     * @brief Export the current schema as a catalog::TableSchema
     */
    catalog::TableSchema export_schema(const std::string& table_name = "recovered_table") const;

    // ---- Result ----
    const SchemaInferenceResult& inference_result() const { return inference_result_; }

    // ---- Reset ----
    void clear();

private:
    // Raw page data pool
    std::vector<std::pair<const uint8_t*, size_t>> page_data_owned_;
    std::vector<std::pair<const uint8_t*, size_t>> page_data_views_;

    // Column samples per column (ordinal → raw values)
    std::vector<std::vector<std::vector<uint8_t>>> column_samples_;

    // Layout info
    std::optional<PhysicalLayoutAnalyzer::LayoutInfo> layout_;

    // Inference result
    SchemaInferenceResult inference_result_;

    // Interactive state
    std::vector<ColumnState> column_states_;

    // Classifier
    StatisticalTypeClassifier classifier_;
    FeatureExtractor extractor_;
};

} // namespace inference
} // namespace dbrecovery

#endif // DBRECOVERY_INFERENCE_INTERACTIVE_BUILDER_HPP