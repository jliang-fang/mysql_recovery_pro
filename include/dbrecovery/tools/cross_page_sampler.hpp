#ifndef DBRECOVERY_TOOLS_CROSS_PAGE_SAMPLER_HPP
#define DBRECOVERY_TOOLS_CROSS_PAGE_SAMPLER_HPP

/**
 * @file cross_page_sampler.hpp
 * @brief Multi-page field sampling aggregator for heuristic schema inference
 *
 * Reads multiple INDEX pages from an IBD file, detects the physical layout
 * (column count, NULL bitmap, field offsets), aggregates field-level statistics
 * across pages, and detects schema version changes (DDL evolution).
 */

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <optional>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <fstream>

#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/inference/statistical_classifier.hpp"

namespace dbrecovery {
namespace tools {

// ==================== Field-Level Statistics ====================

struct FieldStats {
    uint32_t sample_count = 0;
    double null_ratio = 0.0;
    double len_min = 0.0;
    double len_max = 0.0;
    double len_mean = 0.0;
    double len_median = 0.0;
    double len_stddev = 0.0;
    bool is_fixed_length = false;
    uint32_t fixed_size = 0;
    uint32_t distinct_count = 0;
    double ascii_ratio = 0.0;
    double digit_ratio = 0.0;
    double alpha_ratio = 0.0;
    double alphanum_ratio = 0.0;
    double entropy = 0.0;

    // Numeric analysis (computed from byte-interpreted values)
    double integer_ratio = 0.0;
    double numeric_ratio = 0.0;
    double float_ratio = 0.0;
    double signed_ratio = 0.0;
    double zero_ratio = 0.0;
    double empty_ratio = 0.0;
    std::optional<uint64_t> numeric_min;
    std::optional<uint64_t> numeric_max;

    // Character-class ratios
    double has_sign_ratio = 0.0;
    double has_dot_ratio = 0.0;
    double has_colon_ratio = 0.0;
    double has_dash_ratio = 0.0;
    double has_space_ratio = 0.0;
    double has_at_ratio = 0.0;
    double has_slash_ratio = 0.0;
    double has_brace_ratio = 0.0;
    double has_bracket_ratio = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "FieldStats{n=" << sample_count
            << " len=[" << len_min << "-" << len_max << "]"
            << " mean=" << std::fixed << std::setprecision(1) << len_mean
            << " null=" << null_ratio
            << " ascii=" << ascii_ratio
            << " int=" << integer_ratio
            << " float=" << float_ratio
            << " fixed=" << (is_fixed_length ? "Y" : "N")
            << "}";
        return oss.str();
    }
};

// ==================== Schema Version ====================

struct SchemaVersion {
    uint32_t n_fields = 0;
    uint32_t first_page_no = 0;
    uint32_t last_page_no = 0;
    double row_fraction = 0.0;
    std::vector<FieldStats> field_stats;

    bool is_valid() const { return n_fields > 0 && !field_stats.empty(); }
};

// ==================== Sampling Result ====================

struct SamplingResult {
    std::vector<SchemaVersion> versions;
    SchemaVersion consensus;
    uint32_t total_pages_scanned = 0;
    uint32_t total_records = 0;
    uint32_t detected_column_count = 0;
    bool has_ddl_change = false;

    std::string to_report() const {
        std::ostringstream oss;
        oss << "CrossPageSampler Report\n";
        oss << "=======================\n";
        oss << "Pages scanned: " << total_pages_scanned << "\n";
        oss << "Total records: " << total_records << "\n";
        oss << "Detected columns: " << detected_column_count << "\n";
        oss << "Schema versions: " << versions.size() << "\n";
        oss << "DDL change detected: " << (has_ddl_change ? "YES" : "NO") << "\n";
        for (size_t v = 0; v < versions.size(); v++) {
            const auto& sv = versions[v];
            oss << "  Version " << v << ": n_fields=" << sv.n_fields
                << " row_fraction=" << std::fixed << std::setprecision(2)
                << sv.row_fraction << "\n";
        }
        return oss.str();
    }
};

// ==================== CrossPageSampler ====================

class CrossPageSampler {
public:
    CrossPageSampler() = default;

