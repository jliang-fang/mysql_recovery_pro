#ifndef DBRECOVERY_INFERENCE_STATISTICAL_CLASSIFIER_HPP
#define DBRECOVERY_INFERENCE_STATISTICAL_CLASSIFIER_HPP

/**
 * @file statistical_classifier.hpp
 * @brief Statistical type classifier for schemaless column inference (v2.0)
 *
 * Analyzes raw binary column data from InnoDB pages to infer MySQL types
 * without requiring .frm or SDI metadata. Uses statistical feature extraction
 * and per-type likelihood scoring.
 */

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <cmath>
#include <algorithm>
#include <numeric>
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
    std::vector<uint8_t> byte_histogram;
    double repeated_pattern_score = 0.0;

    bool is_fixed_length = false;
    uint32_t fixed_size = 0;
    uint32_t distinct_count = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "Features{n=" << sample_count
            << " len=[" << len_min << "-" << len_max << "] mean=" << len_mean
            << " std=" << len_stddev
            << " null=" << std::fixed << std::setprecision(2) << null_ratio
            << " digit=" << digit_ratio
            << " int=" << integer_ratio
            << " float=" << float_ratio
            << " ascii=" << ascii_ratio
            << " entropy=" << entropy
            << " distinct=" << distinct_count
            << " fixed=" << (is_fixed_length ? "Y" : "N")
            << "}";
        return oss.str();
    }
};

// ==================== Feature Extractor ====================

class FeatureExtractor {
public:
    /**
     * @brief Extract features from a set of raw column values
     * @param raw_values Vector of raw byte vectors for one column across sampled rows
     * @return Computed feature vector
     */
    static ColumnFeatures extract(const std::vector<std::vector<uint8_t>>& raw_values);

private:
    static double compute_entropy(const std::vector<uint8_t>& histogram, uint32_t total);
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

    std::string to_string() const {
        std::ostringstream oss;
        oss << type_name << " (" << std::fixed << std::setprecision(1)
            << confidence * 100 << "%): " << reasoning;
        return oss.str();
    }
};

// ==================== Statistical Type Classifier ====================

class StatisticalTypeClassifier {
public:
    StatisticalTypeClassifier() = default;

    /**
     * @brief Classify a column's features, returning ranked type candidates
     * @param features Extracted column features
     * @param top_n Number of top candidates to return (default 3)
     * @return Ranked type candidates with confidence scores
     */
    std::vector<TypeCandidate> classify(const ColumnFeatures& features, size_t top_n = 3);

private:
    // Likelihood functions — each returns a score in [0.0, 1.0]
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

    // Scoring helpers
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

    std::string to_report() const {
        std::ostringstream oss;
        oss << "Schema Inference Report\n";
        oss << "=======================\n";
        oss << "Table: " << proposed_table_name << "\n";
        oss << "Columns detected: " << detected_column_count << "\n";
        oss << "PK columns: " << detected_pk_count << "\n";
        oss << "Overall confidence: " << std::fixed << std::setprecision(1)
            << overall_confidence * 100 << "%\n";
        oss << "Method: " << inference_method << "\n\n";

        for (const auto& col : columns) {
            oss << "  Column " << col.ordinal_position << ": "
                << col.suggested_name << "\n";
            oss << "    Best: " << col.best_guess.to_string() << "\n";
            for (size_t i = 1; i < col.candidates.size(); i++) {
                oss << "    Alt:  " << col.candidates[i].to_string() << "\n";
            }
            if (col.is_primary_key_candidate) oss << "    [PK candidate]\n";
            if (col.is_auto_increment_candidate) oss << "    [AUTO_INCREMENT candidate]\n";
            oss << "\n";
        }

        if (!warnings.empty()) {
            oss << "Warnings:\n";
            for (const auto& w : warnings) {
                oss << "  - " << w << "\n";
            }
        }

        return oss.str();
    }
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
        uint32_t row_format = 0; // 0=REDUNDANT, 1=COMPACT, 2=DYNAMIC
    };

    /**
     * @brief Analyze physical layout from a page of raw records
     *
     * Examines the offset table, NULL bitmap, and record structure
     * to determine column count and layout without metadata.
     *
     * @param page_data Raw page buffer
     * @param page_size Page size
     * @return Detected layout or nullopt if page is unreadable
     */
    static std::optional<LayoutInfo> analyze_page(
        const uint8_t* page_data, size_t page_size);

    /**
     * @brief Analyze multiple pages and vote for consensus layout
     */
    static LayoutInfo analyze_multiple_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);

    /**
     * @brief Extract raw column values from records using inferred layout
     */
    static std::vector<std::vector<std::vector<uint8_t>>> extract_column_values(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages,
        const LayoutInfo& layout,
        uint32_t max_samples = 5000);
};

