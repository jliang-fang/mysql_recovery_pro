#ifndef DBRECOVERY_TOOLS_SCHEMA_TEMPLATE_INFERENCE_HPP
#define DBRECOVERY_TOOLS_SCHEMA_TEMPLATE_INFERENCE_HPP

/**
 * @file schema_template_inference.hpp
 * @brief Instant DDL version-metadata inference from raw .ibd pages
 *
 * Reconstructs a SchemaTemplate (with version_added / version_dropped /
 * physical_pos) from raw page data using CrossPageSampler and
 * PhysicalLayoutAnalyzer, then appends tentative ALTER TABLE sequences
 * explaining the version transitions.
 */

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cmath>

#include "dbrecovery/engine/schema_template.hpp"
#include "dbrecovery/tools/cross_page_sampler.hpp"
#include "dbrecovery/inference/statistical_classifier.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace tools {

// ==================== Inference Config ====================

struct SchemaTemplateInferenceConfig {
    size_t page_size = 16384;
    uint32_t max_pages_to_sample = 200;
    bool detect_schema_variation = true;  // diagnostic: detect column-count differences
    bool verbose = false;
};

// ==================== Inference Stats ====================

struct SchemaTemplateInferenceStats {
    uint32_t pages_scanned = 0;
    uint32_t columns_inferred = 0;
    uint32_t schema_variation_groups = 0;  // number of distinct column-count groups detected
    double total_ms = 0.0;
};

// ==================== SchemaTemplateInference ====================

class SchemaTemplateInference {
public:
    explicit SchemaTemplateInference(const SchemaTemplateInferenceConfig& cfg = {})
        : cfg_(cfg) {}

    /**
     * @brief Infer SchemaTemplate from an .ibd file
     *
     * Pipeline:
     *   1. Sample pages via CrossPageSampler to detect column count consensus
     *   2. Re-scan pages for PAGE_INSTANT_COL (MySQL 8.0.23+ offset 60, BE)
     *   3. Build ColumnTemplate entries with version metadata
     *   4. Reuse StatisticalTypeClassifier for type inference
     *   5. Emit SchemaTemplate + tentative ALTER TABLE sequence
     */
    engine::SchemaTemplate infer_from_file(const std::string& ibd_path) {
        auto t0 = std::chrono::high_resolution_clock::now();

        engine::SchemaTemplate tmpl;
        tmpl.table_name = extract_table_name(ibd_path);

        // Step 1: Sample pages via CrossPageSampler
        CrossPageSampler sampler;
        auto sampling = sampler.sample_file(ibd_path, cfg_.page_size, cfg_.max_pages_to_sample);

        stats_.pages_scanned = sampling.total_pages_scanned;

        if (sampling.detected_column_count == 0) {
            tmpl.expected_column_count = 0;
            auto t_end = std::chrono::high_resolution_clock::now();
            stats_.total_ms = std::chrono::duration<double, std::milli>(t_end - t0).count();
            return tmpl;
        }

        tmpl.expected_column_count = sampling.detected_column_count;

        // Step 2: Detect non-zero PAGE_INSTANT_COL signal (MySQL 8.0.23+).
        // Diagnostic only — a non-zero value is a variation signal, not proof of
        // INSTANT DDL without authoritative schema identity.
        bool found_instant_col = false;
        uint32_t last_instant_col_count = 0;

        if (cfg_.detect_schema_variation) {
            std::ifstream file(ibd_path, std::ios::binary);
            if (file) {
                std::vector<uint8_t> page_buf(cfg_.page_size);
                uint32_t max_pg = cfg_.max_pages_to_sample > 0 ? cfg_.max_pages_to_sample : 200;

                for (uint32_t pg = 0; pg < max_pg; pg++) {
                    file.seekg(static_cast<std::streamoff>(pg) * cfg_.page_size, std::ios::beg);
                    file.read(reinterpret_cast<char*>(page_buf.data()), cfg_.page_size);
                    if (file.gcount() < static_cast<std::streamsize>(cfg_.page_size)) break;

                    uint16_t page_type = innodb::read_be<uint16_t>(page_buf.data(), 24);
                    if (page_type != innodb::FIL_PAGE_INDEX) continue;

                    uint16_t instant_col = innodb::read_be<uint16_t>(page_buf.data(), 60);
                    if (instant_col > 0 && instant_col <= sampling.detected_column_count) {
                        found_instant_col = true;
                        if (instant_col > last_instant_col_count) {
                            last_instant_col_count = instant_col;
                        }
                    }
                }

                if (found_instant_col) {
                    // Diagnostic only: flag that a non-zero PAGE_INSTANT_COL signal exists.
                    // Not proof of proven INSTANT DDL — no authoritative schema identity here.
                    stats_.schema_variation_groups = (stats_.schema_variation_groups > 0) ? stats_.schema_variation_groups : 2;
                }
            }
        }

        // Step 3: Build ColumnTemplate entries
        const auto& consensus = sampling.consensus;
        uint32_t n_cols = sampling.detected_column_count;

        // Use StatisticalTypeClassifier for type inference per column
        inference::StatisticalTypeClassifier classifier;

        for (uint32_t c = 0; c < n_cols; c++) {
            engine::SchemaTemplate::Column col;

            col.physical_pos = c;

            // Determine version_added / version_dropped
            if (found_instant_col && c >= last_instant_col_count) {
                // Columns beyond the instant COL count were added by a later DDL
                col.version_added = 2;
                col.version_dropped = 66; // not dropped
            } else {
                col.version_added = 1; // original schema version
                col.version_dropped = 66; // not dropped
            }

            // Type inference from FieldStats if available
            if (c < consensus.field_stats.size()) {
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

                auto candidates = classifier.classify(features, 1);

                if (!candidates.empty()) {
                    const auto& best = candidates.front();
                    col.type_name = best.type_name;
                    col.mysql_type = map_type_name_to_id(best.type_name);
                    col.is_nullable = (fs.null_ratio > 0.0);
                    col.is_variable_length = !features.is_fixed_length;
                    col.max_length = static_cast<uint32_t>(features.len_max);
                    col.char_length = static_cast<uint32_t>(features.len_max);
                    col.is_signed = true;
                    col.datetime_precision = infer_datetime_precision(best.type_name, features);
                    col.mbmaxlen = infer_mbmaxlen(best.type_name);
                    col.charset_id = infer_charset_id(best.type_name);
                } else {
                    col.type_name = "BLOB";
                    col.mysql_type = 23;
                    col.is_variable_length = true;
                }

                // PK heuristic: high distinct ratio + integer type
                double distinct_ratio = (fs.sample_count > 0)
                    ? static_cast<double>(fs.distinct_count) / fs.sample_count : 0.0;
                if (distinct_ratio > 0.80 && col.type_name.find("INT") != std::string::npos) {
                    col.is_primary_key = true;
                    if (col.type_name == "BIGINT" || col.type_name == "INT") {
                        col.is_auto_increment = true;
                    }
                    tmpl.primary_key_columns.push_back(c);
                }
            } else {
                col.type_name = "BLOB";
                col.mysql_type = 23;
                col.is_variable_length = true;
            }

            col.name = suggest_name(c, col);
            tmpl.columns.push_back(std::move(col));
        }

        // Step 4: Compute max_row_version
        tmpl.max_row_version = 1;
        if (found_instant_col) {
            tmpl.max_row_version = 2;
        }
        for (const auto& col : tmpl.columns) {
            if (col.version_dropped != 66 && col.version_dropped > tmpl.max_row_version) {
                tmpl.max_row_version = col.version_dropped;
            }
        }

        // Step 5: Append tentative ALTER TABLE sequence
        tmpl.hints.expected_min_records = sampling.total_records;

        auto t_end = std::chrono::high_resolution_clock::now();
        stats_.total_ms = std::chrono::duration<double, std::milli>(t_end - t0).count();
        stats_.columns_inferred = static_cast<uint32_t>(tmpl.columns.size());

        return tmpl;
    }

