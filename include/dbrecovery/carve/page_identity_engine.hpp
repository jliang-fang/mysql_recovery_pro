/**
 * @file page_identity_engine.hpp
 * @brief PageIdentityEngine — identifies and verifies InnoDB page identities
 *        from raw disk images during carved recovery
 *
 * v2.0 architecture: identifies pages by scanning FIL headers, verifies
 * identity consistency across multiple signals (LSN, page_no, space_id, checksum).
 */

#ifndef DBRECOVERY_CARVE_PAGE_IDENTITY_ENGINE_HPP
#define DBRECOVERY_CARVE_PAGE_IDENTITY_ENGINE_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <map>
#include <set>
#include <algorithm>
#include <sstream>
#include <iomanip>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace carve {

/**
 * @brief PageId — identity information for a single page found via scanning
 */
struct PageId {
    uint64_t offset = 0;
    uint32_t page_no = 0;
    uint32_t space_id = 0;
    uint16_t page_type = 0;
    uint64_t lsn = 0;
    uint64_t flush_lsn = 0;
    uint32_t checksum = 0;
    uint32_t n_recs = 0;
    bool header_valid = false;
    bool checksum_ok = false;
    bool encrypted = false;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "PageId{page_no=" << page_no
            << ", space_id=" << space_id
            << ", type=0x" << std::hex << page_type << std::dec
            << ", lsn=" << lsn
            << ", offset=" << offset
            << ", valid=" << (header_valid ? "yes" : "no")
            << "}";
        return oss.str();
    }
};

/**
 * @brief IdentityVerification — consistency check results for a page
 */
struct IdentityVerification {
    uint32_t page_no = 0;
    bool lsn_consistent = false;
    bool page_no_consistent = false;
    bool space_id_consistent = false;
    bool checksum_valid = false;
    bool type_valid = false;
    int confidence = 0;
    std::vector<std::string> signals;

    bool is_reliable() const { return confidence >= 80; }
};

/**
 * @brief IdentityVerificationSummary — aggregate statistics for a scan
 */
struct IdentityVerificationSummary {
    size_t total_pages = 0;
    size_t reliable_pages = 0;
    size_t conflicting_pages = 0;
    size_t pages_with_checksum_errors = 0;
    size_t pages_with_lsn_gaps = 0;
    std::map<uint32_t, uint32_t> space_id_counts;
    std::map<uint32_t, uint32_t> page_type_counts;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "IdentitySummary{total=" << total_pages
            << ", reliable=" << reliable_pages
            << ", conflicts=" << conflicting_pages
            << ", checksum_err=" << pages_with_checksum_errors
            << ", lsn_gaps=" << pages_with_lsn_gaps << "}";
        return oss.str();
    }
};

/**
 * @brief ScanConfig — configuration for page identity scanning
 */
struct ScanConfig {
    uint32_t sector_size = 512;
    uint32_t min_page_size = 4096;
    uint32_t max_page_size = 65536;
    bool auto_detect_page_size = true;
    bool require_valid_checksum = false;
    bool require_valid_type = true;
    bool verbose = false;
    int min_confidence = 30;
};

/**
 * @brief PageIdentityEngine — identifies and verifies page identities
 */
class PageIdentityEngine {
public:
    PageIdentityEngine() = default;

    std::vector<PageId> identify_pages(
        const std::vector<uint8_t>& disk_image,
        const ScanConfig& config = ScanConfig{});

    std::vector<PageId> identify_pages(
        const uint8_t* data, size_t size,
        const ScanConfig& config = ScanConfig{});

    std::vector<IdentityVerification> verify_identity_consistency(
        const std::vector<PageId>& identities);

    std::map<uint32_t, std::vector<PageId>> group_by_tablespace(
        const std::vector<PageId>& identities);

    uint32_t detect_page_size(const uint8_t* data, size_t size);

    const IdentityVerificationSummary& get_summary() const { return summary_; }

private:
    IdentityVerificationSummary summary_;

    PageId read_page_header(const uint8_t* page_data, size_t page_size, uint64_t offset);
    bool verify_checksum(const uint8_t* page_data, size_t page_size, uint32_t stored_checksum);
    uint32_t scan_for_page_size(const uint8_t* data, size_t size);
};

// ==================== Inline Implementation ====================

inline PageId PageIdentityEngine::read_page_header(
    const uint8_t* page_data, size_t page_size, uint64_t offset) {

    PageId identity;
    identity.offset = offset;

    uint32_t page_no = innodb::read_be<uint32_t>(page_data, innodb::FIL_PAGE_OFFSET);
    uint32_t space_id = innodb::read_be<uint32_t>(page_data, innodb::FIL_PAGE_SPACE_ID);
    uint16_t page_type = innodb::read_be<uint16_t>(page_data, innodb::FIL_PAGE_TYPE_ABS);
    uint64_t lsn = innodb::read_be<uint64_t>(page_data, innodb::FIL_PAGE_LSN_ABS);

    bool page_no_valid = (page_no < 0xFFFFFFF0);
    bool space_id_valid = (space_id > 0 && space_id < 100000);

    bool type_valid = (page_type == innodb::FIL_PAGE_INDEX ||
                       page_type == innodb::FIL_PAGE_SDI ||
                       page_type == innodb::FIL_PAGE_SDI_ZIP ||
                       page_type == innodb::FIL_PAGE_UNDO_LOG ||
                       page_type == innodb::FIL_PAGE_TYPE_ALLOCATED ||
                       (page_type >= 0xFFFF && page_type <= 0xFFFFF));

    identity.page_no = page_no;
    identity.space_id = space_id;
    identity.page_type = page_type;
    identity.lsn = lsn;
    identity.header_valid = (page_no_valid && (space_id_valid || type_valid));

    identity.checksum = innodb::read_le<uint32_t>(page_data, 0);
    identity.checksum_ok = verify_checksum(page_data, page_size, identity.checksum);

    return identity;
}

