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

std::optional<PhysicalLayoutAnalyzer::LayoutInfo> PhysicalLayoutAnalyzer::analyze_page(
    const uint8_t* page_data, size_t page_size) {

    if (!page_data || page_size < 100) return std::nullopt;

    // Check FIL header page type (offset 24, big-endian, 2 bytes)
    uint16_t page_type = (static_cast<uint16_t>(page_data[24]) << 8) | page_data[25];
    if (page_type != innodb::FIL_PAGE_INDEX) return std::nullopt;

    // Read PAGE_N_RECS from page header (offset 54, little-endian)
    uint16_t n_recs = (static_cast<uint16_t>(page_data[55]) << 8) | page_data[54];
    if (n_recs == 0 || n_recs > 1000) return std::nullopt;

    // Read PAGE_HEAP_TOP (offset 40, little-endian)
    uint16_t heap_top = (static_cast<uint16_t>(page_data[41]) << 8) | page_data[40];
    if (heap_top < 120 || heap_top > page_size - 8) return std::nullopt;

    // Read the page directory to find record offsets
    uint16_t dir_end = page_size - 2;
    uint16_t n_slots = (static_cast<uint16_t>(page_data[dir_end + 1]) << 8) | page_data[dir_end];
    if (n_slots == 0 || n_slots > 1000) return std::nullopt;

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
        uint16_t rec_off = (static_cast<uint16_t>(page_data[101]) << 8) | page_data[100];
        uint16_t max_recs = n_recs;
        for (uint16_t i = 0; i < max_recs && rec_off >= 120 && rec_off < heap_top; i++) {
            rec_offsets.push_back(rec_off);
            uint16_t next_rec = (static_cast<uint16_t>(page_data[rec_off + 1]) << 8) | page_data[rec_off];
            if (next_rec <= rec_off || next_rec >= heap_top) break;
            rec_off = next_rec;
        }
    }

    if (rec_offsets.empty()) return std::nullopt;

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

        std::vector<uint16_t> field_offsets;
        uint16_t pos = rec_off;

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

        uint32_t user_cols = col_count > 3 ? col_count - 3 : col_count;
        uint32_t null_bytes = (user_cols + 7) / 8;

        col_count_votes[col_count]++;
        layout_votes[col_count].total_columns = col_count;
        layout_votes[col_count].user_columns = user_cols;
        layout_votes[col_count].null_bitmap_bytes = null_bytes;
        layout_votes[col_count].vote_count++;
    }

    if (col_count_votes.empty()) return std::nullopt;

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
    LayoutInfo info;
    info.total_columns = best_layout.total_columns;
    info.user_columns = best_layout.user_columns;
    info.null_bitmap_bytes = best_layout.null_bitmap_bytes;
    info.is_compact_format = true;
    info.row_format = 0;

    // Now analyze fixed vs variable columns by examining actual field data
    std::vector<std::vector<uint32_t>> field_lengths(best_col_count);

    for (uint16_t rec_off : rec_offsets) {
        if (rec_off + 4 > heap_top) continue;

        std::vector<uint16_t> field_offsets;
        uint16_t pos = rec_off;
        for (uint32_t i = 0; i < best_col_count && pos + 2 <= heap_top; i++) {
            uint16_t val = (static_cast<uint16_t>(page_data[pos + 1]) << 8) | page_data[pos];
            if (val == 0 || (i > 0 && val >= field_offsets.back())) break;
            field_offsets.push_back(val);
            pos += 2;
        }

        if (field_offsets.size() != best_col_count) continue;

        for (uint32_t i = 0; i < best_col_count; i++) {
            uint16_t field_end_offset = (i + 1 < best_col_count) ?
                field_offsets[i + 1] : 0;
            uint32_t field_sz = field_offsets[i] - field_end_offset;
            field_lengths[i].push_back(field_sz);
        }
    }

    uint32_t fixed_total = 0;
    for (uint32_t i = 0; i < best_col_count && i < field_lengths.size(); i++) {
        const auto& lengths = field_lengths[i];
        if (lengths.empty()) continue;

        uint32_t min_len = *std::min_element(lengths.begin(), lengths.end());
        uint32_t max_len = *std::max_element(lengths.begin(), lengths.end());

        bool is_system_col = (i >= best_col_count - 3);

        if (is_system_col || min_len == max_len) {
            fixed_total += min_len;
        } else {
            info.var_len_offsets.push_back(static_cast<uint32_t>(i));
        }
    }

    info.fixed_size_total = fixed_total;

    return info;
}

PhysicalLayoutAnalyzer::LayoutInfo PhysicalLayoutAnalyzer::analyze_multiple_pages(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages) {

    std::map<uint32_t, LayoutInfo> layout_map;
    std::map<uint32_t, uint32_t> col_count_votes;

    for (const auto& [data, size] : pages) {
        auto opt_info = analyze_page(data, size);
        if (opt_info.has_value()) {
            auto info = opt_info.value();
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

    return best;
}

std::vector<std::vector<std::vector<uint8_t>>>
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

        uint16_t rec_off = (static_cast<uint16_t>(data[101]) << 8) | data[100];
        uint16_t heap_top = (static_cast<uint16_t>(data[41]) << 8) | data[40];
        uint16_t n_recs = (static_cast<uint16_t>(data[55]) << 8) | data[54];

        for (uint16_t rec_idx = 0; rec_idx < n_recs && sampled < max_samples; rec_idx++) {
            if (rec_off < 120 || rec_off >= heap_top) break;

            std::vector<uint16_t> field_offsets;
            uint16_t pos = rec_off;
            for (uint32_t i = 0; i < col_count && pos + 2 <= size; i++) {
                uint16_t val = (static_cast<uint16_t>(data[pos + 1]) << 8) | data[pos];
                if (val == 0 || val > size) break;
                field_offsets.push_back(val);
                pos += 2;
            }

            if (field_offsets.size() < col_count) break;

            uint16_t data_start = rec_off + col_count * 2 + 1 + layout.null_bitmap_bytes;

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