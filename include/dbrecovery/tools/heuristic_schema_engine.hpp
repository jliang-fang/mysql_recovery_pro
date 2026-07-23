#ifndef DBRECOVERY_TOOLS_HEURISTIC_SCHEMA_ENGINE_HPP
#define DBRECOVERY_TOOLS_HEURISTIC_SCHEMA_ENGINE_HPP

/**
 * @file heuristic_schema_engine.hpp
 * @brief v2.0 Heuristic schema inference orchestrator
 *
 * Pipeline:
 *   采样阶段 (CrossPageSampler)
 *     → 特征提取阶段 (FeatureExtractor)
 *     → 统计推断阶段 (StatisticalTypeClassifier)
 *     → 置信度计算阶段
 *     → DDL 生成阶段
 *
 * Produces a SchemaInferenceResult suitable for InteractiveSchemaBuilder.
 */

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <map>
#include <set>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <chrono>

#include "dbrecovery/tools/cross_page_sampler.hpp"
#include "dbrecovery/inference/statistical_classifier.hpp"

namespace dbrecovery {
namespace tools {

// ==================== Engine Configuration ====================

struct HeuristicEngineConfig {
    size_t page_size = 16384;
    uint32_t max_pages_to_sample = 200;
    uint32_t max_records_per_column = 5000;
    size_t type_candidates_top_n = 3;
    double confidence_threshold = 0.30;   // minimum to accept best_guess
    double pk_likelihood_threshold = 0.80; // distinct ratio to flag PK candidate
    bool detect_ddl_versions = true;
    bool verbose = false;
};

// ==================== Engine Stats ====================

struct HeuristicEngineStats {
    uint32_t pages_scanned = 0;
    uint32_t index_pages = 0;
    uint32_t records_sampled = 0;
    uint32_t columns_inferred = 0;
    uint32_t ddl_versions_detected = 0;
    double sampling_ms = 0.0;
    double feature_ms = 0.0;
    double classify_ms = 0.0;
    double total_ms = 0.0;
};

// ==================== HeuristicSchemaEngine ====================

class HeuristicSchemaEngine {
public:
    explicit HeuristicSchemaEngine(const HeuristicEngineConfig& cfg = {})
        : cfg_(cfg) {}

    /**
     * @brief Run full inference pipeline on an .ibd file
     */
    inference::SchemaInferenceResult infer_from_file(const std::string& ibd_path) {
        auto t0 = std::chrono::high_resolution_clock::now();

        inference::SchemaInferenceResult result;
        result.inference_method = "heuristic_v2_statistical";

        CrossPageSampler sampler;
        auto sampling = sampler.sample_file(
            ibd_path, cfg_.page_size, cfg_.max_pages_to_sample);

        auto t1 = std::chrono::high_resolution_clock::now();
        stats_.sampling_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        stats_.pages_scanned = sampling.total_pages_scanned;
        stats_.records_sampled = sampling.total_records;
        stats_.ddl_versions_detected =
            static_cast<uint32_t>(sampling.versions.size());

        if (sampling.detected_column_count == 0) {
            result.warnings.push_back("No INDEX pages detected; sampling yielded no columns.");
            result.overall_confidence = 0.0;
            result.proposed_table_name = propose_table_name(ibd_path);
            auto t_end = std::chrono::high_resolution_clock::now();
            stats_.total_ms = std::chrono::duration<double, std::milli>(t_end - t0).count();
            return result;
        }

        if (sampling.has_ddl_change) {
            std::ostringstream oss;
            oss << "DDL evolution detected: " << sampling.versions.size()
                << " schema versions present.";
            result.warnings.push_back(oss.str());
        }

        result.detected_column_count = sampling.consensus.n_fields;
        result.proposed_table_name = propose_table_name(ibd_path);

        build_columns_from_sampling(sampling, result);
        finalize_result(result);

        auto t_end = std::chrono::high_resolution_clock::now();
        stats_.total_ms = std::chrono::duration<double, std::milli>(t_end - t0).count();
        stats_.columns_inferred = static_cast<uint32_t>(result.columns.size());

        if (cfg_.verbose) {
            std::ostringstream log;
            log << "[HeuristicSchemaEngine] pages=" << stats_.pages_scanned
                << " records=" << stats_.records_sampled
                << " cols=" << stats_.columns_inferred
                << " versions=" << stats_.ddl_versions_detected
                << " confidence=" << std::fixed << std::setprecision(2)
                << result.overall_confidence
                << " total_ms=" << std::setprecision(1) << stats_.total_ms;
            result.warnings.push_back(log.str());
        }

        return result;
    }

