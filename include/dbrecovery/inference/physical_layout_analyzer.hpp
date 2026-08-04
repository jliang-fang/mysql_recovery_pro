#ifndef DBRECOVERY_INFERENCE_PHYSICAL_LAYOUT_ANALYZER_HPP
#define DBRECOVERY_INFERENCE_PHYSICAL_LAOUT_ANALYZER_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <optional>

namespace dbrecovery {
namespace inference {

struct LayoutInfo {
    uint32_t total_columns = 0;         // Including system columns
    uint32_t user_columns = 0;          // total - 3 (TRX_ID, ROLL_PTR, ROW_ID)
    uint32_t null_bitmap_bytes = 0;
    uint32_t fixed_size_total = 0;
    std::vector<uint32_t> var_len_offsets;
    bool is_compact_format = true;
    double confidence = 0.0;
};

struct ColumnFeatures {
    uint32_t sample_count = 0;
    double null_ratio = 0.0;
    double zero_ratio = 0.0;
    double empty_ratio = 0.0;
    double len_min = 0, len_max = 0, len_mean = 0, len_stddev = 0;
    double ascii_ratio = 0.0;
    double digit_ratio = 0.0;
    double entropy = 0.0;
    bool is_fixed_length = false;
    uint32_t fixed_size = 0;
    std::vector<uint32_t> len_distribution;
};

struct TypeCandidate {
    std::string type_name;
    uint32_t innodb_type_id = 0;
    double confidence = 0.0;
    std::string reasoning;
};

struct InferenceResult {
    std::vector<std::vector<TypeCandidate>> column_candidates;
    std::string proposed_ddl;
    double overall_confidence = 0.0;
    std::vector<std::string> warnings;
    uint32_t detected_column_count = 0;
};

class PhysicalLayoutAnalyzer {
public:
    LayoutInfo analyze_page(const uint8_t* page_data, size_t page_size);
    LayoutInfo analyze_multiple_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);
};

class StatisticalTypeClassifier {
public:
    std::vector<TypeCandidate> classify(const ColumnFeatures& features);
private:
    double likelihood_tinyint(const ColumnFeatures& f);
    double likelihood_smallint(const ColumnFeatures& f);
    double likelihood_int(const ColumnFeatures& f);
    double likelihood_bigint(const ColumnFeatures& f);
    double likelihood_float(const ColumnFeatures& f);
    double likelihood_double(const ColumnFeatures& f);
    double likelihood_decimal(const ColumnFeatures& f);
    double likelihood_char(const ColumnFeatures& f);
    double likelihood_varchar(const ColumnFeatures& f);
    double likelihood_text(const ColumnFeatures& f);
    double likelihood_blob(const ColumnFeatures& f);
    double likelihood_datetime(const ColumnFeatures& f);
    double likelihood_date(const ColumnFeatures& f);
    double likelihood_time(const ColumnFeatures& f);
    double likelihood_timestamp(const ColumnFeatures& f);
    double likelihood_enum(const ColumnFeatures& f);
    double likelihood_set(const ColumnFeatures& f);
    double likelihood_json(const ColumnFeatures& f);
    double likelihood_year(const ColumnFeatures& f);
    double likelihood_bit(const ColumnFeatures& f);
};

class FeatureExtractor {
public:
    ColumnFeatures extract(
        const std::vector<std::vector<uint8_t>>& sample_values);
    std::vector<ColumnFeatures> extract_all(
        const std::vector<std::vector<std::vector<uint8_t>>>& columns_samples);
};

} // namespace inference
} // namespace dbrecovery

#endif