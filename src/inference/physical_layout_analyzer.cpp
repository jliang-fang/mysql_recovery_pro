/**
 * @file physical_layout_analyzer.cpp
 * @brief Implementation of PhysicalLayoutAnalyzer for schemaless column layout inference
 *
 * Algorithm:
 * 1. Read page directory to find all record offsets
 * 2. For each record: read offset table (reverse), calculate NULL bitmap size
 * 3. Locate TRX_ID (6B) + ROLL_PTR (7B) = 13B fixed overhead
 * 4. Analyze fixed vs variable parts
 * 5. Vote across records for most likely column count
 */

#include "dbrecovery/inference/physical_layout_analyzer.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>
#include <limits>
#include <numeric>

namespace dbrecovery {
namespace inference {

// ==================== PhysicalLayoutAnalyzer ====================

LayoutInfo PhysicalLayoutAnalyzer::analyze_page(
    const uint8_t* page_data, size_t page_size) {

    LayoutInfo info;

    if (!page_data || page_size < 100) return info;

    // Check FIL header page type (offset 24, big-endian, 2 bytes)
    uint16_t page_type = (static_cast<uint16_t>(page_data[24]) << 8) | page_data[25];
    if (page_type != innodb::FIL_PAGE_INDEX) return info;

    // Read PAGE_N_RECS from page header (offset 54, little-endian)
    uint16_t n_recs = (static_cast<uint16_t>(page_data[55]) << 8) | page_data[54];
    if (n_recs == 0 || n_recs > 1000) return info;

    // Read PAGE_HEAP_TOP (offset 40, little-endian)
    uint16_t heap_top = (static_cast<uint16_t>(page_data[41]) << 8) | page_data[40];
    if (heap_top < 120 || heap_top > page_size - 8) return info;

    // Read PAGE_DIRECTION (offset 52, little-endian) — not strictly needed but useful
    // uint16_t page_dir = (static_cast<uint16_t>(page_data[53]) << 8) | page_data[52];

    // Read the page directory to find record offsets
    // Page directory starts at the end of the page and grows backward
    // The first entry (last 2 bytes of page) = number of directory slots
    // Each slot is 2 bytes (little-endian offset of the record)
    uint16_t dir_end = page_size - 2;
    uint16_t n_slots = (static_cast<uint16_t>(page_data[dir_end + 1]) << 8) | page_data[dir_end];
    if (n_slots == 0 || n_slots > 1000) return info;

    // Collect all user record offsets from the page directory
    // Skip first slot (INFIMUM) and last slot (SUPREMUM)
    std::vector<uint16_t> rec_offsets;
    for (uint16_t i = 1; i + 1 < n_slots; i++) {
        uint16_t slot_pos = dir_end - (n_slots - i) * 2;
        uint16_t rec_off = (static_cast<uint16_t>(page_data[slot_pos + 1]) << 8) | page_data[slot_pos];
        if (rec_off >= 120 && rec_off < heap_top) {
            rec_offsets.push_back(rec_off);
        }
    }

    // If page directory is unreadable, fall back to INFIMUM chain traversal
    if (rec_offsets.empty()) {
        // Read first user record offset from INFIMUM's next_offset (offsets 100-101, LE)
        uint16_t rec_off = (static_cast<uint16_t>(page_data[101]) << 8) | page_data[100];
        uint16_t max_recs = n_recs;
        for (uint16_t i = 0; i < max_recs && rec_off >= 120 && rec_off < heap_top; i++) {
            rec_offsets.push_back(rec_off);
            uint16_t next_rec = (static_cast<uint16_t>(page_data[rec_off + 1]) << 8) | page_data[rec_off];
            if (next_rec <= rec_off || next_rec >= heap_top) break;
            rec_off = next_rec;
        }
    }

    if (rec_offsets.empty()) return info;

    // Vote for column count across all records
    std::map<uint32_t, uint32_t> col_count_votes;
    struct LayoutVote {
        uint32_t total_columns = 0;
        uint32_t user_columns = 0;
        uint32_t null_bitmap_bytes = 0;
        uint32_t vote_count = 0;
    };
    std::map<uint32_t, LayoutVote> layout_votes;

    for (uint16_t rec_off : rec_offsets) {
        if (rec_off + 4 > heap_top) continue;

        // Read offset table entries (stored in reverse, 2 bytes each, little-endian)
        // First entry is total record length, subsequent entries are field boundaries
        std::vector<uint16_t> field_offsets;
        uint16_t pos = rec_off;

        // Read offset entries until we hit a zero or out-of-order value
        uint16_t first_entry = (static_cast<uint16_t>(page_data[pos + 1]) << 8) | page_data[pos];
        if (first_entry == 0 || first_entry > 8192) continue;

        field_offsets.push_back(first_entry);
        pos += 2;

        while (pos + 2 <= heap_top && field_offsets.size() < 200) {
            uint16_t val = (static_cast<uint16_t>(page_data[pos + 1]) << 8) | page_data[pos];
            if (val == 0 || val >= field_offsets.back()) break;
            field_offsets.push_back(val);
            pos += 2;
        }

        uint32_t col_count = static_cast<uint32_t>(field_offsets.size());
        if (col_count < 3 || col_count > 100) continue;

        // User columns = total - 3 (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID)
        // For REDUNDAND format, the system columns are: DB_ROLL_PTR (7B), DB_TRX_ID (6B), DB_ROW_ID (6B)
        // But the offset table includes all columns including system columns
        uint32_t user_cols = col_count > 3 ? col_count - 3 : col_count;

        // NULL bitmap: ceil(user_columns / 8) bytes
        uint32_t null_bytes = (user_cols + 7) / 8;

        col_count_votes[col_count]++;
        layout_votes[col_count].total_columns = col_count;
        layout_votes[col_count].user_columns = user_cols;
        layout_votes[col_count].null_bitmap_bytes = null_bytes;
        layout_votes[col_count].vote_count++;
    }

    if (col_count_votes.empty()) return info;

    // Find the most voted column count
    uint32_t best_col_count = 0;
    uint32_t best_votes = 0;
    for (const auto& [col_count, votes] : col_count_votes) {
        if (votes > best_votes) {
            best_votes = votes;
            best_col_count = col_count;
        }
    }

    auto best_layout = layout_votes[best_col_count];
    info.total_columns = best_layout.total_columns;
    info.user_columns = best_layout.user_columns;
    info.null_bitmap_bytes = best_layout.null_bitmap_bytes;
    info.is_compact_format = true;

    // Calculate confidence based on vote concentration
    uint32_t total_votes = 0;
    for (const auto& [_, votes] : col_count_votes) {
        total_votes += votes;
    }
    info.confidence = (total_votes > 0) ?
        static_cast<double>(best_votes) / total_votes : 0.0;

    // Now analyze fixed vs variable columns by examining actual field data
    // For each record with the winning column count, extract field sizes
    std::vector<std::vector<uint32_t>> field_lengths(best_col_count);

    for (uint16_t rec_off : rec_offsets) {
        if (rec_off + 4 > heap_top) continue;

        // Re-read offset table
        std::vector<uint16_t> field_offsets;
        uint16_t pos = rec_off;
        for (uint32_t i = 0; i < best_col_count && pos + 2 <= heap_top; i++) {
            uint16_t val = (static_cast<uint16_t>(page_data[pos + 1]) << 8) | page_data[pos];
            if (val == 0 || (i > 0 && val >= field_offsets.back())) break;
            field_offsets.push_back(val);
            pos += 2;
        }

        if (field_offsets.size() != best_col_count) continue;

        // Skip system columns at the end (last 3 columns)
        // In COMPACT format, fields are: user_fields ..., DB_TRX_ID (6B), DB_ROLL_PTR (7B)
        // The offset table stores offsets from the end of the record
        // field_offsets[0] = total record length
        // field_offsets[i] = offset from end for field boundary
        // Field i: position = total_len - field_offsets[i], size = field_offsets[i] - field_offsets[i+1]

        uint16_t data_start = rec_off + best_col_count * 2 + 1; // +1 for info_bits
        data_start += info.null_bitmap_bytes;

        for (uint32_t i = 0; i < best_col_count; i++) {
            uint16_t field_end_offset = (i + 1 < best_col_count) ?
                field_offsets[i + 1] : 0;
            uint32_t field_sz = field_offsets[i] - field_end_offset;
            field_lengths[i].push_back(field_sz);
        }
    }

    // Analyze each column for fixed vs variable length
    uint32_t fixed_total = 0;
    for (uint32_t i = 0; i < best_col_count && i < field_lengths.size(); i++) {
        const auto& lengths = field_lengths[i];
        if (lengths.empty()) continue;

        uint32_t min_len = *std::min_element(lengths.begin(), lengths.end());
        uint32_t max_len = *std::max_element(lengths.begin(), lengths.end());

        // System columns (last 3) have fixed sizes
        bool is_system_col = (i >= best_col_count - 3);

        if (is_system_col || min_len == max_len) {
            // Fixed-length column
            fixed_total += min_len;
        } else {
            // Variable-length column
            info.var_len_offsets.push_back(static_cast<uint32_t>(i));
        }
    }

    info.fixed_size_total = fixed_total;

    return info;
}

LayoutInfo PhysicalLayoutAnalyzer::analyze_multiple_pages(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages) {

    std::map<uint32_t, LayoutInfo> layout_map;
    std::map<uint32_t, uint32_t> col_count_votes;

    for (const auto& [data, size] : pages) {
        auto info = analyze_page(data, size);
        if (info.total_columns > 0) {
            uint32_t key = info.total_columns;
            col_count_votes[key]++;
            if (layout_map.find(key) == layout_map.end() ||
                col_count_votes[key] > col_count_votes[layout_map.rbegin()->first]) {
                layout_map[key] = info;
            }
        }
    }

    LayoutInfo best;
    uint32_t best_votes = 0;
    for (const auto& [col_count, votes] : col_count_votes) {
        if (votes > best_votes) {
            best_votes = votes;
            best = layout_map[col_count];
        }
    }

    // Update confidence to reflect cross-page agreement
    if (!col_count_votes.empty()) {
        uint32_t total = 0;
        for (const auto& [_, votes] : col_count_votes) {
            total += votes;
        }
        best.confidence = static_cast<double>(best_votes) / total;
    }

    return best;
}

// ==================== StatisticalTypeClassifier ====================

std::vector<TypeCandidate> StatisticalTypeClassifier::classify(
    const ColumnFeatures& f) {

    std::vector<TypeCandidate> candidates;

    auto add = [&](const std::string& name, uint32_t type_id, double score, const std::string& reason) {
        if (score > 0.01) {
            candidates.push_back({name, type_id, score, reason});
        }
    };

    add("TINYINT",    3,  likelihood_tinyint(f),     "1-byte integer");
    add("SMALLINT",   4,  likelihood_smallint(f),    "2-byte integer");
    add("INT",        6,  likelihood_int(f),         "4-byte integer");
    add("BIGINT",     7,  likelihood_bigint(f),      "8-byte integer");
    add("FLOAT",      8,  likelihood_float(f),       "4-byte floating point");
    add("DOUBLE",     9,  likelihood_double(f),      "8-byte floating point");
    add("DECIMAL",    10, likelihood_decimal(f),     "Fixed-point decimal");
    add("CHAR(N)",    16, likelihood_char(f),        "Fixed-length string");
    add("VARCHAR(N)", 17, likelihood_varchar(f),     "Variable-length string");
    add("TEXT",       22, likelihood_text(f),          "Long text");
    add("BLOB",       23, likelihood_blob(f),          "Binary large object");
    add("DATETIME",   19, likelihood_datetime(f),    "Date+time");
    add("DATE",       20, likelihood_date(f),        "Date only");
    add("TIME",       21, likelihood_time(f),        "Time only");
    add("TIMESTAMP",  18, likelihood_timestamp(f),   "Unix epoch timestamp");
    add("YEAR",       2,  likelihood_year(f),        "Year value");
    add("ENUM",       28, likelihood_enum(f),        "Enumeration");
    add("SET",        29, likelihood_set(f),         "Set type");
    add("JSON",       31, likelihood_json(f),        "JSON document");
    add("BIT",        15, likelihood_bit(f),         "Bit field");

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

    return candidates;
}

// Type likelihood functions
double StatisticalTypeClassifier::likelihood_tinyint(const ColumnFeatures& f) {
    if (f.len_min > 4 || f.len_max > 4) return 0.0;
    double score = 0.5;
    if (f.is_fixed_length && f.fixed_size <= 1) score += 0.3;
    if (f.digit_ratio > 0.8) score += 0.2;
    if (f.entropy < 3.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_smallint(const ColumnFeatures& f) {
    if (f.len_min > 6 || f.len_max > 6) return 0.0;
    double score = 0.4;
    if (f.is_fixed_length && f.fixed_size <= 2) score += 0.3;
    if (f.digit_ratio > 0.8) score += 0.2;
    if (f.entropy < 4.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_int(const ColumnFeatures& f) {
    if (f.len_min > 11 || f.len_max > 11) return 0.0;
    double score = 0.4;
    if (f.is_fixed_length && f.fixed_size <= 4) score += 0.3;
    if (f.digit_ratio > 0.8) score += 0.2;
    if (f.entropy < 5.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_bigint(const ColumnFeatures& f) {
    if (f.len_min > 20 || f.len_max > 20) return 0.0;
    double score = 0.3;
    if (f.is_fixed_length && f.fixed_size <= 8) score += 0.3;
    if (f.digit_ratio > 0.8) score += 0.2;
    if (f.entropy < 6.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_float(const ColumnFeatures& f) {
    double score = 0.3;
    if (f.is_fixed_length && f.fixed_size <= 4) score += 0.2;
    if (f.digit_ratio > 0.5 && f.entropy < 6.0) score += 0.2;
    if (f.len_mean > 0 && f.len_mean <= 12) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_double(const ColumnFeatures& f) {
    double score = 0.2;
    if (f.is_fixed_length && f.fixed_size <= 8) score += 0.3;
    if (f.digit_ratio > 0.5) score += 0.2;
    if (f.len_mean > 0 && f.len_mean <= 22) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_decimal(const ColumnFeatures& f) {
    if (f.len_mean > 20) return 0.0;
    double score = 0.3;
    if (f.digit_ratio > 0.7) score += 0.2;
    if (f.len_stddev < 1.0) score += 0.2; // fixed precision
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_char(const ColumnFeatures& f) {
    if (!f.is_fixed_length || f.fixed_size == 0 || f.fixed_size > 255) return 0.0;
    double score = 0.5;
    if (f.ascii_ratio > 0.8) score += 0.3;
    if (f.entropy > 2.0 && f.entropy < 6.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_varchar(const ColumnFeatures& f) {
    if (f.is_fixed_length) return 0.1;
    if (f.ascii_ratio < 0.3) return 0.1;
    double score = 0.4;
    if (f.ascii_ratio > 0.7) score += 0.2;
    if (f.len_stddev > 1.0) score += 0.2;
    if (f.len_max <= 255) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_text(const ColumnFeatures& f) {
    if (f.len_max <= 255) return 0.1;
    double score = 0.3;
    if (f.ascii_ratio > 0.6) score += 0.2;
    if (f.len_stddev > 10.0) score += 0.2;
    if (f.len_max > 1000) score += 0.2;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_blob(const ColumnFeatures& f) {
    if (f.ascii_ratio > 0.6) return 0.1; // too ascii for blob
    double score = 0.3;
    if (f.entropy > 4.0) score += 0.3;
    if (f.len_stddev > 5.0) score += 0.2;
    if (f.len_max > 100) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_datetime(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.7) return 0.0;
    if (f.len_mean >= 18 && f.len_mean <= 20) {
        double score = 0.7;
        if (f.len_stddev < 0.5) score += 0.2;
        return std::min(score, 1.0);
    }
    return 0.2;
}

double StatisticalTypeClassifier::likelihood_date(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.7) return 0.0;
    if (f.len_mean >= 9 && f.len_mean <= 11) {
        double score = 0.6;
        if (f.len_stddev < 0.5) score += 0.2;
        return std::min(score, 1.0);
    }
    return 0.1;
}

double StatisticalTypeClassifier::likelihood_time(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.6) return 0.0;
    double score = 0.3;
    if (f.len_mean >= 8 && f.len_mean <= 15) score += 0.3;
    if (f.len_stddev < 1.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_timestamp(const ColumnFeatures& f) {
    // Unix timestamps are 10-digit integers
    if (f.digit_ratio < 0.8) return 0.0;
    double score = 0.3;
    if (f.len_min == 10 && f.len_max == 10) score += 0.4;
    if (f.len_min >= 10 && f.len_max <= 13) score += 0.1;
    if (f.entropy < 5.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_year(const ColumnFeatures& f) {
    if (f.digit_ratio < 0.8) return 0.0;
    double score = 0.3;
    if (f.len_min == 4 && f.len_max == 4) score += 0.4;
    if (f.entropy < 3.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_enum(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.7) return 0.0;
    double score = 0.3;
    if (f.len_stddev < 2.0) score += 0.2;
    if (f.len_max < 50) score += 0.2;
    if (f.entropy < 5.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_set(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.6) return 0.0;
    double score = 0.2;
    if (f.len_stddev < 3.0) score += 0.2;
    if (f.len_max < 100) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_json(const ColumnFeatures& f) {
    if (f.ascii_ratio < 0.5) return 0.0;
    double score = 0.2;
    if (f.len_stddev > 5.0) score += 0.2;
    if (f.len_max > 100) score += 0.2;
    if (f.entropy > 4.0) score += 0.1;
    return std::min(score, 1.0);
}

double StatisticalTypeClassifier::likelihood_bit(const ColumnFeatures& f) {
    if (f.len_max > 8) return 0.1;
    if (f.ascii_ratio > 0.5) return 0.1;
    double score = 0.3;
    if (f.len_stddev < 0.3) score += 0.3;
    if (f.entropy < 2.0) score += 0.1;
    return std::min(score, 1.0);
}

// ==================== FeatureExtractor ====================

ColumnFeatures FeatureExtractor::extract(
    const std::vector<std::vector<uint8_t>>& sample_values) {

    ColumnFeatures f;
    f.sample_count = static_cast<uint32_t>(sample_values.size());
    if (f.sample_count == 0) return f;

    std::vector<double> lengths;
    uint32_t null_count = 0, zero_count = 0, empty_count = 0;
    uint32_t ascii_count = 0, digit_count = 0;
    std::vector<uint8_t> byte_hist(256, 0);
    std::set<std::vector<uint8_t>> distinct_values;

    for (const auto& bytes : sample_values) {
        if (bytes.empty()) {
            null_count++;
            zero_count++;
            empty_count++;
            lengths.push_back(0.0);
            continue;
        }

        distinct_values.insert(bytes);
        lengths.push_back(static_cast<double>(bytes.size()));

        bool all_zero = true;
        bool has_digit = false;
        bool all_ascii = true;

        for (uint8_t b : bytes) {
            byte_hist[b]++;
            if (b != 0) all_zero = false;
            if (b >= 0x20 && b <= 0x7E) {
                // ASCII printable
                if (b >= '0' && b <= '9') has_digit = true;
            } else {
                all_ascii = false;
            }
        }

        if (all_zero) zero_count++;
        if (all_ascii) ascii_count++;
        if (has_digit) digit_count++;
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
        double sq_sum = 0;
        for (auto l : lengths) sq_sum += (l - f.len_mean) * (l - f.len_mean);
        f.len_stddev = std::sqrt(sq_sum / lengths.size());

        // Build length distribution (buckets of 1)
        std::map<uint32_t, uint32_t> dist;
        for (auto l : lengths) dist[static_cast<uint32_t>(l)]++;
        f.len_distribution.clear();
        for (const auto& [len, count] : dist) {
            f.len_distribution.push_back(len);
        }
    }

    f.ascii_ratio = static_cast<double>(ascii_count) / n;
    f.digit_ratio = static_cast<double>(digit_count) / n;

    // Compute entropy (per-byte, normalized by total bytes)
    double entropy = 0.0;
    size_t total_bytes = 0;
    for (const auto& bytes : sample_values) {
        total_bytes += bytes.size();
    }
    if (total_bytes > 0) {
        for (size_t i = 0; i < 256; i++) {
            if (byte_hist[i] > 0) {
                double p = static_cast<double>(byte_hist[i]) / total_bytes;
                entropy -= p * std::log2(p);
            }
        }
    }
    f.entropy = entropy;

    // Fixed-length detection
    if (f.len_stddev < 0.5 && f.len_min > 0 && f.len_max == f.len_min) {
        f.is_fixed_length = true;
        f.fixed_size = static_cast<uint32_t>(f.len_min);
    }

    return f;
}

std::vector<ColumnFeatures> FeatureExtractor::extract_all(
    const std::vector<std::vector<std::vector<uint8_t>>>& columns_samples) {

    std::vector<ColumnFeatures> results;
    results.reserve(columns_samples.size());

    for (const auto& samples : columns_samples) {
        results.push_back(extract(samples));
    }

    return results;
}

} // namespace inference
} // namespace dbrecovery