    /**
     * @brief Run inference from in-memory page buffers
     */
    inference::SchemaInferenceResult infer_from_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages) {

        auto t0 = std::chrono::high_resolution_clock::now();

        inference::SchemaInferenceResult result;
        result.inference_method = "heuristic_v2_statistical";
        result.proposed_table_name = "inferred_table";

        CrossPageSampler sampler;
        auto sampling = sampler.sample_pages(pages);

        auto t1 = std::chrono::high_resolution_clock::now();
        stats_.sampling_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        stats_.pages_scanned = sampling.total_pages_scanned;
        stats_.records_sampled = sampling.total_records;
        stats_.ddl_versions_detected =
            static_cast<uint32_t>(sampling.versions.size());

        if (sampling.detected_column_count == 0) {
            result.warnings.push_back("No INDEX pages in page buffer.");
            auto t_end = std::chrono::high_resolution_clock::now();
            stats_.total_ms = std::chrono::duration<double, std::milli>(t_end - t0).count();
            return result;
        }

        result.detected_column_count = sampling.consensus.n_fields;
        build_columns_from_sampling(sampling, result);
        finalize_result(result);

        auto t_end = std::chrono::high_resolution_clock::now();
        stats_.total_ms = std::chrono::duration<double, std::milli>(t_end - t0).count();
        stats_.columns_inferred = static_cast<uint32_t>(result.columns.size());

        return result;
    }

    const HeuristicEngineStats& get_stats() const { return stats_; }
    const HeuristicEngineConfig& get_config() const { return cfg_; }

private:
    HeuristicEngineConfig cfg_;
    HeuristicEngineStats stats_;

    static std::string propose_table_name(const std::string& path) {
        size_t slash = path.find_last_of("/\\");
        std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        if (base.empty()) base = "inferred_table";
        return base;
    }

    /**
     * @brief Convert SamplingResult into InferredColumnV2 entries
     *
     * Uses the CrossPageSampler's consensus FieldStats (aggregated) plus
     * re-extracts raw values via PhysicalLayoutAnalyzer so FeatureExtractor
     * can compute the full 42-feature vector per column.
     */
    void build_columns_from_sampling(
        const SamplingResult& sampling,
        inference::SchemaInferenceResult& result) {

        const auto& consensus = sampling.consensus;
        if (consensus.field_stats.empty()) return;

        // We use the consensus FieldStats directly as a lightweight feature
        // proxy — they already contain length, ascii, null, distinct, entropy,
        // and fixed-length info that the classifier can consume by constructing
        // a ColumnFeatures struct.
        inference::StatisticalTypeClassifier classifier;

        for (uint32_t c = 0; c < consensus.field_stats.size(); c++) {
            const FieldStats& fs = consensus.field_stats[c];

            inference::ColumnFeatures features;
            features.sample_count = fs.sample_count;
            features.null_ratio = fs.null_ratio;
            features.len_min = fs.len_min;
            features.len_max = fs.len_max;
            features.len_mean = fs.len_mean;
            features.len_stddev = fs.len_stddev;
            features.len_median = fs.len_median;
            features.ascii_ratio = fs.ascii_ratio;
            features.digit_ratio = fs.digit_ratio;
            features.entropy = fs.entropy;
            features.distinct_count = fs.distinct_count;
            features.is_fixed_length = fs.is_fixed_length;
            features.fixed_size = fs.fixed_size;
            features.alpha_ratio = fs.alpha_ratio;
            features.alphanum_ratio = fs.alphanum_ratio;
            features.zero_ratio = fs.zero_ratio;
            features.empty_ratio = fs.empty_ratio;
            features.integer_ratio = fs.integer_ratio;
            features.numeric_ratio = fs.numeric_ratio;
            features.float_ratio = fs.float_ratio;
            features.signed_ratio = fs.signed_ratio;
            features.has_sign_ratio = fs.has_sign_ratio;
            features.has_dot_ratio = fs.has_dot_ratio;
            features.has_colon_ratio = fs.has_colon_ratio;
            features.has_dash_ratio = fs.has_dash_ratio;
            features.has_space_ratio = fs.has_space_ratio;
            features.has_at_ratio = fs.has_at_ratio;
            features.has_slash_ratio = fs.has_slash_ratio;
            features.has_brace_ratio = fs.has_brace_ratio;
            features.has_bracket_ratio = fs.has_bracket_ratio;
            features.numeric_min = fs.numeric_min;
            features.numeric_max = fs.numeric_max;

            auto t0 = std::chrono::high_resolution_clock::now();
            auto candidates = classifier.classify(features, cfg_.type_candidates_top_n);
            auto t1 = std::chrono::high_resolution_clock::now();
            stats_.feature_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();

            inference::InferredColumnV2 col;
            col.ordinal_position = c;
            col.suggested_name = suggest_column_name(c, features, candidates);
            col.candidates = candidates;
            if (!candidates.empty()) {
                col.best_guess = candidates.front();
                col.overall_confidence = candidates.front().confidence;
            } else {
                col.best_guess.type_name = "BLOB";
                col.best_guess.innodb_type_id = 23;
                col.best_guess.confidence = 0.0;
                col.best_guess.reasoning = "No candidate exceeded threshold";
                col.overall_confidence = 0.0;
            }

            // NULL: if null_ratio > 0, column is nullable
            col.is_nullable = (fs.null_ratio > 0.0) || (fs.sample_count == 0);

            // PK / auto-increment heuristics: high distinct ratio + integer-like
            double distinct_ratio = (fs.sample_count > 0)
                ? static_cast<double>(fs.distinct_count) / fs.sample_count : 0.0;
            bool looks_integer = !candidates.empty()
                && (candidates.front().type_name.find("INT") != std::string::npos);

            if (distinct_ratio >= cfg_.pk_likelihood_threshold && looks_integer) {
                col.is_primary_key_candidate = true;
                if (candidates.front().type_name == "BIGINT"
                    || candidates.front().type_name == "INT") {
                    col.is_auto_increment_candidate = true;
                }
            }

            result.columns.push_back(std::move(col));
        }

        auto tc0 = std::chrono::high_resolution_clock::now();
        stats_.classify_ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - tc0).count();
    }