    /**
     * @brief Generate a tentative ALTER TABLE sequence explaining
     *        the version transitions inferred from the schema template.
     */
    static std::string generate_alter_sequence(const engine::SchemaTemplate& tmpl) {
        if (tmpl.columns.empty()) return "-- No columns to generate ALTER TABLE for.\n";

        std::ostringstream oss;
        oss << "-- Tentative ALTER TABLE sequence for `" << tmpl.table_name << "`\n";
        oss << "-- Inferred " << tmpl.columns.size() << " columns, "
            << tmpl.max_row_version << " row version(s)\n\n";

        // Version 1: CREATE TABLE (columns with version_added == 1)
        oss << "-- Version 1 (original schema):\n";
        oss << "CREATE TABLE `" << tmpl.table_name << "` (\n";
        bool first_v1 = true;
        for (const auto& col : tmpl.columns) {
            if (col.version_added == 1 && col.version_dropped == 66) {
                if (!first_v1) oss << ",\n";
                first_v1 = false;
                oss << "  `" << col.name << "` " << col.type_name;
                if (!col.is_nullable) oss << " NOT NULL";
                if (col.is_auto_increment) oss << " AUTO_INCREMENT";
            }
        }
        if (!tmpl.primary_key_columns.empty()) {
            oss << ",\n  PRIMARY KEY (";
            for (size_t i = 0; i < tmpl.primary_key_columns.size(); i++) {
                if (i > 0) oss << ", ";
                oss << "`" << tmpl.columns[tmpl.primary_key_columns[i]].name << "`";
            }
            oss << ")";
        }
        oss << "\n) ENGINE=InnoDB;\n\n";

        // Version 2+ transitions: ADD COLUMN
        for (uint16_t v = 2; v <= tmpl.max_row_version; v++) {
            bool has_add = false, has_drop = false;
            std::ostringstream add_oss, drop_oss;

            for (const auto& col : tmpl.columns) {
                if (col.version_added == v) {
                    has_add = true;
                    add_oss << "  ADD COLUMN `" << col.name << "` " << col.type_name;
                    if (!col.is_nullable) add_oss << " NOT NULL";
                    add_oss << ",\n";
                }
                if (col.version_dropped == v) {
                    has_drop = true;
                    drop_oss << "  DROP COLUMN `" << col.name << "`,\n";
                }
            }

            if (has_add || has_drop) {
                oss << "-- Version " << v << " (DDL change):\n";
                oss << "ALTER TABLE `" << tmpl.table_name << "`\n";
                oss << drop_oss.str();
                oss << add_oss.str();
                oss.seekp(-2, std::ios_base::end); // remove trailing ",\n"
                oss << ";\n\n";
            }
        }

        return oss.str();
    }