    SamplingResult sample_file(
        const std::string& ibd_path,
        size_t page_size = 16384,
        uint32_t max_pages = 0);

    SamplingResult sample_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);

    static FieldStats compute_field_stats(
        const std::vector<std::vector<uint8_t>>& values);
};

// ==================== Inline Implementations ====================

inline FieldStats CrossPageSampler::compute_field_stats(
    const std::vector<std::vector<uint8_t>>& values) {

    FieldStats stats;
    stats.sample_count = static_cast<uint32_t>(values.size());
    if (values.empty()) return stats;

    double null_count = 0, ascii_count = 0, digit_count = 0, alpha_count = 0, alphanum_count = 0;
    double integer_cnt = 0, float_cnt = 0, numeric_cnt = 0, signed_cnt = 0;
    double zero_count = 0, empty_count = 0;
    double dot_cnt = 0, colon_cnt = 0, dash_cnt = 0, space_cnt = 0;
    double at_cnt = 0, slash_cnt = 0, brace_cnt = 0, bracket_cnt = 0;
    uint64_t num_min = UINT64_MAX, num_max = 0;
    std::vector<double> lengths;
    std::set<std::vector<uint8_t>> distinct;
    std::vector<uint8_t> byte_hist(256, 0);

    for (const auto& v : values) {
        if (v.empty()) {
            null_count++; lengths.push_back(0);
            zero_count++; empty_count++;
            continue;
        }
        distinct.insert(v);
        lengths.push_back(static_cast<double>(v.size()));

        bool all_ascii = true, all_digit = true, has_alpha = false, has_digit = false;
        bool has_sign = false, has_dot = false, has_colon = false, has_dash = false;
        bool has_space = false, has_at = false, has_slash = false;
        bool has_brace = false, has_bracket = false;
        bool all_zero = true;

        for (uint8_t b : v) {
            byte_hist[b]++;
            if (b != 0) all_zero = false;
            if (b < 0x20 || b > 0x7E) all_ascii = false;
            if (b < '0' || b > '9') all_digit = false;
            if (b >= 'a' && b <= 'z') has_alpha = true;
            if (b >= 'A' && b <= 'Z') has_alpha = true;
            if (b >= '0' && b <= '9') has_digit = true;
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
        if (has_digit && !has_alpha) { digit_count++; }
        if (has_alpha && !has_digit) alpha_count++;
        if (has_digit || has_alpha) alphanum_count++;
        if (has_sign) signed_cnt++;
        if (has_dot) dot_cnt++;
        if (has_colon) colon_cnt++;
        if (has_dash) dash_cnt++;
        if (has_space) space_cnt++;
        if (has_at) at_cnt++;
        if (has_slash) slash_cnt++;
        if (has_brace) brace_cnt++;
        if (has_bracket) bracket_cnt++;

        // Numeric parsing (same as FeatureExtractor)
        if (has_digit && !has_alpha && !has_slash && !has_at && !has_colon && !has_brace && !has_bracket) {
            std::string val_str(v.begin(), v.end());
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

    double n = static_cast<double>(values.size());
    stats.null_ratio = null_count / n;
    stats.ascii_ratio = ascii_count / n;
    stats.digit_ratio = digit_count / n;
    stats.alpha_ratio = alpha_count / n;
    stats.alphanum_ratio = alphanum_count / n;
    stats.distinct_count = static_cast<uint32_t>(distinct.size());
    stats.zero_ratio = zero_count / n;
    stats.empty_ratio = empty_count / n;
    stats.integer_ratio = integer_cnt / n;
    stats.float_ratio = float_cnt / n;
    stats.numeric_ratio = numeric_cnt / n;
    stats.signed_ratio = signed_cnt / n;
    stats.has_sign_ratio = signed_cnt / n;
    stats.has_dot_ratio = dot_cnt / n;
    stats.has_colon_ratio = colon_cnt / n;
    stats.has_dash_ratio = dash_cnt / n;
    stats.has_space_ratio = space_cnt / n;
    stats.has_at_ratio = at_cnt / n;
    stats.has_slash_ratio = slash_cnt / n;
    stats.has_brace_ratio = brace_cnt / n;
    stats.has_bracket_ratio = bracket_cnt / n;

    if (num_max > 0 || num_min < UINT64_MAX) {
        stats.numeric_min = (num_min < UINT64_MAX) ? std::optional<uint64_t>(num_min) : std::nullopt;
        stats.numeric_max = (num_max > 0) ? std::optional<uint64_t>(num_max) : std::nullopt;
    }

    if (!lengths.empty()) {
        std::sort(lengths.begin(), lengths.end());
        stats.len_min = lengths.front();
        stats.len_max = lengths.back();
        stats.len_mean = std::accumulate(lengths.begin(), lengths.end(), 0.0) / lengths.size();
        stats.len_median = lengths[lengths.size() / 2];

        double sq_sum = 0;
        for (double l : lengths) sq_sum += (l - stats.len_mean) * (l - stats.len_mean);
        stats.len_stddev = std::sqrt(sq_sum / lengths.size());

        stats.is_fixed_length = (stats.len_stddev < 0.5 && stats.len_min > 0
                                 && stats.len_max == stats.len_min);
        stats.fixed_size = stats.is_fixed_length ? static_cast<uint32_t>(stats.len_min) : 0;
    }

    double entropy = 0.0;
    for (size_t i = 0; i < byte_hist.size(); i++) {
        if (byte_hist[i] > 0) {
            double p = static_cast<double>(byte_hist[i]) / n;
            entropy -= p * std::log2(p);
        }
    }
    stats.entropy = entropy;

    return stats;
}

inline SamplingResult CrossPageSampler::sample_file(
    const std::string& ibd_path,
    size_t page_size,
    uint32_t max_pages) {

    SamplingResult result;

    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file) return result;

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size);
    if (max_pages > 0 && max_pages < total_pages) {
        total_pages = max_pages;
    }

    std::vector<uint8_t> page_buf(page_size);
    std::map<uint32_t, uint32_t> col_count_votes;
    std::map<uint32_t, uint32_t> col_count_records;

    // First pass: detect column count consensus
    for (uint32_t pg = 0; pg < total_pages; pg++) {
        file.seekg(static_cast<std::streamoff>(pg) * page_size, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buf.data()), page_size);
        if (file.gcount() < static_cast<std::streamsize>(page_size)) break;

        result.total_pages_scanned++;

        uint16_t page_type = innodb::read_be<uint16_t>(page_buf.data(), 24);
        if (page_type != innodb::FIL_PAGE_INDEX) continue;

        auto layout = inference::PhysicalLayoutAnalyzer::analyze_page(
            page_buf.data(), page_size);
        if (!layout) continue;

        uint32_t n_cols = layout->total_columns;
        if (n_cols == 0) continue;

        uint16_t n_recs = innodb::read_be<uint16_t>(page_buf.data(), 54);
        result.total_records += n_recs;

        col_count_votes[n_cols]++;
        col_count_records[n_cols] += n_recs;
    }

    if (col_count_votes.empty()) return result;

    uint32_t max_votes = 0;
    uint32_t consensus_cols = 0;
    for (const auto& [n_cols, votes] : col_count_votes) {
        if (votes > max_votes) { max_votes = votes; consensus_cols = n_cols; }
    }

    result.detected_column_count = consensus_cols;
    result.has_ddl_change = col_count_votes.size() > 1;

    // Build all SchemaVersion entries
    for (const auto& [n_cols, count] : col_count_records) {
        SchemaVersion sv;
        sv.n_fields = n_cols;
        sv.row_fraction = result.total_records > 0
            ? static_cast<double>(count) / result.total_records : 0.0;
        result.versions.push_back(sv);
    }

    // Second pass: extract column values from consensus pages
    inference::PhysicalLayoutAnalyzer::LayoutInfo layout;
    layout.total_columns = consensus_cols;
    layout.user_columns = consensus_cols > 3 ? consensus_cols - 3 : consensus_cols;
    layout.null_bitmap_bytes = (layout.user_columns + 7) / 8;
    layout.is_compact_format = true;
    layout.row_format = 1;

    std::vector<std::pair<const uint8_t*, size_t>> consensus_pages;
    std::vector<std::vector<uint8_t>> page_storage;
    uint32_t sampled = 0;
    uint32_t max_sample = 50;

    file.seekg(0, std::ios::beg);
    for (uint32_t pg = 0; pg < total_pages && sampled < max_sample; pg++) {
        file.seekg(static_cast<std::streamoff>(pg) * page_size, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buf.data()), page_size);
        if (file.gcount() < static_cast<std::streamsize>(page_size)) break;

        uint16_t page_type = innodb::read_be<uint16_t>(page_buf.data(), 24);
        if (page_type != innodb::FIL_PAGE_INDEX) continue;

        auto pl = inference::PhysicalLayoutAnalyzer::analyze_page(
            page_buf.data(), page_size);
        if (!pl || pl->total_columns != consensus_cols) continue;

        page_storage.push_back(page_buf);
        consensus_pages.emplace_back(page_storage.back().data(), page_size);
        sampled++;
    }

    auto column_values = inference::PhysicalLayoutAnalyzer::extract_column_values(
        consensus_pages, layout, 5000);
    column_values.resize(consensus_cols);

    std::vector<FieldStats> field_stats(consensus_cols);
    for (uint32_t c = 0; c < consensus_cols; c++) {
        field_stats[c] = compute_field_stats(column_values[c]);
    }

    result.consensus.n_fields = consensus_cols;
    result.consensus.row_fraction = 1.0;
    result.consensus.field_stats = field_stats;

    return result;
}