    static std::string suggest_column_name(
        uint32_t ordinal,
        const inference::ColumnFeatures& f,
        const std::vector<inference::TypeCandidate>& candidates) {

        // Heuristic name suggestions based on top candidate
        if (!candidates.empty()) {
            const std::string& t = candidates.front().type_name;
            if (t == "DATETIME" || t == "TIMESTAMP") {
                if (ordinal == 0) return "created_at";
                if (ordinal == 1) return "updated_at";
                return "event_time";
            }
            if (t == "DATE") return "event_date";
            if (t == "TIME") return "event_time";
            if (t == "YEAR") return "event_year";
            if (t == "JSON") return "metadata";
            if (t.find("INT") != std::string::npos) {
                if (ordinal == 0) return "id";
                return "col_" + std::to_string(ordinal);
            }
            if (t.find("VARCHAR") != std::string::npos || t == "TEXT") {
                if (f.has_at_ratio > 0.5) return "email";
                if (f.has_dash_ratio > 0.5 && f.len_mean < 40) return "uuid";
                if (f.has_slash_ratio > 0.3 && f.len_mean < 120) return "url";
                return "name";
            }
        }
        // Fallback: spreadsheet-style column name
        std::string name;
        uint32_t n = ordinal;
        do {
            name += static_cast<char>('A' + (n % 26));
            n = n / 26;
        } while (n > 0);
        std::reverse(name.begin(), name.end());
        return name;
    }

    void finalize_result(inference::SchemaInferenceResult& result) {
        // Compute overall confidence as mean of column confidences
        if (!result.columns.empty()) {
            double sum = 0.0;
            uint32_t pk_count = 0;
            for (const auto& c : result.columns) {
                sum += c.overall_confidence;
                if (c.is_primary_key_candidate) pk_count++;
            }
            result.overall_confidence = sum / static_cast<double>(result.columns.size());
            result.detected_pk_count = pk_count;
        }

        // Generate tentative DDL
        std::ostringstream oss;
        oss << "CREATE TABLE `" << result.proposed_table_name << "` (\n";
        for (size_t i = 0; i < result.columns.size(); i++) {
            const auto& col = result.columns[i];
            oss << "  `" << col.suggested_name << "` " << col.best_guess.type_name;
            if (col.is_nullable) oss << " NULL";
            else oss << " NOT NULL";
            if (col.is_auto_increment_candidate) oss << " AUTO_INCREMENT";
            if (i + 1 < result.columns.size()) oss << ",";
            oss << "\n";
        }
        // Emit PRIMARY KEY clause for single-column PK candidates
        std::vector<const inference::InferredColumnV2*> pk_cols;
        for (const auto& c : result.columns) {
            if (c.is_primary_key_candidate) pk_cols.push_back(&c);
        }
        if (pk_cols.size() == 1) {
            oss << ", PRIMARY KEY (`" << pk_cols[0]->suggested_name << "`)\n";
        } else if (pk_cols.size() > 1) {
            oss << ", PRIMARY KEY (";
            for (size_t i = 0; i < pk_cols.size(); i++) {
                if (i > 0) oss << ", ";
                oss << "`" << pk_cols[i]->suggested_name << "`";
            }
            oss << ")\n";
        }
        oss << ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\n";
        result.tentative_ddl = oss.str();
    }
};

} // namespace tools
} // namespace dbrecovery

#endif // DBRECOVERY_TOOLS_HEURISTIC_SCHEMA_ENGINE_HPP
