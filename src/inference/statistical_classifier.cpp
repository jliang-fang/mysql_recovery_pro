/**
 * @file statistical_classifier.cpp
 * @brief Statistical type classifier for schemaless column inference (v2.0)
 *
 * Implementations extracted from statistical_classifier.hpp for better compilation.
 */

#include "dbrecovery/inference/statistical_classifier.hpp"

#include <cmath>
#include <numeric>
#include <set>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace dbrecovery {
namespace inference {

// ==================== ColumnFeatures ====================

std::string ColumnFeatures::to_string() const {
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

// ==================== FeatureExtractor ====================

ColumnFeatures FeatureExtractor::extract(
    const std::vector<std::vector<uint8_t>>& raw_values) {

    ColumnFeatures f;
    f.sample_count = static_cast<uint32_t>(raw_values.size());
    if (f.sample_count == 0) return f;

    std::vector<double> lengths;
    std::vector<uint32_t> byte_hist(256, 0);
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

        for (uint8_t b : bytes) {
            byte_hist[b]++;
            if (b != 0) all_zero = false;
            if (!is_ascii_printable(b)) all_ascii = false;
            if (is_digit(b)) { has_digit_char = true; }
            if (is_alpha(b)) { has_alpha_char = true; }
            if (b == '-') has_sign = true;
            if (b == '.') has_dot = true;
            if (b == ':') has_colon = true;
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

    uint64_t total_bytes = 0;
    for (auto c : byte_hist) total_bytes += c;

    f.entropy = compute_entropy(byte_hist, n, total_bytes);
    f.byte_histogram = byte_hist;
    f.repeated_pattern_score = compute_repeated_pattern_score(raw_values);
    f.distinct_count = static_cast<uint32_t>(distinct_values.size());

    if (f.len_stddev < 0.5 && f.len_min > 0 && f.len_max == f.len_min) {
        f.is_fixed_length = true;
        f.fixed_size = static_cast<uint32_t>(f.len_min);
    }

    return f;
}

double FeatureExtractor::compute_entropy(
    const std::vector<uint32_t>& histogram, uint32_t sample_count, uint64_t total_bytes) {
    if (total_bytes == 0) return 0.0;
    double entropy = 0.0;
    for (size_t i = 0; i < histogram.size(); i++) {
        if (histogram[i] > 0) {
            double p = static_cast<double>(histogram[i]) / total_bytes;
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

double FeatureExtractor::compute_repeated_pattern_score(
    const std::vector<std::vector<uint8_t>>& values) {
    if (values.size() <= 1) return 0.0;
    std::set<std::vector<uint8_t>> unique;
    for (const auto& v : values) {
        if (!v.empty()) unique.insert(v);
    }
    double ratio = static_cast<double>(unique.size()) / values.size();
    return 1.0 - std::min(ratio * 5.0, 1.0);
}

bool FeatureExtractor::is_ascii_printable(uint8_t byte) {
    return byte >= 0x20 && byte <= 0x7E;
}

bool FeatureExtractor::is_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

bool FeatureExtractor::is_alpha(uint8_t byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

// ==================== StatisticalTypeClassifier ====================

std::vector<TypeCandidate> StatisticalTypeClassifier::classify(
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

double StatisticalTypeClassifier::likelihood_tinyint(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.8) return 0.0;
    if (f.len_max > 4) return 0.0;
    double score = f.integer_ratio;
    if (f.numeric_max.has_value() && f.numeric_max.value() <= 255) score += 0.3;
    if (f.signed_ratio > 0.3) score *= 0.8;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_smallint(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.8) return 0.0;
    double score = f.integer_ratio * 0.9;
    if (f.numeric_max.has_value()) {
        uint64_t max_val = f.numeric_max.value();
        if (max_val > 255 && max_val <= 65535) score += 0.4;
        if (max_val <= 255) score *= 0.5;
    }
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_mediumint(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.8) return 0.0;
    double score = f.integer_ratio * 0.7;
    if (f.numeric_max.has_value()) {
        uint64_t max_val = f.numeric_max.value();
        if (max_val > 65535 && max_val <= 16777215) score += 0.5;
        if (max_val <= 65535) score *= 0.3;
    } else {
        score *= 0.3;
    }
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_int(const ColumnFeatures& f) {
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

double StatisticalTypeClassifier::likelihood_bigint(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.85) return 0.0;
    double score = f.integer_ratio * 0.85;
    if (f.len_max >= 8 || f.len_max > 15) score += 0.15;
    if (f.numeric_max.has_value() && f.numeric_max.value() > 4294967295ULL) score += 0.3;
    if (f.is_fixed_length && f.fixed_size == 8) score += 0.2;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_float_type(const ColumnFeatures& f) {
    if (f.float_ratio < 0.7) return 0.0;
    double score = f.float_ratio * 0.7;
    if (f.has_dot_ratio > 0.7) score += 0.2;
    if (f.len_max <= 12) score += 0.1;
    if (f.is_fixed_length && f.fixed_size == 4) score += 0.15;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_double_type(const ColumnFeatures& f) {
    if (f.float_ratio < 0.5) return 0.0;
    double score = f.float_ratio * 0.6;
    if (f.has_dot_ratio > 0.5) score += 0.15;
    if (f.len_max > 12) score += 0.15;
    if (f.is_fixed_length && f.fixed_size == 8) score += 0.2;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_decimal(const ColumnFeatures& f) {
    if (f.numeric_ratio < 0.7) return 0.0;
    if (f.has_dot_ratio < 0.3) return 0.0;
    double score = f.numeric_ratio * 0.6 + f.has_dot_ratio * 0.3;
    if (f.len_stddev < 1.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_char(const ColumnFeatures& f) {
    if (f.is_fixed_length && f.fixed_size > 0 && f.fixed_size <= 255 && f.ascii_ratio > 0.8) {
        double score = 0.8;
        if (f.len_stddev < 0.2) score += 0.1;
        if (f.has_space_ratio > 0.3) score += 0.05;
        return std::min(score, 1.0);
    }
    return 0.0;
}

double StatisticalTypeClassifier::likelihood_varchar(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.6) return 0.1;
    if (f.len_stddev < 0.3 && f.len_max > 0) return 0.2;
    double score = f.ascii_ratio * 0.7;
    if (f.len_stddev > 1.0) score += 0.2;
    if (f.len_max <= 255) score += 0.1;
    if (f.distinct_count > 5) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_text(const ColumnFeatures& f) {
    if (f.len_max <= 255) return 0.1;
    double score = f.ascii_ratio * 0.5;
    if (f.len_max > 1000) score += 0.3;
    if (f.len_stddev > 10.0) score += 0.2;
    if (f.has_space_ratio > 0.5 && f.ascii_ratio > 0.7) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_blob(const ColumnFeatures& f) {
    if (f.ascii_ratio > 0.7) return 0.1;
    if (f.entropy < 4.0) return 0.1;
    double score = (f.entropy - 3.0) / 5.0;
    if (f.len_stddev > 5.0) score += 0.2;
    if (f.len_max > 100) score += 0.2;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_datetime(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.8) return 0.0;
    if (f.has_dash_ratio < 0.5) return 0.0;
    if (f.has_colon_ratio < 0.5) return 0.0;
    double score = 0.6;
    if (f.len_mean >= 19 && f.len_mean <= 19) score += 0.3;
    if (f.len_stddev < 0.5) score += 0.1;
    if (f.is_fixed_length && f.fixed_size == 19) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_date(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.8) return 0.0;
    if (f.has_dash_ratio < 0.5) return 0.0;
    if (f.has_colon_ratio > 0.1) return 0.1;
    double score = 0.5;
    if (f.len_mean >= 10 && f.len_mean <= 10) score += 0.3;
    if (f.len_stddev < 0.5) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_time(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.7) return 0.0;
    if (f.has_colon_ratio < 0.6) return 0.0;
    if (f.has_dash_ratio > 0.2) return 0.1;
    double score = f.has_colon_ratio * 0.8;
    if (f.len_mean >= 8 && f.len_mean <= 15) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_timestamp(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.9) return 0.0;
    double score = f.integer_ratio * 0.5;
    if (f.len_min == 10 && f.len_max == 10) score += 0.4;
    if (f.len_min >= 10 && f.len_max <= 13) score += 0.1;
    if (f.numeric_min.has_value() && f.numeric_min.value() > 1000000000ULL &&
        f.numeric_max.has_value() && f.numeric_max.value() < 2000000000ULL) {
        score += 0.2;
    }
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_year(const ColumnFeatures& f) {
    if (f.integer_ratio < 0.9) return 0.0;
    double score = f.integer_ratio * 0.5;
    if (f.len_min == 4 && f.len_max == 4) score += 0.4;
    if (f.numeric_min.has_value() && f.numeric_min.value() >= 1900 &&
        f.numeric_max.has_value() && f.numeric_max.value() <= 2155) {
        score += 0.2;
    }
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_enum(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.8) return 0.1;
    if (f.distinct_count > 50 || f.distinct_count < 1) return 0.0;
    double score = (50.0 - f.distinct_count) / 50.0;
    if (f.repeated_pattern_score > 0.7) score += 0.3;
    if (f.len_stddev < 2.0) score += 0.2;
    if (f.len_max < 50) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_set(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.7) return 0.0;
    double score = likelihood_enum(f) * 0.5;
    if (f.has_dot_ratio > 0.1 && f.ascii_ratio > 0.8) score += 0.2;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_json(const ColumnFeatures& f) {
    if (f.has_brace_ratio < 0.5 && f.has_bracket_ratio < 0.5) return 0.0;
    if (f.ascii_ratio < 0.6) return 0.1;
    double score = 0.5;
    if (f.has_brace_ratio > 0.7 || f.has_bracket_ratio > 0.7) score += 0.3;
    if (f.len_stddev > 5.0) score += 0.15;
    if (f.len_max > 100) score += 0.1;
    if (f.has_colon_ratio > 0.5 && f.has_dot_ratio > 0.3) score += 0.05;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_bit(const ColumnFeatures& f) {
    if (f.len_max > 8 || f.len_min < 1) return 0.1;
    if (f.ascii_ratio > 0.5) return 0.1;
    double score = 0.5;
    if (f.len_stddev < 0.3) score += 0.3;
    if (f.entropy < 2.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_binary(const ColumnFeatures& f) {
    if (f.ascii_ratio > 0.7) return 0.1;
    if (f.is_fixed_length && f.fixed_size > 0 && f.fixed_size <= 255) {
        double score = 0.6;
        if (f.entropy > 3.0) score += 0.2;
        return std::min(score, 1.0);
    }
    if (f.len_stddev > 3.0 && f.entropy > 4.0) return 0.5;
    return 0.0;
}

double StatisticalTypeClassifier::score_range(
    double value, double ideal_min, double ideal_max, double tolerance) {
    if (value >= ideal_min && value <= ideal_max) return 1.0;
    if (value < ideal_min) return std::max(0.0, 1.0 - (ideal_min - value) / tolerance);
    return std::max(0.0, 1.0 - (value - ideal_max) / tolerance);
}

double StatisticalTypeClassifier::score_above(double value, double threshold) {
    return value >= threshold ? 1.0 : value / threshold;
}

double StatisticalTypeClassifier::score_below(double value, double threshold) {
    return value <= threshold ? 1.0 : threshold / value;
}

double StatisticalTypeClassifier::score_near(
    double value, double target, double tolerance) {
    return std::max(0.0, 1.0 - std::abs(value - target) / tolerance);
}

// ==================== TypeCandidate ====================

std::string TypeCandidate::to_string() const {
    std::ostringstream oss;
    oss << type_name << " (" << std::fixed << std::setprecision(1)
        << confidence * 100 << "%): " << reasoning;
    return oss.str();
}

// ==================== SchemaInferenceResult ====================

std::string SchemaInferenceResult::to_report() const {
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

// PhysicalLayoutAnalyzer is implemented in physical_layout_analyzer.cpp

} // namespace inference
} // namespace dbrecovery