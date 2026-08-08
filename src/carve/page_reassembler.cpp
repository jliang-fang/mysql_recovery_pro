/**
 * @file page_reassembler.cpp
 * @brief Page fragment reassembly implementation
 *
 * Implements PageFragmentReassembler (from page_reassembler.hpp) and
 * PageFragmentReassembler (from fragment_reassembler.hpp).
 *
 * Note: Both headers define dbrecovery::carve::PageFragmentReassembler with
 * different method sets. This .cpp provides implementations for both.
 * The fragment_reassembler.hpp version delegates to page_reassembler.hpp
 * internally.
 */

#include "dbrecovery/carve/page_reassembler.hpp"
#include "dbrecovery/carve/fragment_reassembler.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace dbrecovery {
namespace carve {

// ==================== PageFragmentReassembler (page_reassembler.hpp) ====================

PageIdentity PageFragmentReassembler::select_best_candidate(
    const std::vector<PageIdentity>& candidates) {

    if (candidates.size() == 1) return candidates[0];

    std::vector<PageIdentity> sorted = candidates;
    std::sort(sorted.begin(), sorted.end(),
        [](const PageIdentity& a, const PageIdentity& b) {
            if (a.checksum_valid != b.checksum_valid) return a.checksum_valid;
            if (a.lsn != b.lsn) return a.lsn > b.lsn;
            auto is_zero = [](const std::vector<uint8_t>& d) {
                for (size_t i = 0; i < std::min(d.size(), size_t(64)); i++) {
                    if (d[i] != 0) return false;
                }
                return true;
            };
            return !is_zero(a.data);
        });

    return sorted[0];
}

std::vector<uint8_t> PageFragmentReassembler::create_placeholder(
    uint32_t space_id, uint32_t page_no, uint32_t page_size) {

    std::vector<uint8_t> placeholder(page_size, 0);
    if (page_size < innodb::FIL_HEADER_SIZE) return placeholder;

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

std::vector<uint8_t> PageFragmentReassembler::synthesize_page0(
    uint32_t space_id, uint32_t page_size) {

    std::vector<uint8_t> page(page_size, 0);
    if (page_size < 256) return page;

    page[4] = 0; page[5] = 0; page[6] = 0; page[7] = 0;
    page[24] = 0x45; page[25] = 0xB8;

    page[34] = (space_id >> 24) & 0xFF;
    page[35] = (space_id >> 16) & 0xFF;
    page[36] = (space_id >> 8) & 0xFF;
    page[37] = space_id & 0xFF;

    // FSP header at offset 38: space_id (4B), then space_flags (4B at abs 42)
    uint32_t space_flags = (5 << 1) | (1 << 8) | (1 << 15);
    page[42] = space_flags & 0xFF;
    page[43] = (space_flags >> 8) & 0xFF;
    page[44] = (space_flags >> 16) & 0xFF;
    page[45] = (space_flags >> 24) & 0xFF;

    uint32_t free_limit = 64;
    page[54] = free_limit & 0xFF;
    page[55] = (free_limit >> 8) & 0xFF;
    page[56] = (free_limit >> 16) & 0xFF;
    page[57] = (free_limit >> 24) & 0xFF;

    page[58] = 1; page[59] = 0; page[60] = 0; page[61] = 0;
    page[62] = 0; page[63] = 0; page[64] = 0; page[65] = 0;
    page[102] = 1;

    for (uint32_t i = 0; i < 4; i++) {
        page[page_size - 8 + i] = 0xFF;
    }

    return page;
}

std::map<uint32_t, std::vector<PageIdentity>> PageFragmentReassembler::group_pages() {
    space_groups_.clear();

    for (const auto& page : pages_) {
        space_groups_[page.space_id].push_back(page);
    }

    for (auto& [space_id, group_pages] : space_groups_) {
        std::map<uint32_t, std::vector<PageIdentity>> by_page_no;
        for (auto& page : group_pages) {
            by_page_no[page.page_no].push_back(std::move(page));
        }

        std::vector<PageIdentity> deduped;
        for (auto& [pno, candidates] : by_page_no) {
            deduped.push_back(select_best_candidate(candidates));
        }

        std::sort(deduped.begin(), deduped.end(),
            [](const PageIdentity& a, const PageIdentity& b) {
                return a.page_no < b.page_no;
            });

        group_pages = std::move(deduped);
    }

    return space_groups_;
}

std::vector<PageGap> PageFragmentReassembler::analyze_gaps(
    uint32_t space_id, const std::vector<PageIdentity>& sorted_pages) {

    std::vector<PageGap> gaps;
    if (sorted_pages.empty()) return gaps;

    if (sorted_pages[0].page_no != 0) {
        PageGap gap;
        gap.space_id = space_id;
        gap.start_page_no = 0;
        gap.end_page_no = sorted_pages[0].page_no;
        gap.missing_count = sorted_pages[0].page_no;
        gap.severity = PageGap::CRITICAL;
        gap.description = "Missing Page 0 (FSP_HDR)";
        gaps.push_back(gap);
    }

    for (size_t i = 0; i + 1 < sorted_pages.size(); i++) {
        uint32_t expected_next = sorted_pages[i].page_no + 1;
        uint32_t actual_next = sorted_pages[i + 1].page_no;

        if (actual_next > expected_next) {
            PageGap gap;
            gap.space_id = space_id;
            gap.start_page_no = expected_next;
            gap.end_page_no = actual_next;
            gap.missing_count = actual_next - expected_next;

            if (expected_next <= 2 && actual_next > 2) {
                gap.severity = PageGap::HIGH;
                gap.description = "Missing metadata pages (FSP_HDR/IBUF_BITMAP/INODE)";
            } else if ((expected_next <= 4 && actual_next > 4) || expected_next == 3 || expected_next == 4) {
                gap.severity = PageGap::MEDIUM;
                gap.description = "Missing SDI metadata pages (MySQL 8.0+)";
            } else {
                gap.severity = PageGap::LOW;
                gap.description = "Missing data pages " +
                    std::to_string(expected_next) + "-" + std::to_string(actual_next - 1);
            }
            gaps.push_back(gap);
        }
    }

    return gaps;
}

ReassemblyStats PageFragmentReassembler::reassemble(const std::string& output_dir) {
    auto start_time = std::chrono::high_resolution_clock::now();
    stats_ = {};

    std::filesystem::create_directories(output_dir);

    auto groups = group_pages();
    stats_.total_pages_found = static_cast<uint32_t>(pages_.size());

    for (auto& [space_id, sorted_pages] : groups) {
        stats_.total_pages_used += static_cast<uint32_t>(sorted_pages.size());

        auto gaps = analyze_gaps(space_id, sorted_pages);
        all_gaps_.insert(all_gaps_.end(), gaps.begin(), gaps.end());

        stats_.total_gaps += static_cast<uint32_t>(gaps.size());
        for (const auto& g : gaps) {
            if (g.severity == PageGap::CRITICAL) stats_.critical_gaps++;
        }

        bool has_critical_gap = false;
        for (const auto& g : gaps) {
            if (g.severity == PageGap::CRITICAL && g.start_page_no == 0) {
                has_critical_gap = true;
                break;
            }
        }
        if (has_critical_gap) {
            uint32_t psize = 16384;
            for (const auto& p : sorted_pages) {
                if (p.page_size > 0) { psize = p.page_size; break; }
            }
            auto synthetic = synthesize_page0(space_id, psize);
            PageIdentity p0;
            p0.space_id = space_id;
            p0.page_no = 0;
            p0.page_type = innodb::FIL_PAGE_TYPE_FSP_HDR;
            p0.page_size = psize;
            p0.data = std::move(synthetic);
            p0.is_synthetic = true;
            sorted_pages.insert(sorted_pages.begin(), std::move(p0));
            stats_.synthetic_pages_written++;
        }

        uint32_t page_size = 16384;
        for (const auto& p : sorted_pages) {
            if (p.page_size > 0) { page_size = p.page_size; break; }
        }

        std::string filename = "space_" + std::to_string(space_id) + ".ibd";
        std::string output_path = output_dir + "/" + filename;

        if (write_composed_ibd(output_path, sorted_pages, gaps, page_size)) {
            stats_.spaces_reconstructed++;
            stats_.total_bytes_output += std::filesystem::file_size(output_path);
        }
    }

    uint32_t total_expected = stats_.total_pages_used;
    for (const auto& g : all_gaps_) {
        total_expected += g.missing_count;
    }
    if (total_expected > 0) {
        stats_.completeness_pct = 100.0 * stats_.total_pages_used / total_expected;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.time_seconds = std::chrono::duration<double>(end_time - start_time).count();

    return stats_;
}

bool PageFragmentReassembler::write_composed_ibd(
    const std::string& output_path,
    const std::vector<PageIdentity>& sorted_pages,
    const std::vector<PageGap>& gaps,
    uint32_t page_size) {

    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;

    uint32_t max_page_no = 0;
    for (const auto& p : sorted_pages) {
        max_page_no = std::max(max_page_no, p.page_no);
    }
    uint32_t total_pages = max_page_no + 1;

    size_t page_idx = 0;
    for (uint32_t pno = 0; pno < total_pages; pno++) {
        if (page_idx < sorted_pages.size() && sorted_pages[page_idx].page_no == pno) {
            const auto& page = sorted_pages[page_idx];
            if (page.data.size() >= page_size) {
                out.write(reinterpret_cast<const char*>(page.data.data()), page_size);
            } else {
                std::vector<uint8_t> padded = page.data;
                padded.resize(page_size, 0);
                out.write(reinterpret_cast<const char*>(padded.data()), page_size);
            }
            page_idx++;
        } else {
            auto placeholder = create_placeholder(
                sorted_pages.empty() ? 0 : sorted_pages[0].space_id,
                pno, page_size);
            out.write(reinterpret_cast<const char*>(placeholder.data()), page_size);
            stats_.synthetic_pages_written++;
        }
    }

    return out.good();
}

std::string PageFragmentReassembler::generate_report() const {
    std::ostringstream oss;
    oss << "============================================================\n";
    oss << "  Page Fragment Reassembly Report\n";
    oss << "============================================================\n\n";

    oss << "Summary:\n";
    oss << "  Total pages found:     " << stats_.total_pages_found << "\n";
    oss << "  Pages used (deduped):  " << stats_.total_pages_used << "\n";
    oss << "  Tablespaces rebuilt:   " << stats_.spaces_reconstructed << "\n";
    oss << "  Total gaps detected:   " << stats_.total_gaps << "\n";
    oss << "  Critical gaps:         " << stats_.critical_gaps << "\n";
    oss << "  Completeness:          " << std::fixed << std::setprecision(1)
        << stats_.completeness_pct << "%\n";
    oss << "  Total output:          " << stats_.total_bytes_output << " bytes\n";
    oss << "  Time:                  " << stats_.time_seconds << "s\n\n";

    if (!all_gaps_.empty()) {
        oss << "Gap Analysis:\n";
        oss << "  " << std::setw(10) << "Space" << std::setw(10) << "Start"
            << std::setw(10) << "End" << std::setw(10) << "Missing"
            << std::setw(12) << "Severity" << "  Description\n";
        oss << "  " << std::string(65, '-') << "\n";

        for (const auto& gap : all_gaps_) {
            oss << "  " << std::setw(10) << gap.space_id
                << std::setw(10) << gap.start_page_no
                << std::setw(10) << gap.end_page_no
                << std::setw(10) << gap.missing_count
                << std::setw(12) << gap.severity_name()
                << "  " << gap.description << "\n";
        }
        oss << "\n";
    }

    if (!space_groups_.empty()) {
        oss << "Recovered Spaces:\n";
        for (const auto& [space_id, pages] : space_groups_) {
            uint32_t data_pages = 0, index_pages = 0, sdi_pages = 0, other = 0;
            for (const auto& p : pages) {
                if (p.page_type == 17855) {
                    if (p.page_no == 0 || p.page_no == 1 || p.page_no == 2 || p.page_no == 3) {
                        other++;
                    } else {
                        data_pages++;
                    }
                } else if (p.page_type == 17853 || p.page_type == 17854) {
                    sdi_pages++;
                } else {
                    other++;
                }
            }
            oss << "  Space " << space_id << ": "
                << pages.size() << " pages ("
                << data_pages << " data, "
                << sdi_pages << " SDI, "
                << other << " other)\n";
        }
    }

    return oss.str();
}

// ==================== TablespaceComposer ====================

std::map<std::string, uint32_t> TablespaceComposer::compose(
    const std::vector<PageIdentity>& pages,
    const std::string& output_dir,
    const std::string& base_name) {

    reassembler_.clear();
    reassembler_.add_pages(pages);
    reassembler_.reassemble(output_dir);

    std::map<std::string, uint32_t> result;
    for (const auto& [space_id, group] : reassembler_.group_pages()) {
        std::string filename = "space_" + std::to_string(space_id) + ".ibd";
        result[output_dir + "/" + filename] = space_id;
    }

    return result;
}

} // namespace carve
} // namespace dbrecovery