inline SamplingResult CrossPageSampler::sample_pages(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages) {

    SamplingResult result;
    result.total_pages_scanned = static_cast<uint32_t>(pages.size());

    std::map<uint32_t, uint32_t> col_count_votes;
    std::map<uint32_t, uint32_t> col_count_records;

    for (const auto& [data, size] : pages) {
        auto layout = inference::PhysicalLayoutAnalyzer::analyze_page(data, size);
        if (!layout) continue;

        uint32_t n_cols = layout->total_columns;
        col_count_votes[n_cols]++;

        uint16_t n_recs = innodb::read_be<uint16_t>(data, 54);
        col_count_records[n_cols] += n_recs;
        result.total_records += n_recs;
    }

    if (col_count_votes.empty()) return result;

    uint32_t max_votes = 0;
    uint32_t consensus_cols = 0;
    for (const auto& [n_cols, votes] : col_count_votes) {
        if (votes > max_votes) { max_votes = votes; consensus_cols = n_cols; }
    }

    result.detected_column_count = consensus_cols;
    result.has_ddl_change = col_count_votes.size() > 1;

    // Build all SchemaVersion entries
    for (const auto& [n_cols, count] : col_count_records) {
        SchemaVersion sv;
        sv.n_fields = n_cols;
        sv.row_fraction = result.total_records > 0
            ? static_cast<double>(count) / result.total_records : 0.0;
        result.versions.push_back(sv);
    }

    // Build consensus layout
    inference::PhysicalLayoutAnalyzer::LayoutInfo layout;
    layout.total_columns = consensus_cols;
    layout.user_columns = consensus_cols > 3 ? consensus_cols - 3 : consensus_cols;
    layout.null_bitmap_bytes = (layout.user_columns + 7) / 8;
    layout.is_compact_format = true;
    layout.row_format = 1;

    auto column_values = inference::PhysicalLayoutAnalyzer::extract_column_values(
        pages, layout, 5000);
    column_values.resize(consensus_cols);

    std::vector<FieldStats> field_stats(consensus_cols);
    for (uint32_t c = 0; c < consensus_cols; c++) {
        field_stats[c] = compute_field_stats(column_values[c]);
    }

    result.consensus.n_fields = consensus_cols;
    result.consensus.row_fraction = 1.0;
    result.consensus.field_stats = field_stats;

    return result;
}

} // namespace tools
} // namespace dbrecovery

#endif // DBRECOVERY_TOOLS_CROSS_PAGE_SAMPLER_HPP