    const SchemaTemplateInferenceStats& get_stats() const { return stats_; }

private:
    SchemaTemplateInferenceConfig cfg_;
    SchemaTemplateInferenceStats stats_;

    static std::string extract_table_name(const std::string& path) {
        size_t slash = path.find_last_of("/\\");
        std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        if (base.empty()) base = "inferred_table";
        return base;
    }

    static uint8_t map_type_name_to_id(const std::string& type_name) {
        // Strip parenthesized suffix like "CHAR(N)" -> "CHAR", "VARCHAR(255)" -> "VARCHAR"
        std::string base = type_name;
        size_t paren = base.find('(');
        if (paren != std::string::npos) base = base.substr(0, paren);
        // Trim trailing spaces
        while (!base.empty() && base.back() == ' ') base.pop_back();

        if (base == "TINYINT") return 1;
        if (base == "SMALLINT") return 2;
        if (base == "MEDIUMINT") return 9;
        if (base == "INT") return 3;
        if (base == "BIGINT") return 8;
        if (base == "FLOAT") return 4;
        if (base == "DOUBLE") return 5;
        if (base == "DECIMAL") return 6;
        if (base == "DATETIME") return 12;
        if (base == "TIMESTAMP") return 7;
        if (base == "TIME") return 11;
        if (base == "DATE") return 10;
        if (base == "YEAR") return 13;
        if (base == "VARCHAR") return 15;
        if (base == "CHAR") return 14;
        if (base == "TEXT") return 16;
        if (base == "TINYTEXT") return 16;
        if (base == "MEDIUMTEXT") return 16;
        if (base == "LONGTEXT") return 16;
        if (base == "BLOB") return 23;
        if (base == "TINYBLOB") return 23;
        if (base == "MEDIUMBLOB") return 23;
        if (base == "LONGBLOB") return 23;
        if (base == "JSON") return 31;
        if (base == "ENUM") return 18;
        if (base == "SET") return 19;
        if (base == "BINARY") return 20;
        if (base == "VARBINARY") return 21;
        if (base == "BIT") return 15;
        return 0;
    }

    static uint8_t infer_datetime_precision(
        const std::string& type_name,
        const inference::ColumnFeatures& features) {
        if (type_name == "DATETIME" || type_name == "TIMESTAMP" || type_name == "TIME") {
            double avg_len = features.len_mean;
            if (avg_len > 5) return 6;
            if (avg_len > 4) return 2;
            return 0;
        }
        return 0;
    }

    static uint32_t infer_mbmaxlen(const std::string& type_name) {
        if (type_name == "VARCHAR" || type_name == "CHAR" ||
            type_name == "TEXT" || type_name == "TINYTEXT" ||
            type_name == "MEDIUMTEXT" || type_name == "LONGTEXT") {
            return 4; // utf8mb4 default
        }
        return 1;
    }

    static uint32_t infer_charset_id(const std::string& type_name) {
        if (type_name == "VARCHAR" || type_name == "CHAR" ||
            type_name == "TEXT" || type_name == "TINYTEXT" ||
            type_name == "MEDIUMTEXT" || type_name == "LONGTEXT") {
            return 45; // utf8mb4 (ID 45 in MySQL)
        }
        return 0; // binary
    }

    static std::string suggest_name(uint32_t ordinal, const engine::SchemaTemplate::Column& col) {
        if (col.is_primary_key && ordinal == 0) return "id";
        if (col.type_name.find("DATETIME") != std::string::npos ||
            col.type_name.find("TIMESTAMP") != std::string::npos) {
            if (ordinal == 0) return "created_at";
            if (ordinal == 1) return "updated_at";
            return "event_time";
        }
        if (col.type_name == "DATE") return "event_date";
        if (col.type_name == "JSON") return "metadata";
        if (col.type_name.find("INT") != std::string::npos) {
            return "col_" + std::to_string(ordinal);
        }
        if (col.type_name.find("VARCHAR") != std::string::npos ||
            col.type_name == "TEXT") {
            return "col_" + std::to_string(ordinal);
        }
        // Spreadsheet-style fallback
        std::string name;
        uint32_t n = ordinal;
        do {
            name += static_cast<char>('A' + (n % 26));
            n = n / 26;
        } while (n > 0);
        std::reverse(name.begin(), name.end());
        return name;
    }
};

} // namespace tools
} // namespace dbrecovery

#endif // DBRECOVERY_TOOLS_SCHEMA_TEMPLATE_INFERENCE_HPP