// ==================== FeatureExtractor Implementation ====================

inline ColumnFeatures FeatureExtractor::extract(
    const std::vector<std::vector<uint8_t>>& raw_values) {

    ColumnFeatures f;
    f.sample_count = static_cast<uint32_t>(raw_values.size());
    if (f.sample_count == 0) return f;

    std::vector<double> lengths;
    std::vector<uint8_t> byte_hist(256, 0);
    uint32_t null_count = 0, zero_count = 0, empty_count = 0;
    uint32_t ascii_count = 0, digit_count = 0, alpha_count = 0, alphanum_count = 0;
    uint32_t signed_cnt = 0, dot_cnt = 0, colon_cnt = 0, dash_cnt = 0;
    uint32_t space_cnt = 0, at_cnt = 0, slash_cnt = 0;
    uint32_t brace_cnt = 0, bracket_cnt = 0;
    uint32_t numeric_cnt = 0, integer_cnt = 0, float_cnt = 0;
    uint64_t num_min = UINT64_MAX, num_max = 0;
    std::set<std::vector<uint8_t>> distinct_values;

    for (const auto& bytes : raw_values) {
        if (bytes.empty()) {
            null_count++;
            lengths.push_back(0);
            zero_count++;
            empty_count++;
            continue;
        }

        distinct_values.insert(bytes);
        lengths.push_back(static_cast<double>(bytes.size()));

        bool all_zero = true;
        bool has_digit_char = false;
        bool has_alpha_char = false;
        bool has_sign = false;
        bool has_dot = false;
        bool has_colon = false;
        bool has_dash = false;
        bool has_space = false;
        bool has_at = false;
        bool has_slash = false;
        bool has_brace = false;
        bool has_bracket = false;
        bool all_ascii = true;
        uint32_t digit_count_in_val = 0;
        uint32_t alpha_count_in_val = 0;

        for (uint8_t b : bytes) {
            byte_hist[b]++;
            if (b != 0) all_zero = false;
            if (!is_ascii_printable(b)) all_ascii = false;
            if (is_digit(b)) { has_digit_char = true; digit_count_in_val++; }
            if (is_alpha(b)) { has_alpha_char = true; alpha_count_in_val++; }
            if (b == '-') has_sign = true;
            if (b == '.') has_dot = true;
            if (b == ':') has_colon = true;
            if (b == '-') has_dash = true;
            if (b == ' ') has_space = true;
            if (b == '@') has_at = true;
            if (b == '/') has_slash = true;
            if (b == '{' || b == '}') has_brace = true;
            if (b == '[' || b == ']') has_bracket = true;
        }

        if (all_zero) zero_count++;
        if (all_ascii) ascii_count++;
        if (has_digit_char && !has_alpha_char) digit_count++;
        if (has_alpha_char && !has_digit_char) alpha_count++;
        if (has_digit_char || has_alpha_char) alphanum_count++;
        if (has_sign) signed_cnt++;
        if (has_dot) dot_cnt++;
        if (has_colon) colon_cnt++;
        if (has_dash) dash_cnt++;
        if (has_space) space_cnt++;
        if (has_at) at_cnt++;
        if (has_slash) slash_cnt++;
        if (has_brace) brace_cnt++;
        if (has_bracket) bracket_cnt++;

        // Numeric parsing
        if (has_digit_char && !has_alpha_char && !has_slash && !has_at && !has_colon && !has_brace && !has_bracket) {
            std::string val_str(bytes.begin(), bytes.end());
            try {
                if (has_dot) {
                    double dval = std::stod(val_str);
                    float_cnt++;
                    if (dval >= 0) {
                        auto uval = static_cast<uint64_t>(dval);
                        num_min = std::min(num_min, uval);
                        num_max = std::max(num_max, uval);
                    }
                } else {
                    uint64_t uval = std::stoull(val_str);
                    integer_cnt++;
                    num_min = std::min(num_min, uval);
                    num_max = std::max(num_max, uval);
                }
                numeric_cnt++;
            } catch (...) {}
        }
    }

    uint32_t n = f.sample_count;
    f.null_ratio = static_cast<double>(null_count) / n;
    f.zero_ratio = static_cast<double>(zero_count) / n;
    f.empty_ratio = static_cast<double>(empty_count) / n;

    if (!lengths.empty()) {
        std::sort(lengths.begin(), lengths.end());
        f.len_min = lengths.front();
        f.len_max = lengths.back();
        f.len_mean = std::accumulate(lengths.begin(), lengths.end(), 0.0) / lengths.size();
        f.len_median = lengths[lengths.size() / 2];
        double sq_sum = 0;
        for (auto l : lengths) sq_sum += (l - f.len_mean) * (l - f.len_mean);
        f.len_stddev = std::sqrt(sq_sum / lengths.size());
    }

    f.ascii_ratio = static_cast<double>(ascii_count) / n;
    f.digit_ratio = static_cast<double>(digit_count) / n;
    f.alpha_ratio = static_cast<double>(alpha_count) / n;
    f.alphanum_ratio = static_cast<double>(alphanum_count) / n;
    f.has_sign_ratio = static_cast<double>(signed_cnt) / n;
    f.has_dot_ratio = static_cast<double>(dot_cnt) / n;
    f.has_colon_ratio = static_cast<double>(colon_cnt) / n;
    f.has_dash_ratio = static_cast<double>(dash_cnt) / n;
    f.has_space_ratio = static_cast<double>(space_cnt) / n;
    f.has_at_ratio = static_cast<double>(at_cnt) / n;
    f.has_slash_ratio = static_cast<double>(slash_cnt) / n;
    f.has_brace_ratio = static_cast<double>(brace_cnt) / n;
    f.has_bracket_ratio = static_cast<double>(bracket_cnt) / n;

    f.numeric_ratio = static_cast<double>(numeric_cnt) / n;
    f.integer_ratio = static_cast<double>(integer_cnt) / n;
    f.float_ratio = static_cast<double>(float_cnt) / n;
    f.signed_ratio = static_cast<double>(signed_cnt) / n;

    if (num_max > 0 || num_min < UINT64_MAX) {
        f.numeric_min = (num_min < UINT64_MAX) ? std::optional<uint64_t>(num_min) : std::nullopt;
        f.numeric_max = (num_max > 0) ? std::optional<uint64_t>(num_max) : std::nullopt;
    }

    f.entropy = compute_entropy(byte_hist, n);
    f.byte_histogram = byte_hist;
    f.repeated_pattern_score = compute_repeated_pattern_score(raw_values);
    f.distinct_count = static_cast<uint32_t>(distinct_values.size());

    // Fixed-length detection
    if (f.len_stddev < 0.5 && f.len_min > 0 && f.len_max == f.len_min) {
        f.is_fixed_length = true;
        f.fixed_size = static_cast<uint32_t>(f.len_min);
    }

    return f;
}

