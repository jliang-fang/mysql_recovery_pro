/**
 * @file ddl_change_detector.cpp
 * @brief DDLChangeDetector implementation
 *
 * Detects column-count variations across InnoDB INDEX pages to identify
 * schema version boundaries. Clusters pages by column count, determines
 * version ordering, and computes confidence for each detected version.
 */

#include "dbrecovery/inference/ddl_change_detector.hpp"
#include "dbrecovery/inference/physical_layout_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace dbrecovery {
namespace inference {

using dbrecovery::innodb::read_be;

DDLChangeChainResult DDLChangeDetector::detect_versions_from_pages(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages) {

    DDLChangeChainResult result;

    if (pages.empty()) {
        result.summary = "No pages to scan";
        return result;
    }

    // First pass: detect column count per page via PhysicalLayoutAnalyzer
    std::map<uint32_t, uint32_t> col_count_votes;
    std::map<uint32_t, std::vector<uint32_t>> col_count_pages;
    uint32_t index_page_count = 0;

    for (size_t i = 0; i < pages.size(); i++) {
        const auto& [data, size] = pages[i];
        if (size < 120) continue;

        uint16_t page_type = read_be<uint16_t>(data, 24);
        if (page_type != dbrecovery::innodb::FIL_PAGE_INDEX) continue;

        auto layout = PhysicalLayoutAnalyzer::analyze_page(data, size);
        if (!layout) continue;

        index_page_count++;
        uint32_t n_cols = layout->total_columns;
        col_count_votes[n_cols]++;
        col_count_pages[n_cols].push_back(static_cast<uint32_t>(i));
    }

    if (col_count_votes.empty()) {
        result.summary = "No INDEX pages with detectable layout found";
        return result;
    }

    // Find the most common column count (consensus)
    uint32_t max_votes = 0;
    uint32_t consensus_cols = 0;
    for (const auto& [n_cols, votes] : col_count_votes) {
        if (votes > max_votes) {
            max_votes = votes;
            consensus_cols = n_cols;
        }
    }

    // Build SchemaVersion entries sorted by column count (ascending = older first)
    std::vector<SchemaVersion> versions;
    for (const auto& [n_cols, votes] : col_count_votes) {
        SchemaVersion ver;
        ver.version_id = 0; // Will be assigned after sorting
        ver.column_count = n_cols;
        ver.page_count = votes;
        ver.page_numbers = col_count_pages[n_cols];
        ver.confidence = static_cast<double>(votes) / index_page_count;
        versions.push_back(ver);
    }

    std::sort(versions.begin(), versions.end(),
        [](const SchemaVersion& a, const SchemaVersion& b) {
            return a.column_count < b.column_count;
        });

    // Assign version_id: 1 = oldest (fewest columns), 2, 3, ...
    for (size_t v = 0; v < versions.size(); v++) {
        versions[v].version_id = static_cast<uint32_t>(v + 1);
    }

    // Detect columns added/dropped between consecutive versions
    for (size_t v = 1; v < versions.size(); v++) {
        const auto& prev = versions[v - 1];
        const auto& curr = versions[v];

        if (curr.column_count > prev.column_count) {
            // Columns were added
            uint32_t added = curr.column_count - prev.column_count;
            for (uint32_t c = 0; c < added; c++) {
                ColumnDelta delta;
                delta.ordinal_position = prev.column_count + c;
                delta.column_name = "col_" + std::to_string(prev.column_count + c);
                delta.type_name = "VARCHAR(255)";
                delta.confidence = 0.5; // moderate - no type info without sampling
                // Make non-const ref
                auto& versions_nonconst = const_cast<std::vector<SchemaVersion>&>(versions);
                versions_nonconst[v].columns_added.push_back(delta);
            }
        } else if (curr.column_count < prev.column_count) {
            uint32_t dropped = prev.column_count - curr.column_count;
            for (uint32_t c = 0; c < dropped; c++) {
                ColumnDelta delta;
                delta.ordinal_position = prev.column_count - 1 - c;
                delta.column_name = "col_" + std::to_string(prev.column_count - 1 - c);
                delta.type_name = "VARCHAR(255)";
                delta.confidence = 0.5;
                auto& versions_nonconst = const_cast<std::vector<SchemaVersion>&>(versions);
                versions_nonconst[v].columns_dropped.push_back(delta);
            }
        }
    }

    result.versions = versions;
    result.has_ddl_change = versions.size() > 1;
    result.blocked_auto_decode = versions.size() > 1;

    // Generate summary
    std::ostringstream summary;
    if (versions.size() == 1) {
        summary << "Single schema version: " << versions[0].column_count
                << " columns (" << versions[0].page_count << " pages, "
                << std::fixed << std::setprecision(1)
                << (versions[0].confidence * 100.0) << "% confidence)";
    } else {
        summary << "Detected " << versions.size()
                << " distinct column-count groups (schema evolution). ";
        for (size_t v = 0; v < versions.size(); v++) {
            if (v > 0) summary << "; ";
            summary << "v" << versions[v].version_id << ": "
                    << versions[v].column_count << " cols ("
                    << versions[v].page_count << " pages, "
                    << std::fixed << std::setprecision(1)
                    << (versions[v].confidence * 100.0) << "%)";
        }
        summary << ". Auto-decode blocked: pages use different schemas.";
    }
    result.summary = summary.str();

    return result;
}

DDLChangeChainResult DDLChangeDetector::detect_versions_from_buffers(
    const std::vector<std::vector<uint8_t>>& page_buffers) {

    std::vector<std::pair<const uint8_t*, size_t>> pages;
    for (const auto& buf : page_buffers) {
        pages.emplace_back(buf.data(), buf.size());
    }
    return detect_versions_from_pages(pages);
}

bool DDLChangeDetector::apply_to_schema_template(
    const DDLChangeChainResult& result,
    engine::SchemaTemplate& tmpl) const {

    if (result.versions.empty() || tmpl.columns.empty()) {
        return false;
    }

    // The latest version should have the most columns - use it as the base
    const auto& latest = result.versions.back();
    uint32_t max_cols = latest.column_count;

    if (tmpl.columns.size() < max_cols) {
        // Pad template columns if needed
        tmpl.columns.resize(max_cols);
    }

    // For each version, mark columns as added or dropped
    for (const auto& ver : result.versions) {
        uint16_t vid = static_cast<uint16_t>(ver.version_id);

        for (const auto& added : ver.columns_added) {
            if (added.ordinal_position < tmpl.columns.size()) {
                tmpl.columns[added.ordinal_position].version_added = vid;
            }
        }
        for (const auto& dropped : ver.columns_dropped) {
            if (dropped.ordinal_position < tmpl.columns.size()) {
                tmpl.columns[dropped.ordinal_position].version_dropped = vid;
            }
        }
    }

    // Update max_row_version
    tmpl.max_row_version = static_cast<uint16_t>(result.versions.size());

    return true;
}

} // namespace inference
} // namespace dbrecovery