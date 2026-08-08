#ifndef DBRECOVERY_CARVE_TABLESPACE_COMPOSER_HPP
#define DBRECOVERY_CARVE_TABLESPACE_COMPOSER_HPP

// TablespaceComposerExtended composes complete .ibd files from reassembled
// page fragments, handling gap filling, page reordering, and output
// validation for disk-carved recovery.
//
// NOTE: The base class `carve::TablespaceComposer` is declared in
// page_reassembler.hpp with compose(pages, output_dir, base_name) and
// get_stats().  This header provides an extended version that additionally
// wraps PageFragmentReassemblerV2 and adds gap-aware page reordering,
// page-level validation, and multi-space batch composition.
#include "dbrecovery/carve/fragment_reassembler.hpp"
#include "dbrecovery/carve/page_reassembler.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <filesystem>

namespace dbrecovery {
namespace carve {

/**
 * @brief Extended tablespace composer with V2 fragment reassembly support
 *
 * Builds on the existing TablespaceComposer (page_reassembler.hpp) by adding:
 * - V2 fragment reassembly (PageFragmentReassemblerV2) for gap-aware merging
 * - Page-level validation (checksum, LSN monotonicity, type consistency)
 * - Multi-space batch composition from a single fragment pool
 * - Gap analysis and reporting
 */
class TablespaceComposerExtended {
public:
    TablespaceComposerExtended() = default;
    ~TablespaceComposerExtended() = default;

    // ---- Input ----
    void add_fragment(const PageFragment& fragment);
    void add_fragments(const std::vector<PageFragment>& fragments);
    void add_identity(const PageIdentity& page);

    // ---- Composition ----
    /**
     * @brief Compose tablespace files from accumulated fragments/pages
     * @param output_dir Output directory for composed .ibd files
     * @param base_name  Base name prefix
     * @return Map of output file paths → space_id
     */
    std::map<std::string, uint32_t> compose_all(const std::string& output_dir,
                                                  const std::string& base_name = "recovered_space");

    /**
     * @brief Compose a single space using the V1 TablespaceComposer
     */
    std::map<std::string, uint32_t> compose_v1(const std::vector<PageIdentity>& pages,
                                                 const std::string& output_dir,
                                                 const std::string& base_name = "recovered_space");

    /**
     * @brief Compose a single space using V2 fragment reassembly
     */
    bool compose_v2(const std::vector<PageFragment>& fragments,
                    uint32_t space_id,
                    const std::string& output_path);

    // ---- Validation ----
    struct ValidationResult {
        bool valid = true;
        uint32_t total_pages = 0;
        uint32_t pages_with_valid_checksum = 0;
        uint32_t lsn_out_of_order = 0;
        uint32_t type_mismatches = 0;
        std::vector<std::string> warnings;
    };

    ValidationResult validate_space(const std::vector<PageIdentity>& pages) const;
    ValidationResult validate_fragments(const std::vector<PageFragment>& fragments) const;

    // ---- Gap analysis ----
    std::vector<GapInfo> detect_gaps(const std::vector<PageFragment>& sorted_fragments) const;
    std::vector<PageGap> analyze_gaps(uint32_t space_id,
                                       const std::vector<PageIdentity>& sorted_pages) const;

    // ---- Reports ----
    std::vector<ReassemblyReport> generate_reports() const;

    // ---- Stats ----
    const ReassemblyStats& stats_v1() const { return v1_stats_; }
    size_t spaces_composed() const { return spaces_composed_; }

    void clear();

private:
    // V1 page pool
    std::vector<PageIdentity> pages_;
    // V2 fragment pool
    std::vector<PageFragment> fragments_;

    // Delegated V1 composer
    TablespaceComposer v1_composer_;
    // mutable: detect_gaps/generate_report are const in the facade but
    // PageFragmentReassemblerV2 methods are non-const
    mutable PageFragmentReassemblerV2 v2_reassembler_;

    ReassemblyStats v1_stats_;
    size_t spaces_composed_ = 0;

    // Group fragments by space_id
    std::map<uint32_t, std::vector<PageFragment>> group_fragments() const;
};

} // namespace carve
} // namespace dbrecovery

#endif // DBRECOVERY_CARVE_TABLESPACE_COMPOSER_HPP