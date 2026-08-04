#ifndef DBRECOVERY_CARVE_FRAGMENT_REASSEMBLER_HPP
#define DBRECOVERY_CARVE_FRAGMENT_REASSEMBLER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <string>
#include <optional>
#include <algorithm>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

namespace dbrecovery {
namespace carve {

struct PageFragment {
    uint32_t space_id;
    uint32_t page_no;
    uint16_t page_type;
    uint64_t lsn;
    uint32_t checksum;
    bool checksum_valid;
    uint64_t disk_offset;
    std::vector<uint8_t> data;
};

struct GapInfo {
    uint32_t space_id;
    uint32_t page_no;
    bool is_critical;  // FSP_HDR (page 0), SDI (page 3,4), INODE (page 2)
};

struct ReassemblyReport {
    uint32_t space_id;
    size_t total_pages_found = 0;
    size_t total_gaps = 0;
    size_t critical_gaps = 0;
    size_t data_pages = 0;
    size_t index_pages = 0;
    double completeness = 0.0;
    uint64_t estimated_row_count = 0;
    std::vector<GapInfo> gaps;
};

class PageFragmentReassemblerV2 {
public:
    // Group fragments by SPACE_ID, sort by PAGE_NO, detect gaps
    std::map<uint32_t, std::vector<PageFragment>> group_by_space_id(
        const std::vector<PageFragment>& fragments);

    // Select best candidate for duplicate PAGE_NO (checksum + latest LSN)
    static PageFragment select_best_candidate(
        const std::vector<PageFragment>& candidates);

    // Detect gaps in a sorted fragment list
    std::vector<GapInfo> detect_gaps(
        const std::vector<PageFragment>& sorted_fragments);

    // Generate reassembly report
    ReassemblyReport generate_report(
        uint32_t space_id,
        const std::vector<PageFragment>& fragments,
        const std::vector<GapInfo>& gaps);

    // Write composed .ibd with placeholders for gaps
    bool write_composed_ibd(
        const std::string& output_path,
        const std::vector<PageFragment>& sorted_fragments,
        uint32_t total_pages,
        const std::vector<GapInfo>& gaps);

    // Full pipeline: reassemble fragments into logical tablespace
    std::vector<uint8_t> reassemble(
        const std::vector<PageFragment>& fragments,
        uint32_t space_id,
        ReassemblyReport* report = nullptr);

private:
    // Create a placeholder page (zero-filled with valid FIL header)
    std::vector<uint8_t> create_placeholder_page(
        uint32_t space_id, uint32_t page_no);

    // Analyze gap impact
    bool is_critical_gap(uint32_t page_no);
};

} // namespace carve
} // namespace dbrecovery

#endif