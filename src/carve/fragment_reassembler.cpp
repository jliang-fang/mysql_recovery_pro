/**
 * @file fragment_reassembler.cpp
 * @brief PageFragmentReassemblerV2 implementation
 *
 * Provides fragment grouping, gap detection, candidate selection, reassembly
 * report generation, and composed .ibd writing for carved page fragments.
 */

#include "dbrecovery/carve/fragment_reassembler.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dbrecovery {
namespace carve {

std::map<uint32_t, std::vector<PageFragment>>
PageFragmentReassemblerV2::group_by_space_id(
    const std::vector<PageFragment>& fragments) {

    std::map<uint32_t, std::vector<PageFragment>> groups;
    for (const auto& f : fragments) {
        groups[f.space_id].push_back(f);
    }

    for (auto& [sid, frags] : groups) {
        std::sort(frags.begin(), frags.end(),
            [](const PageFragment& a, const PageFragment& b) {
                return a.page_no < b.page_no;
            });
    }

    return groups;
}

PageFragment PageFragmentReassemblerV2::select_best_candidate(
    const std::vector<PageFragment>& candidates) {

    if (candidates.size() == 1) return candidates[0];

    std::vector<PageFragment> sorted = candidates;
    std::sort(sorted.begin(), sorted.end(),
        [](const PageFragment& a, const PageFragment& b) {
            if (a.checksum_valid != b.checksum_valid) return a.checksum_valid;
            if (a.lsn != b.lsn) return a.lsn > b.lsn;
            return false;
        });

    return sorted[0];
}

std::vector<GapInfo> PageFragmentReassemblerV2::detect_gaps(
    const std::vector<PageFragment>& sorted_fragments) {

    std::vector<GapInfo> gaps;
    if (sorted_fragments.empty()) return gaps;

    if (sorted_fragments[0].page_no != 0) {
        GapInfo gap;
        gap.space_id = sorted_fragments[0].space_id;
        gap.page_no = 0;
        gap.is_critical = true;
        gaps.push_back(gap);
    }

    for (size_t i = 0; i + 1 < sorted_fragments.size(); i++) {
        uint32_t expected = sorted_fragments[i].page_no + 1;
        uint32_t actual = sorted_fragments[i + 1].page_no;

        for (uint32_t missing = expected; missing < actual; missing++) {
            GapInfo gap;
            gap.space_id = sorted_fragments[i].space_id;
            gap.page_no = missing;
            gap.is_critical = is_critical_gap(missing);
            gaps.push_back(gap);
        }
    }

    return gaps;
}

ReassemblyReport PageFragmentReassemblerV2::generate_report(
    uint32_t space_id,
    const std::vector<PageFragment>& fragments,
    const std::vector<GapInfo>& gaps) {

    ReassemblyReport report;
    report.space_id = space_id;
    report.total_pages_found = fragments.size();
    report.total_gaps = gaps.size();

    for (const auto& g : gaps) {
        if (g.is_critical) report.critical_gaps++;
    }

    for (const auto& f : fragments) {
        if (f.page_type == 17855) report.index_pages++;
        else report.data_pages++;
    }

    uint32_t total_pages = fragments.empty() ? 0 : fragments.back().page_no + 1;
    if (total_pages > 0) {
        report.completeness = 100.0 * fragments.size() / (fragments.size() + gaps.size());
    }

    report.gaps = gaps;
    return report;
}

bool PageFragmentReassemblerV2::write_composed_ibd(
    const std::string& output_path,
    const std::vector<PageFragment>& sorted_fragments,
    uint32_t total_pages,
    const std::vector<GapInfo>& gaps) {

    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;

    size_t frag_idx = 0;
    for (uint32_t pno = 0; pno < total_pages; pno++) {
        if (frag_idx < sorted_fragments.size() && sorted_fragments[frag_idx].page_no == pno) {
            const auto& frag = sorted_fragments[frag_idx];
            if (!frag.data.empty()) {
                out.write(reinterpret_cast<const char*>(frag.data.data()), frag.data.size());
            }
            frag_idx++;
        } else {
            auto placeholder = create_placeholder_page(
                sorted_fragments.empty() ? 0 : sorted_fragments[0].space_id, pno);
            out.write(reinterpret_cast<const char*>(placeholder.data()), placeholder.size());
        }
    }

    return out.good();
}

std::vector<uint8_t> PageFragmentReassemblerV2::reassemble(
    const std::vector<PageFragment>& fragments,
    uint32_t space_id,
    ReassemblyReport* report) {

    auto groups = group_by_space_id(fragments);

    auto it = groups.find(space_id);
    if (it == groups.end()) return {};

    const auto& sorted = it->second;
    auto gaps = detect_gaps(sorted);

    if (report) {
        *report = generate_report(space_id, sorted, gaps);
    }

    uint32_t total_pages = sorted.empty() ? 0 : sorted.back().page_no + 1;

    std::vector<uint8_t> result;
    for (uint32_t pno = 0; pno < total_pages; pno++) {
        bool found = false;
        for (const auto& frag : sorted) {
            if (frag.page_no == pno) {
                result.insert(result.end(), frag.data.begin(), frag.data.end());
                found = true;
                break;
            }
        }
        if (!found) {
            auto placeholder = create_placeholder_page(space_id, pno);
            result.insert(result.end(), placeholder.begin(), placeholder.end());
        }
    }

    return result;
}

std::vector<uint8_t> PageFragmentReassemblerV2::create_placeholder_page(
    uint32_t space_id, uint32_t page_no) {

    std::vector<uint8_t> placeholder(16384, 0);

    placeholder[34] = (space_id >> 24) & 0xFF;
    placeholder[35] = (space_id >> 16) & 0xFF;
    placeholder[36] = (space_id >> 8) & 0xFF;
    placeholder[37] = space_id & 0xFF;

    placeholder[4] = (page_no >> 24) & 0xFF;
    placeholder[5] = (page_no >> 16) & 0xFF;
    placeholder[6] = (page_no >> 8) & 0xFF;
    placeholder[7] = page_no & 0xFF;

    placeholder[24] = 0;
    placeholder[25] = 0;

    return placeholder;
}

bool PageFragmentReassemblerV2::is_critical_gap(uint32_t page_no) {
    return page_no == 0 || page_no == 1 || page_no == 2 || page_no == 3;
}

} // namespace carve
} // namespace dbrecovery