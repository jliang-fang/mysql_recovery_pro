#ifndef DBRECOVERY_INFERENCE_STATISTICAL_CLASSIFIER_HPP
#define DBRECOVERY_INFERENCE_STATISTICAL_CLASSIFIER_HPP

/**
 * @file statistical_classifier.hpp
 * @brief Statistical type classifier for schemaless column inference (v2.0)
 *
 * Analyzes raw binary column data from InnoDB pages to infer MySQL types
 * without requiring .frm or SDI metadata. Uses statistical feature extraction
 * and per-type likelihood scoring.
 *
 * Note: Implementations moved to src/inference/statistical_classifier.cpp
 */

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>

#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace inference {

// ==================== Column Features ====================

struct ColumnFeatures {
    uint32_t sample_count = 0;
    double null_ratio = 0.0;
    double zero_ratio = 0.0;
    double empty_ratio = 0.0;

    double len_min = 0.0;
    double len_max = 0.0;
    double len_mean = 0.0;
    double len_median = 0.0;
    double len_stddev = 0.0;
    std::vector<uint32_t> len_distribution;

    double ascii_ratio = 0.0;
    double digit_ratio = 0.0;
    double alpha_ratio = 0.0;
    double alphanum_ratio = 0.0;
    double has_sign_ratio = 0.0;
    double has_dot_ratio = 0.0;
    double has_colon_ratio = 0.0;
    double has_dash_ratio = 0.0;
    double has_space_ratio = 0.0;
    double has_at_ratio = 0.0;
    double has_slash_ratio = 0.0;
    double has_brace_ratio = 0.0;
    double has_bracket_ratio = 0.0;

    std::optional<uint64_t> numeric_min;
    std::optional<uint64_t> numeric_max;
    double numeric_ratio = 0.0;
    double integer_ratio = 0.0;
    double float_ratio = 0.0;
    double signed_ratio = 0.0;

    double entropy = 0.0;
    std::vector<uint32_t> byte_histogram;
    double repeated_pattern_score = 0.0;

    bool is_fixed_length = false;
    uint32_t fixed_size = 0;
    uint32_t distinct_count = 0;

    std::string to_string() const;
};

// ==================== Feature Extractor ====================

class FeatureExtractor {
public:
    static ColumnFeatures extract(const std::vector<std::vector<uint8_t>>& raw_values);

private:
    static double compute_entropy(const std::vector<uint32_t>& histogram, uint32_t sample_count, uint64_t total_bytes);
    static double compute_repeated_pattern_score(const std::vector<std::vector<uint8_t>>& values);
    static bool is_ascii_printable(uint8_t byte);
    static bool is_digit(uint8_t byte);
    static bool is_alpha(uint8_t byte);
};

// ==================== Type Candidate ====================

struct TypeCandidate {
    std::string type_name;
    uint32_t innodb_type_id;
    double confidence;
    std::string reasoning;

    std::string to_string() const;
};

// ==================== Statistical Type Classifier ====================

class StatisticalTypeClassifier {
public:
    StatisticalTypeClassifier() = default;

    std::vector<TypeCandidate> classify(const ColumnFeatures& features, size_t top_n = 3);

private:
    double likelihood_tinyint(const ColumnFeatures& f);
    double likelihood_smallint(const ColumnFeatures& f);
    double likelihood_mediumint(const ColumnFeatures& f);
    double likelihood_int(const ColumnFeatures& f);
    double likelihood_bigint(const ColumnFeatures& f);
    double likelihood_float_type(const ColumnFeatures& f);
    double likelihood_double_type(const ColumnFeatures& f);
    double likelihood_decimal(const ColumnFeatures& f);
    double likelihood_char(const ColumnFeatures& f);
    double likelihood_varchar(const ColumnFeatures& f);
    double likelihood_text(const ColumnFeatures& f);
    double likelihood_blob(const ColumnFeatures& f);
    double likelihood_datetime(const ColumnFeatures& f);
    double likelihood_date(const ColumnFeatures& f);
    double likelihood_time(const ColumnFeatures& f);
    double likelihood_timestamp(const ColumnFeatures& f);
    double likelihood_year(const ColumnFeatures& f);
    double likelihood_enum(const ColumnFeatures& f);
    double likelihood_set(const ColumnFeatures& f);
    double likelihood_json(const ColumnFeatures& f);
    double likelihood_bit(const ColumnFeatures& f);
    double likelihood_binary(const ColumnFeatures& f);

    static double score_range(double value, double ideal_min, double ideal_max, double tolerance);
    static double score_above(double value, double threshold);
    static double score_below(double value, double threshold);
    static double score_near(double value, double target, double tolerance);
};

// ==================== Inferred Column ====================

struct InferredColumnV2 {
    uint32_t ordinal_position = 0;
    std::string suggested_name;
    std::vector<TypeCandidate> candidates;
    TypeCandidate best_guess;
    bool is_nullable = true;
    bool is_primary_key_candidate = false;
    bool is_auto_increment_candidate = false;
    double overall_confidence = 0.0;
};

// ==================== Schema Inference Result ====================

struct SchemaInferenceResult {
    std::string proposed_table_name;
    uint32_t detected_column_count = 0;
    uint32_t detected_pk_count = 0;
    std::vector<InferredColumnV2> columns;
    std::string tentative_ddl;
    double overall_confidence = 0.0;
    std::vector<std::string> warnings;
    std::string inference_method;

    std::string to_report() const;
};

// ==================== Physical Layout Analyzer ====================

class PhysicalLayoutAnalyzer {
public:
    struct LayoutInfo {
        uint32_t total_columns = 0;
        uint32_t user_columns = 0;
        uint32_t null_bitmap_bytes = 0;
        uint32_t fixed_size_total = 0;
        std::vector<uint32_t> var_len_offsets;
        bool is_compact_format = true;
        uint32_t row_format = 0;
    };

    static std::optional<LayoutInfo> analyze_page(
        const uint8_t* page_data, size_t page_size);

    static LayoutInfo analyze_multiple_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);

    static std::vector<std::vector<std::vector<uint8_t>>> extract_column_values(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages,
        const LayoutInfo& layout,
        uint32_t max_samples = 5000);
};

} // namespace inference
} // namespace dbrecovery

#endif // DBRECOVERY_INFERENCE_STATISTICAL_CLASSIFIER_HPP