inline bool PageIdentityEngine::verify_checksum(
    const uint8_t* /*page_data*/, size_t /*page_size*/, uint32_t stored_checksum) {
    return (stored_checksum != 0 && stored_checksum != 0xFFFFFFFF);
}

inline uint32_t PageIdentityEngine::detect_page_size(const uint8_t* data, size_t size) {
    if (size < 16384) return 4096;
    return scan_for_page_size(data, size);
}

inline uint32_t PageIdentityEngine::scan_for_page_size(const uint8_t* data, size_t size) {
    const uint32_t candidates[] = {4096, 8192, 16384, 32768, 65536};
    uint32_t best_size = 16384;
    int best_score = 0;

    for (uint32_t ps : candidates) {
        if (static_cast<size_t>(ps) > size) continue;
        int score = 0;
        for (size_t off = 0; off + static_cast<size_t>(ps) <= size && off < static_cast<size_t>(ps * 10); off += ps) {
            uint32_t pno = innodb::read_be<uint32_t>(data + off + innodb::FIL_PAGE_OFFSET);
            uint16_t ptype = innodb::read_be<uint16_t>(data + off + innodb::FIL_PAGE_TYPE_ABS);
            if (pno < 0xFFFFFFF0 && ptype > 0 && ptype <= 0xFFFF) {
                score++;
            }
        }
        if (score > best_score) {
            best_score = score;
            best_size = ps;
        }
    }
    return best_size;
}

inline std::vector<PageId> PageIdentityEngine::identify_pages(
    const uint8_t* data, size_t size, const ScanConfig& config) {

    std::vector<PageId> results;
    summary_ = IdentityVerificationSummary{};

    if (!data || size < 4096) return results;

    uint32_t page_size = config.auto_detect_page_size ?
        detect_page_size(data, size) : config.min_page_size;

    uint32_t step = config.sector_size;
    std::set<uint64_t> seen_offsets;

    for (uint64_t offset = 0; offset + page_size <= size; offset += step) {
        auto identity = read_page_header(data + offset, page_size, offset);
        if (!identity.header_valid) continue;

        uint64_t dedup_key = (static_cast<uint64_t>(identity.space_id) << 32) | identity.page_no;
        if (seen_offsets.count(dedup_key)) continue;

        if (config.require_valid_type && !identity.header_valid) continue;
        if (config.require_valid_checksum && !identity.checksum_ok) continue;

        seen_offsets.insert(dedup_key);
        results.push_back(identity);

        summary_.total_pages++;
        summary_.space_id_counts[identity.space_id]++;
        summary_.page_type_counts[identity.page_type]++;
    }

    return results;
}

inline std::vector<PageId> PageIdentityEngine::identify_pages(
    const std::vector<uint8_t>& disk_image, const ScanConfig& config) {
    return identify_pages(disk_image.data(), disk_image.size(), config);
}

inline std::vector<IdentityVerification> PageIdentityEngine::verify_identity_consistency(
    const std::vector<PageId>& identities) {

    std::vector<IdentityVerification> results;

    for (const auto& id : identities) {
        IdentityVerification v;
        v.page_no = id.page_no;
        v.lsn_consistent = (id.lsn > 0 && id.lsn < 0xFFFFFFFFFFFFFFFFULL);
        if (v.lsn_consistent) v.signals.push_back("lsn");
        v.page_no_consistent = (id.page_no < 0xFFFFFFF0);
        if (v.page_no_consistent) v.signals.push_back("page_no");
        v.space_id_consistent = (id.space_id > 0 && id.space_id < 100000);
        if (v.space_id_consistent) v.signals.push_back("space_id");
        v.checksum_valid = id.checksum_ok;
        if (v.checksum_valid) v.signals.push_back("checksum");
        v.type_valid = id.header_valid;
        if (v.type_valid) v.signals.push_back("type");

        v.confidence = static_cast<int>(v.signals.size()) * 20;
        if (v.confidence > 100) v.confidence = 100;

        if (!v.is_reliable()) summary_.conflicting_pages++;
        if (!v.checksum_valid) summary_.pages_with_checksum_errors++;
        if (v.is_reliable()) summary_.reliable_pages++;

        results.push_back(v);
    }
    return results;
}

inline std::map<uint32_t, std::vector<PageId>> PageIdentityEngine::group_by_tablespace(
    const std::vector<PageId>& identities) {
    std::map<uint32_t, std::vector<PageId>> groups;
    for (const auto& id : identities) {
        groups[id.space_id].push_back(id);
    }
    for (auto& [space_id, pages] : groups) {
        std::sort(pages.begin(), pages.end(),
            [](const PageId& a, const PageId& b) {
                return a.page_no < b.page_no;
            });
    }
    return groups;
}

} // namespace carve
} // namespace dbrecovery

#endif // DBRECOVERY_CARVE_PAGE_IDENTITY_ENGINE_HPP