inline double FeatureExtractor::compute_entropy(
    const std::vector<uint8_t>& histogram, uint32_t total) {
    if (total == 0) return 0.0;
    double entropy = 0.0;
    for (size_t i = 0; i < histogram.size(); i++) {
        if (histogram[i] > 0) {
            double p = static_cast<double>(histogram[i]) / total;
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

inline double FeatureExtractor::compute_repeated_pattern_score(
    const std::vector<std::vector<uint8_t>>& values) {
    if (values.size() <= 1) return 0.0;
    std::set<std::vector<uint8_t>> unique;
    for (const auto& v : values) {
        if (!v.empty()) unique.insert(v);
    }
    // High score when few unique values relative to sample count
    double ratio = static_cast<double>(unique.size()) / values.size();
    return 1.0 - std::min(ratio * 5.0, 1.0);
}

inline bool FeatureExtractor::is_ascii_printable(uint8_t byte) {
    return byte >= 0x20 && byte <= 0x7E;
}

inline bool FeatureExtractor::is_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

inline bool FeatureExtractor::is_alpha(uint8_t byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

// ==================== StatisticalTypeClassifier Implementation ====================

inline std::vector<TypeCandidate> StatisticalTypeClassifier::classify(
    const ColumnFeatures& f, size_t top_n) {

    std::vector<TypeCandidate> candidates;

    auto score_and_add = [&](const std::string& name, uint32_t type_id, double score, const std::string& reason) {
        if (score > 0.01) {
            candidates.push_back({name, type_id, score, reason});
        }
    };

    score_and_add("TINYINT",    3,  likelihood_tinyint(f),     "1-byte integer, range 0-255");
    score_and_add("SMALLINT",   4,  likelihood_smallint(f),    "2-byte integer, range 0-65535");
    score_and_add("MEDIUMINT",  5,  likelihood_mediumint(f),   "3-byte integer");
    score_and_add("INT",        6,  likelihood_int(f),         "4-byte integer");
    score_and_add("BIGINT",     7,  likelihood_bigint(f),      "8-byte integer");
    score_and_add("FLOAT",      8,  likelihood_float_type(f),  "4-byte floating point");
    score_and_add("DOUBLE",     9,  likelihood_double_type(f), "8-byte floating point");
    score_and_add("DECIMAL",    10, likelihood_decimal(f),     "Fixed-point decimal");
    score_and_add("CHAR(N)",    16, likelihood_char(f),        "Fixed-length string");
    score_and_add("VARCHAR(N)", 17, likelihood_varchar(f),     "Variable-length string");
    score_and_add("TEXT",       22, likelihood_text(f),        "Long text");
    score_and_add("BLOB",       23, likelihood_blob(f),        "Binary large object");
    score_and_add("DATETIME",   19, likelihood_datetime(f),    "Date+time ISO format");
    score_and_add("DATE",       20, likelihood_date(f),        "Date only");
    score_and_add("TIME",       21, likelihood_time(f),        "Time only");
    score_and_add("TIMESTAMP",  18, likelihood_timestamp(f),   "Unix epoch timestamp");
    score_and_add("YEAR",       2,  likelihood_year(f),        "Year value");
    score_and_add("ENUM",       28, likelihood_enum(f),        "Enumeration (few distinct values)");
    score_and_add("SET",        29, likelihood_set(f),         "Set type");
    score_and_add("JSON",       31, likelihood_json(f),        "JSON document");
    score_and_add("BIT",        15, likelihood_bit(f),         "Bit field");
    score_and_add("BINARY",     24, likelihood_binary(f),      "Fixed-length binary");
    score_and_add("VARBINARY",  25, likelihood_binary(f),      "Variable-length binary");

    // Sort by confidence descending
    std::sort(candidates.begin(), candidates.end(),
        [](const TypeCandidate& a, const TypeCandidate& b) {
            return a.confidence > b.confidence;
        });

    // Normalize to [0, 1]
    if (!candidates.empty()) {
        double max_score = candidates[0].confidence;
        if (max_score > 0) {
            for (auto& c : candidates) {
                c.confidence /= max_score;
            }
        }
    }

    if (candidates.size() > top_n) {
        candidates.resize(top_n);
    }

    return candidates;
}

// Type-specific likelihood functions
inline double StatisticalTypeClassifier::likelihood_tinyint(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.8) return 0.0;
    if (f.len_max > 4) return 0.0; // tinyint is 1 byte, but display can be 3 digits
    double score = f.integer_ratio;
    if (f.numeric_max.has_value() && f.numeric_max.value() <= 255) score += 0.3;
    if (f.signed_ratio > 0.3) score *= 0.8; // signed tinyint less common than unsigned
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_smallint(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.8) return 0.0;
    double score = f.integer_ratio * 0.9;
    if (f.numeric_max.has_value()) {
        uint64_t max_val = f.numeric_max.value();
        if (max_val > 255 && max_val <= 65535) score += 0.4;
        if (max_val <= 255) score *= 0.5; // tinyint is more likely
    }
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_mediumint(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.8) return 0.0;
    double score = f.integer_ratio * 0.7;
    if (f.numeric_max.has_value()) {
        uint64_t max_val = f.numeric_max.value();
        if (max_val > 65535 && max_val <= 16777215) score += 0.5;
        if (max_val <= 65535) score *= 0.3;
    } else {
        score *= 0.3; // mediumint is less common
    }
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_int(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.85) return 0.0;
    double score = f.integer_ratio;
    if (f.len_max >= 1 && f.len_max <= 10) score += 0.1;
    if (f.numeric_max.has_value()) {
        uint64_t max_val = f.numeric_max.value();
        if (max_val > 16777215 && max_val <= 4294967295ULL) score += 0.3;
    }
    if (f.signed_ratio > 0.3) score *= 0.9;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_bigint(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.85) return 0.0;
    double score = f.integer_ratio * 0.85;
    if (f.len_max >= 8 || f.len_max > 15) score += 0.15;
    if (f.numeric_max.has_value() && f.numeric_max.value() > 4294967295ULL) score += 0.3;
    if (f.is_fixed_length && f.fixed_size == 8) score += 0.2;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_float_type(const ColumnFeatures& f) {
    if (f.float_ratio < 0.7) return 0.0;
    double score = f.float_ratio * 0.7;
    if (f.has_dot_ratio > 0.7) score += 0.2;
    if (f.len_max <= 12) score += 0.1;
    if (f.is_fixed_length && f.fixed_size == 4) score += 0.15;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_double_type(const ColumnFeatures& f) {
    if (f.float_ratio < 0.5) return 0.0;
    double score = f.float_ratio * 0.6;
    if (f.has_dot_ratio > 0.5) score += 0.15;
    if (f.len_max > 12) score += 0.15;
    if (f.is_fixed_length && f.fixed_size == 8) score += 0.2;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_decimal(const ColumnFeatures& f) {
    if (f.numeric_ratio < 0.7) return 0.0;
    if (f.has_dot_ratio < 0.3) return 0.0;
    double score = f.numeric_ratio * 0.6 + f.has_dot_ratio * 0.3;
    if (f.len_stddev < 1.0) score += 0.1; // fixed precision
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_char(const ColumnFeatures& f) {
    if (f.is_fixed_length && f.fixed_size > 0 && f.fixed_size <= 255 && f.ascii_ratio > 0.8) {
        double score = 0.8;
        if (f.len_stddev < 0.2) score += 0.1;
        if (f.has_space_ratio > 0.3) score += 0.05; // trailing spaces in CHAR
        return std::min(score, 1.0);
    }
    return 0.0;
}

inline double StatisticalTypeClassifier::likelihood_varchar(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.6) return 0.1;
    if (f.len_stddev < 0.3 && f.len_max > 0) return 0.2; // too uniform for varchar
    double score = f.ascii_ratio * 0.7;
    if (f.len_stddev > 1.0) score += 0.2;
    if (f.len_max <= 255) score += 0.1;
    if (f.distinct_count > 5) score += 0.1;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_text(const ColumnFeatures& f) {
    if (f.len_max <= 255) return 0.1;
    double score = f.ascii_ratio * 0.5;
    if (f.len_max > 1000) score += 0.3;
    if (f.len_stddev > 10.0) score += 0.2;
    if (f.has_space_ratio > 0.5 && f.ascii_ratio > 0.7) score += 0.1;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_blob(const ColumnFeatures& f) {
    if (f.ascii_ratio > 0.7) return 0.1; // too ascii for blob
    if (f.entropy < 4.0) return 0.1;
    double score = (f.entropy - 3.0) / 5.0;
    if (f.len_stddev > 5.0) score += 0.2;
    if (f.len_max > 100) score += 0.2;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_datetime(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.8) return 0.0;
    if (f.has_dash_ratio < 0.5) return 0.0;
    if (f.has_colon_ratio < 0.5) return 0.0;
    double score = 0.6;
    if (f.len_mean >= 19 && f.len_mean <= 19) score += 0.3; // YYYY-MM-DD HH:MM:SS
    if (f.len_stddev < 0.5) score += 0.1;
    if (f.is_fixed_length && f.fixed_size == 19) score += 0.1;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_date(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.8) return 0.0;
    if (f.has_dash_ratio < 0.5) return 0.0;
    if (f.has_colon_ratio > 0.1) return 0.1; // likely datetime instead
    double score = 0.5;
    if (f.len_mean >= 10 && f.len_mean <= 10) score += 0.3; // YYYY-MM-DD
    if (f.len_stddev < 0.5) score += 0.1;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_time(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.7) return 0.0;
    if (f.has_colon_ratio < 0.6) return 0.0;
    if (f.has_dash_ratio > 0.2) return 0.1;
    double score = f.has_colon_ratio * 0.8;
    if (f.len_mean >= 8 && f.len_mean <= 15) score += 0.1; // HH:MM:SS or HH:MM:SS.ffffff
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_timestamp(const ColumnFeatures& f) {
    // Unix timestamps are 10-digit integers
    if (f.integer_ratio < 0.9) return 0.0;
    double score = f.integer_ratio * 0.5;
    if (f.len_min == 10 && f.len_max == 10) score += 0.4; // classic unix timestamp
    if (f.len_min >= 10 && f.len_max <= 13) score += 0.1; // milliseconds
    if (f.numeric_min.has_value() && f.numeric_min.value() > 1000000000ULL &&
        f.numeric_max.has_value() && f.numeric_max.value() < 2000000000ULL) {
        score += 0.2; // within unix epoch range
    }
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_year(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.9) return 0.0;
    double score = f.integer_ratio * 0.5;
    if (f.len_min == 4 && f.len_max == 4) score += 0.4;
    if (f.numeric_min.has_value() && f.numeric_min.value() >= 1900 &&
        f.numeric_max.has_value() && f.numeric_max.value() <= 2155) {
        score += 0.2;
    }
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_enum(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.8) return 0.1;
    if (f.distinct_count > 50 || f.distinct_count < 1) return 0.0;
    double score = (50.0 - f.distinct_count) / 50.0;
    if (f.repeated_pattern_score > 0.7) score += 0.3;
    if (f.len_stddev < 2.0) score += 0.2;
    if (f.len_max < 50) score += 0.1;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_set(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.7) return 0.0;
    double score = likelihood_enum(f) * 0.5; // similar to enum
    if (f.has_dot_ratio > 0.1 && f.ascii_ratio > 0.8) score += 0.2; // comma-separated
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_json(const ColumnFeatures& f) {
    if (f.has_brace_ratio < 0.5 && f.has_bracket_ratio < 0.5) return 0.0;
    if (f.ascii_ratio < 0.6) return 0.1;
    double score = 0.5;
    if (f.has_brace_ratio > 0.7 || f.has_bracket_ratio > 0.7) score += 0.3;
    if (f.len_stddev > 5.0) score += 0.15;
    if (f.len_max > 100) score += 0.1;
    if (f.has_colon_ratio > 0.5 && f.has_dot_ratio > 0.3) score += 0.05; // JSON key:value
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_bit(const ColumnFeatures& f) {
    // BIT values are packed binary, very short
    if (f.len_max > 8 || f.len_min < 1) return 0.1;
    if (f.ascii_ratio > 0.5) return 0.1; // not readable as text
    double score = 0.5;
    if (f.len_stddev < 0.3) score += 0.3;
    if (f.entropy < 2.0) score += 0.1;
    return std::min(score, 1.0);
}

inline double StatisticalTypeClassifier::likelihood_binary(const ColumnFeatures& f) {
    if (f.ascii_ratio > 0.7) return 0.1;
    if (f.is_fixed_length && f.fixed_size > 0 && f.fixed_size <= 255) {
        double score = 0.6;
        if (f.entropy > 3.0) score += 0.2;
        return std::min(score, 1.0);
    }
    if (f.len_stddev > 3.0 && f.entropy > 4.0) return 0.5; // varbinary-like
    return 0.0;
}

// ==================== Score Helpers ====================

inline double StatisticalTypeClassifier::score_range(
    double value, double ideal_min, double ideal_max, double tolerance) {
    if (value >= ideal_min && value <= ideal_max) return 1.0;
    if (value < ideal_min) return std::max(0.0, 1.0 - (ideal_min - value) / tolerance);
    return std::max(0.0, 1.0 - (value - ideal_max) / tolerance);
}

inline double StatisticalTypeClassifier::score_above(double value, double threshold) {
    return value >= threshold ? 1.0 : value / threshold;
}

inline double StatisticalTypeClassifier::score_below(double value, double threshold) {
    return value <= threshold ? 1.0 : threshold / value;
}

inline double StatisticalTypeClassifier::score_near(
    double value, double target, double tolerance) {
    return std::max(0.0, 1.0 - std::abs(value - target) / tolerance);
}

// ==================== PhysicalLayoutAnalyzer Implementation ====================

inline std::optional<PhysicalLayoutAnalyzer::LayoutInfo>
PhysicalLayoutAnalyzer::analyze_page(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 100) return std::nullopt;

    LayoutInfo info;

    // Check FIL header page type (offset 24, big-endian, 2 bytes)
    uint16_t page_type = (static_cast<uint16_t>(page_data[24]) << 8) | page_data[25];
    if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX) return std::nullopt;

    // Read PAGE_N_RECS from page header (FIL_HEADER_SIZE + 16 = 38 + 16 = 54)
    uint16_t n_recs = (static_cast<uint16_t>(page_data[55]) << 8) | page_data[54];
    if (n_recs == 0 || n_recs > 1000) return std::nullopt;

    // Read PAGE_HEAP_TOP from page header (FIL_HEADER_SIZE + 2 = 38 + 2 = 40)
    uint16_t heap_top = (static_cast<uint16_t>(page_data[41]) << 8) | page_data[40];
    if (heap_top < 120 || heap_top > page_size - 8) return std::nullopt;

    // Read INFIMUM offset (FIL_HEADER + 38 bytes page header + 6 = 38 + 44 = 82)
    // Actually: PAGE_HEADER_SIZE=56, PAGE_NEW_INFIMUM at PAGE_HEADER + 36
    // Read first user record offset from INFIMUM's next_offset (offset 100-101, LE)
    uint16_t first_rec_offset = (static_cast<uint16_t>(page_data[101]) << 8) | page_data[100];

    // Read SUPREMUM offset
    uint16_t supremum_offset = (static_cast<uint16_t>(page_data[103]) << 8) | page_data[102];

    // Count offset table entries from INFIMUM record
    // COMPACT format: record header [info_bits(1B)] then [next_offset(2B)]
    // After the 5-byte header, the offset table entries follow
    // Each entry is 2 bytes (little-endian), stored in reverse order from record end

    // INFIMUM has no offset table (it's a pseudo-record)
    // First real record starts after INFIMUM (5 bytes for COMPACT)
    // INFIMUM's next_offset points to the first real record, already read above

    if (first_rec_offset < 120 || first_rec_offset >= heap_top) return std::nullopt;

    // Read the offset table from the first real record
    // The record's offset table is at the end of the record, stored in reverse order
    // Record layout: [info_bits(1B)] [next_offset(2B)] [offset_table(N*2B)] [NULL bitmap] [data]
    // Actually for COMPACT: [offset_table(variable)] [info_bits(1B)] [NULL bitmap] [data]
    // The offset_table starts at record_offset and ends at the info_bits byte

    // Simple approach: count non-zero offset entries from the first record
    // The first offset gives the record length, subsequent offsets are field boundaries
    uint32_t col_count = 0;
    uint16_t pos = first_rec_offset;
    std::vector<uint16_t> offsets;

    // Read offset entries until we hit the info_bits pattern
    // The first entry (offset) should point near the end of the record data section
    uint16_t first_entry = (static_cast<uint16_t>(page_data[pos + 1]) << 8) | page_data[pos];
    uint16_t second_entry = (static_cast<uint16_t>(page_data[pos + 3]) << 8) | page_data[pos + 2];

    // Count how many offset entries there are
    while (pos + 2 <= page_size && col_count < 200) {
        uint16_t val = (static_cast<uint16_t>(page_data[pos + 1]) << 8) | page_data[pos];
        if (val == 0 || val > page_size) break;
        if (!offsets.empty() && val >= offsets.back()) break; // offsets must be strictly decreasing
        offsets.push_back(val);
        col_count++;
        pos += 2;
    }

    if (col_count < 3 || col_count > 100) return std::nullopt;

    // User columns = total - 3 (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID/system cols)
    info.total_columns = col_count;
    info.user_columns = col_count > 3 ? col_count - 3 : col_count;
    info.is_compact_format = true;
    info.row_format = 1; // COMPACT

    // NULL bitmap: ceil(user_columns / 8) bytes, after offset table
    info.null_bitmap_bytes = (info.user_columns + 7) / 8;

    // Check REDUNDANT format
    // REDUNDANT records have a 6-byte fixed header unlike COMPACT's variable header
    if (first_entry < 6 || (offsets.size() > 1 && offsets[0] - offsets[1] < 2)) {
        info.row_format = 0; // REDUNDANT
    }

    return info;
}

inline PhysicalLayoutAnalyzer::LayoutInfo
PhysicalLayoutAnalyzer::analyze_multiple_pages(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages) {

    std::map<uint32_t, uint32_t> col_count_votes;
    LayoutInfo best_info;

    for (const auto& [data, size] : pages) {
        auto info = analyze_page(data, size);
        if (info) {
            col_count_votes[info->total_columns]++;
            if (col_count_votes[info->total_columns] > col_count_votes[best_info.total_columns]) {
                best_info = *info;
            }
        }
    }

    return best_info;
}

inline std::vector<std::vector<std::vector<uint8_t>>>
PhysicalLayoutAnalyzer::extract_column_values(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages,
    const LayoutInfo& layout,
    uint32_t max_samples) {

    uint32_t col_count = layout.total_columns;
    std::vector<std::vector<std::vector<uint8_t>>> column_values(col_count);
    uint32_t sampled = 0;

    for (const auto& [data, size] : pages) {
        if (sampled >= max_samples) break;
        if (size < 120) continue;

        uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
        if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX) continue;

        // Read first user record offset from INFIMUM's next_offset (offset 100-101, LE)
        uint16_t rec_off = (static_cast<uint16_t>(data[101]) << 8) | data[100];
        uint16_t heap_top = (static_cast<uint16_t>(data[41]) << 8) | data[40];
        uint16_t n_recs = (static_cast<uint16_t>(data[55]) << 8) | data[54];

        for (uint16_t rec_idx = 0; rec_idx < n_recs && sampled < max_samples; rec_idx++) {
            if (rec_off < 120 || rec_off >= heap_top) break;

            // COMPACT format: [offset_table(N*2)] [info_bits(1B)] [NULL bitmap(?B)] [fields...]
            // Read offset table entries
            std::vector<uint16_t> field_offsets;
            uint16_t pos = rec_off;
            for (uint32_t i = 0; i < col_count && pos + 2 <= size; i++) {
                uint16_t val = (static_cast<uint16_t>(data[pos + 1]) << 8) | data[pos];
                if (val == 0 || val > size) break;
                field_offsets.push_back(val);
                pos += 2;
            }

            if (field_offsets.size() < col_count) break;

            // Skip info_bits (1 byte) and NULL bitmap
            uint16_t info_pos = rec_off + col_count * 2;
            uint16_t data_start = info_pos + 1 + layout.null_bitmap_bytes;

            // Extract each field based on offset table
            // Offsets are stored in reverse: last field first
            // Field boundaries: offset[i] to offset[i+1] (offset[0] is total length)
            uint16_t record_end = rec_off + field_offsets[0];

            for (uint32_t i = 0; i < col_count && i < column_values.size(); i++) {
                uint16_t field_start, field_end;
                if (i + 1 < field_offsets.size()) {
                    field_start = rec_off + field_offsets[i + 1];
                    field_end = rec_off + field_offsets[i];
                } else {
                    field_start = data_start;
                    field_end = rec_off + field_offsets[i];
                }

                if (field_end > field_start && field_end <= size && field_start >= data_start) {
                    std::vector<uint8_t> field_data(
                        data + field_start, data + field_end);
                    column_values[i].push_back(std::move(field_data));
                }
            }

            sampled++;

            // Advance to next record
            uint16_t next_rec = (static_cast<uint16_t>(data[rec_off + 1]) << 8)
                              | data[rec_off];
            if (next_rec <= rec_off || next_rec >= heap_top) break;
            rec_off = next_rec;
        }
    }

    return column_values;
}

} // namespace inference
} // namespace dbrecovery

#endif // DBRECOVERY_INFERENCE_STATISTICAL_CLASSIFIER_HPP