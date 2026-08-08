/**
 * @file tablespace_composer.cpp
 * @brief TablespaceComposerExtended implementation
 */

#include "dbrecovery/carve/tablespace_composer.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace dbrecovery {
namespace carve {

// ---- Input ----

void TablespaceComposerExtended::add_fragment(const PageFragment& fragment) {
    fragments_.push_back(fragment);
}

void TablespaceComposerExtended::add_fragments(const std::vector<PageFragment>& fragments) {
    fragments_.insert(fragments_.end(), fragments.begin(), fragments.end());
}

void TablespaceComposerExtended::add_identity(const PageIdentity& page) {
    pages_.push_back(page);
}

// ---- Grouping ----

std::map<uint32_t, std::vector<PageFragment>> TablespaceComposerExtended::group_fragments() const {
    std::map<uint32_t, std::vector<PageFragment>> groups;
    for (const auto& f : fragments_) {
        groups[f.space_id].push_back(f);
    }
    // Sort each group by page_no
    for (auto& [space_id, vec] : groups) {
        std::sort(vec.begin(), vec.end(),
            [](const PageFragment& a, const PageFragment& b) {
                return a.page_no < b.page_no;
            });
    }
    return groups;
}

// ---- Composition ----

std::map<std::string, uint32_t> TablespaceComposerExtended::compose_all(
        const std::string& output_dir,
        const std::string& base_name) {
    std::map<std::string, uint32_t> results;

    // Compose using V1 if we have PageIdentity objects
    if (!pages_.empty()) {
        auto v1_results = compose_v1(pages_, output_dir, base_name);
        results.insert(v1_results.begin(), v1_results.end());
    }

    // Compose using V2 for each space group in fragment pool
    auto groups = group_fragments();
    for (const auto& [space_id, fragments] : groups) {
        std::ostringstream path;
        path << output_dir << "/" << base_name << "_" << space_id << ".ibd";
        std::string output_path = path.str();

        bool ok = compose_v2(fragments, space_id, output_path);
        if (ok) {
            results[output_path] = space_id;
        }
    }

    spaces_composed_ = results.size();
    return results;
}

std::map<std::string, uint32_t> TablespaceComposerExtended::compose_v1(
        const std::vector<PageIdentity>& pages,
        const std::string& output_dir,
        const std::string& base_name) {
    // Delegate to the existing V1 TablespaceComposer
    v1_stats_ = ReassemblyStats{};
    auto result = v1_composer_.compose(pages, output_dir, base_name);
    v1_stats_ = v1_composer_.get_stats();
    if (!result.empty()) {
        spaces_composed_++;
    }
    return result;
}

bool TablespaceComposerExtended::compose_v2(
        const std::vector<PageFragment>& fragments,
        uint32_t space_id,
        const std::string& output_path) {
    ReassemblyReport report;
    auto composed = v2_reassembler_.reassemble(fragments, space_id, &report);
    if (composed.empty()) return false;

    // Write the composed data to the output path
    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(composed.data()), composed.size());
    return out.good();
}

// ---- Validation ----

TablespaceComposerExtended::ValidationResult
TablespaceComposerExtended::validate_space(const std::vector<PageIdentity>& pages) const {
    ValidationResult result;
    result.total_pages = static_cast<uint32_t>(pages.size());

    uint64_t prev_lsn = 0;
    bool have_prev = false;

    // Group by space_id and sort by page_no for ordered validation
    std::map<uint32_t, std::vector<PageIdentity>> by_space;
    for (const auto& p : pages) {
        by_space[p.space_id].push_back(p);
    }

    for (auto& [sid, vec] : by_space) {
        std::sort(vec.begin(), vec.end(),
            [](const PageIdentity& a, const PageIdentity& b) {
                return a.page_no < b.page_no;
            });

        uint16_t prev_type = 0;
        for (const auto& p : vec) {
            if (p.checksum_valid) result.pages_with_valid_checksum++;

            if (have_prev && p.lsn < prev_lsn) {
                result.lsn_out_of_order++;
            }
            prev_lsn = p.lsn;
            have_prev = true;

            if (prev_type != 0 && p.page_type != prev_type) {
                result.type_mismatches++;
            }
            prev_type = p.page_type;
        }
    }

    result.valid = (result.pages_with_valid_checksum > result.total_pages / 2);
    return result;
}

TablespaceComposerExtended::ValidationResult
TablespaceComposerExtended::validate_fragments(const std::vector<PageFragment>& fragments) const {
    ValidationResult result;
    result.total_pages = static_cast<uint32_t>(fragments.size());

    // Sort by space_id then page_no
    std::vector<PageFragment> sorted = fragments;
    std::sort(sorted.begin(), sorted.end(),
        [](const PageFragment& a, const PageFragment& b) {
            if (a.space_id != b.space_id) return a.space_id < b.space_id;
            return a.page_no < b.page_no;
        });

    uint64_t prev_lsn = 0;
    bool have_prev = false;
    uint32_t prev_space = UINT32_MAX;
    uint16_t prev_type = 0;

    for (const auto& f : sorted) {
        if (f.checksum_valid) result.pages_with_valid_checksum++;

        if (f.space_id != prev_space) {
            have_prev = false;
            prev_type = 0;
            prev_space = f.space_id;
        }

        if (have_prev && f.lsn < prev_lsn) {
            result.lsn_out_of_order++;
        }
        prev_lsn = f.lsn;
        have_prev = true;

        if (prev_type != 0 && f.page_type != prev_type) {
            result.type_mismatches++;
        }
        prev_type = f.page_type;
    }

    result.valid = (result.pages_with_valid_checksum > result.total_pages / 2);
    return result;
}

// ---- Gap analysis ----

std::vector<GapInfo> TablespaceComposerExtended::detect_gaps(
        const std::vector<PageFragment>& sorted_fragments) const {
    return v2_reassembler_.detect_gaps(sorted_fragments);
}

std::vector<PageGap> TablespaceComposerExtended::analyze_gaps(
        uint32_t space_id,
        const std::vector<PageIdentity>& sorted_pages) const {
    // PageFragmentReassembler has analyze_gaps — but we need to use our own copy
    // Build a temporary reassembler with these pages
    PageFragmentReassembler tmp;
    tmp.add_pages(sorted_pages);
    return tmp.analyze_gaps(space_id, sorted_pages);
}

// ---- Reports ----

std::vector<ReassemblyReport> TablespaceComposerExtended::generate_reports() const {
    std::vector<ReassemblyReport> reports;
    auto groups = group_fragments();
    for (const auto& [space_id, fragments] : groups) {
        auto gaps = v2_reassembler_.detect_gaps(fragments);
        auto report = v2_reassembler_.generate_report(space_id, fragments, gaps);
        reports.push_back(report);
    }
    return reports;
}

// ---- Clear ----

void TablespaceComposerExtended::clear() {
    pages_.clear();
    fragments_.clear();
    v1_stats_ = ReassemblyStats{};
    spaces_composed_ = 0;
}

} // namespace carve
} // namespace dbrecovery