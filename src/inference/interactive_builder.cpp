/**
 * @file interactive_builder.cpp
 * @brief InteractiveBuilder implementation
 */

#include "dbrecovery/inference/interactive_builder.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <fstream>

namespace dbrecovery {
namespace inference {

// ---- Page data loading ----

size_t InteractiveBuilder::load_ibd_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return 0;

    uint64_t file_size = static_cast<uint64_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    // Default page size for detection
    size_t page_size = 16384;
    auto buf = std::make_unique<uint8_t[]>(page_size);
    size_t count = 0;

    for (uint64_t offset = 0; offset + page_size <= file_size; offset += page_size) {
        file.read(reinterpret_cast<char*>(buf.get()), page_size);
        if (file.gcount() < static_cast<std::streamsize>(page_size)) break;

        auto copy = std::make_unique<uint8_t[]>(page_size);
        std::memcpy(copy.get(), buf.get(), page_size);
        page_data_owned_.emplace_back(copy.release(), page_size);
        count++;
    }

    return count;
}

void InteractiveBuilder::add_page_data(const uint8_t* data, size_t size) {
    auto copy = std::make_unique<uint8_t[]>(size);
    std::memcpy(copy.get(), data, size);
    page_data_owned_.emplace_back(copy.release(), size);
}

void InteractiveBuilder::add_column_samples(const std::vector<std::vector<uint8_t>>& raw_values) {
    column_samples_.push_back(raw_values);
}

// ---- Layout analysis ----

std::optional<PhysicalLayoutAnalyzer::LayoutInfo> InteractiveBuilder::analyze_layout() {
    // Build a vector of page views from owned data
    std::vector<std::pair<const uint8_t*, size_t>> pages;
    for (const auto& [data, size] : page_data_owned_) {
        pages.emplace_back(data, size);
    }
    for (const auto& [data, size] : page_data_views_) {
        pages.emplace_back(data, size);
    }

    if (pages.empty()) return std::nullopt;

    layout_ = PhysicalLayoutAnalyzer::analyze_multiple_pages(pages);
    return layout_;
}

std::vector<std::vector<std::vector<uint8_t>>> InteractiveBuilder::extract_column_values(
    const PhysicalLayoutAnalyzer::LayoutInfo& layout,
    uint32_t max_samples) {

    std::vector<std::pair<const uint8_t*, size_t>> pages;
    for (const auto& [data, size] : page_data_owned_) {
        pages.emplace_back(data, size);
    }
    for (const auto& [data, size] : page_data_views_) {
        pages.emplace_back(data, size);
    }

    column_samples_ = PhysicalLayoutAnalyzer::extract_column_values(
        pages, layout, max_samples);
    return column_samples_;
}

// ---- Type inference ----

SchemaInferenceResult InteractiveBuilder::infer_types(
    const std::vector<std::vector<std::vector<uint8_t>>>& column_values,
    const std::vector<std::string>& column_hints) {

    SchemaInferenceResult result;
    result.detected_column_count = static_cast<uint32_t>(column_values.size());

    for (uint32_t ord = 0; ord < column_values.size(); ord++) {
        const auto& raw_values = column_values[ord];
        if (raw_values.empty()) continue;

        // Extract features
        ColumnFeatures features = FeatureExtractor::extract(raw_values);

        // Classify
        auto candidates = classifier_.classify(features, 3);

        InferredColumnV2 col;
        col.ordinal_position = ord;
        col.candidates = candidates;

        if (!candidates.empty()) {
            col.best_guess = candidates[0];
            col.overall_confidence = candidates[0].confidence;
        }

        // Apply column hint if provided
        if (ord < column_hints.size() && !column_hints[ord].empty()) {
            col.suggested_name = column_hints[ord];
        } else {
            col.suggested_name = "col_" + std::to_string(ord);
        }

        col.is_nullable = features.null_ratio > 0.0;
        col.is_primary_key_candidate = features.null_ratio == 0.0 &&
                                       features.distinct_count == features.sample_count;
        col.is_auto_increment_candidate = features.numeric_ratio > 0.9 &&
                                          features.integer_ratio > 0.9 &&
                                          features.null_ratio == 0.0;

        result.columns.push_back(col);
    }

    result.overall_confidence = 0.0;
    for (const auto& col : result.columns) {
        result.overall_confidence += col.overall_confidence;
    }
    if (!result.columns.empty()) {
        result.overall_confidence /= static_cast<double>(result.columns.size());
    }

    inference_result_ = result;
    column_states_.clear();

    // Initialize column states
    for (const auto& col : result.columns) {
        ColumnState state;
        state.inferred = col;
        state.accepted = false;
        state.rejected = false;
        column_states_.push_back(state);
    }

    return result;
}

// ---- Interactive refinement ----

void InteractiveBuilder::apply_correction(const UserCorrection& correction) {
    for (auto& state : column_states_) {
        if (state.inferred.ordinal_position == correction.ordinal_position) {
            state.correction = correction;
            state.accepted = true;
            state.rejected = false;
            return;
        }
    }
}

void InteractiveBuilder::confirm_column(uint32_t ordinal_position) {
    for (auto& state : column_states_) {
        if (state.inferred.ordinal_position == ordinal_position) {
            state.accepted = true;
            state.rejected = false;
            return;
        }
    }
}

void InteractiveBuilder::reject_column(uint32_t ordinal_position) {
    for (auto& state : column_states_) {
        if (state.inferred.ordinal_position == ordinal_position) {
            state.rejected = true;
            state.accepted = false;
            return;
        }
    }
}

// ---- DDL generation ----

std::string InteractiveBuilder::generate_ddl(const std::string& table_name) const {
    std::ostringstream ddl;
    ddl << "CREATE TABLE " << table_name << " (\n";

    bool first = true;
    for (const auto& state : column_states_) {
        if (!first) ddl << ",\n";
        first = false;

        ddl << "  ";

        // Column name
        if (state.correction && state.correction->corrected_name) {
            ddl << *state.correction->corrected_name;
        } else {
            ddl << state.inferred.suggested_name;
        }

        ddl << " ";

        // Column type
        if (state.correction && !state.correction->corrected_type_name.empty()) {
            ddl << state.correction->corrected_type_name;
        } else {
            ddl << state.inferred.best_guess.type_name;
        }

        // Nullability
        bool nullable = state.inferred.is_nullable;
        if (state.correction) nullable = state.correction->is_nullable;

        if (!nullable) {
            ddl << " NOT NULL";
        } else {
            ddl << " NULL";
        }

        // Auto-increment hint
        if (state.inferred.is_auto_increment_candidate) {
            ddl << " AUTO_INCREMENT";
        }
    }

    // Primary key
    std::vector<uint32_t> pk_candidates;
    for (const auto& state : column_states_) {
        if (state.inferred.is_primary_key_candidate && !state.rejected) {
            pk_candidates.push_back(state.inferred.ordinal_position);
        }
    }

    if (!pk_candidates.empty()) {
        ddl << ",\n  PRIMARY KEY (";
        bool pk_first = true;
        for (auto ord : pk_candidates) {
            if (!pk_first) ddl << ", ";
            pk_first = false;
            for (const auto& state : column_states_) {
                if (state.inferred.ordinal_position == ord) {
                    if (state.correction && state.correction->corrected_name) {
                        ddl << *state.correction->corrected_name;
                    } else {
                        ddl << state.inferred.suggested_name;
                    }
                    break;
                }
            }
        }
        ddl << ")";
    }

    ddl << "\n) ENGINE=InnoDB;\n";
    return ddl.str();
}

// ---- Schema export ----

catalog::TableSchema InteractiveBuilder::export_schema(const std::string& table_name) const {
    catalog::TableSchema schema;
    schema.table_name = table_name;

    for (const auto& state : column_states_) {
        catalog::ColumnInfo col;
        col.ordinal = state.inferred.ordinal_position;
        col.name = state.inferred.suggested_name;
        col.mysql_type_id = state.inferred.best_guess.innodb_type_id;
        col.is_nullable = state.inferred.is_nullable;
        schema.columns[col.ordinal] = col;
    }

    return schema;
}

// ---- Reset ----

void InteractiveBuilder::clear() {
    page_data_owned_.clear();
    page_data_views_.clear();
    column_samples_.clear();
    layout_.reset();
    inference_result_ = SchemaInferenceResult{};
    column_states_.clear();
}

} // namespace inference
} // namespace dbrecovery