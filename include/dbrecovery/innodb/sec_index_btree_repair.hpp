#ifndef DBRECOVERY_INNODB_SEC_INDEX_BTREE_REPAIR_HPP
#define DBRECOVERY_INNODB_SEC_INDEX_BTREE_REPAIR_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <algorithm>
#include <sstream>
#include <cstring>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

struct PageKeyRange {
    uint32_t page_no = 0;
    uint16_t level = 0;
    std::vector<uint8_t> min_key;
    std::vector<uint8_t> max_key;
    bool is_leaf = false;
};

struct SecondaryIndexBTreeRepairStats {
    uint64_t pages_analyzed = 0;
    uint64_t leaf_pages = 0;
    uint64_t internal_pages = 0;
    uint64_t pages_relinked = 0;
    uint64_t overlapping_ranges = 0;
    uint64_t chain_breaks_fixed = 0;
    double repair_time_ms = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "SecondaryIndexBTreeRepairStats{analyzed=" << pages_analyzed
            << ", leaf=" << leaf_pages
            << ", internal=" << internal_pages
            << ", relinked=" << pages_relinked
            << ", overlaps=" << overlapping_ranges
            << ", breaks_fixed=" << chain_breaks_fixed
            << ", time_ms=" << repair_time_ms << "}";
        return oss.str();
    }
};

/**
 * @brief SecondaryIndexBTreeRepairer — repair fragmented B+Tree page chains
 *
 * When a secondary index IBD has broken page chains (e.g., after TRUNCATE
 * or filesystem corruption), the current fix_chain_breaks() sorts pages by
 * page_no — wrong for fragmented B+Trees where adjacent pages in the tree
 * are not adjacent on disk. This class implements content-based ordering:
 * extract min/max key from each leaf page and sort by key range.
 */
class SecondaryIndexBTreeRepairer {
public:
    explicit SecondaryIndexBTreeRepairer(size_t page_size = 16384)
        : page_size_(page_size) {}

    void set_schema(const catalog::TableSchema& schema) { schema_ = schema; }
    void set_page_size(size_t page_size) { page_size_ = page_size; }

    void set_page_data(const std::map<uint32_t, std::vector<uint8_t>>& page_data) {
        page_data_map_ = page_data;
    }

    void set_page_list(const std::vector<uint32_t>& page_nos,
                       const std::map<uint32_t, std::vector<uint8_t>>& page_data) {
        page_nos_ = page_nos;
        page_data_map_ = page_data;
    }

    const SecondaryIndexBTreeRepairStats& get_stats() const { return stats_; }

    /**
     * @brief Extract key ranges from all pages in page_nos_
     */
    std::vector<PageKeyRange> extract_key_ranges() {
        std::vector<PageKeyRange> ranges;
        for (auto page_no : page_nos_) {
            auto it = page_data_map_.find(page_no);
            if (it == page_data_map_.end()) continue;
            auto range = extract_key_range_from_page(it->second);
            if (range) {
                range->page_no = page_no;
                ranges.push_back(std::move(*range));
            }
        }
        return ranges;
    }

    /**
     * @brief Extract key range from a single page
     */
    std::optional<PageKeyRange> extract_key_range_from_page(const std::vector<uint8_t>& page_data) {
        if (page_data.size() < page_size_) return std::nullopt;
        return extract_key_range_from_raw(page_data.data());
    }

    /**
     * @brief Extract key range from raw page data
     */
    std::optional<PageKeyRange> extract_key_range_from_raw(const uint8_t* page_data) {
        PageKeyRange range;

        uint16_t page_type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);
        if (page_type != FIL_PAGE_INDEX) return std::nullopt;

        stats_.pages_analyzed++;
        range.level = read_be<uint16_t>(page_data, BTR_N_LEVELS_ABS);
        range.is_leaf = (range.level == 0);

        if (range.is_leaf) {
            stats_.leaf_pages++;

            auto min_key = extract_min_key_bytes(page_data);
            auto max_key = extract_max_key_bytes(page_data);

            if (!min_key || !max_key) return std::nullopt;

            range.min_key = std::move(*min_key);
            range.max_key = std::move(*max_key);
        } else {
            stats_.internal_pages++;
        }

        return range;
    }

    /**
     * @brief Extract raw key bytes from the first user record on a leaf page
     */
    std::optional<std::vector<uint8_t>> extract_min_key_bytes(const uint8_t* page_data) {
        // Start at INFIMUM (absolute offset 54)
        uint16_t pos = PAGE_INFIMUM_REC_OFFSET_ABS;
        if (pos + 5 > page_size_) return std::nullopt;

        // Read INFIMUM's next_record_offset → first user record
        auto header = RecordHeader::parse(page_data + pos, 5);
        if (!header) return std::nullopt;

        int16_t next_off = header->get_next_record_offset();
        if (next_off == 0) return std::nullopt;

        int16_t next_pos = static_cast<int16_t>(pos) + next_off;
        if (next_pos < 0 || static_cast<size_t>(next_pos) + 5 > page_size_) return std::nullopt;

        pos = static_cast<uint16_t>(next_pos);

        // Read first user record
        auto rec_header = RecordHeader::parse(page_data + pos, 5);
        if (!rec_header) return std::nullopt;

        // Skip if SUPREMUM (no user records)
        if (rec_header->get_rec_type() == 3) return std::nullopt;

        // Extract raw bytes after record header
        size_t data_start = pos + 5;
        size_t max_bytes = page_size_ - data_start;
        if (max_bytes > 256) max_bytes = 256;
        if (max_bytes == 0) return std::nullopt;

        return std::vector<uint8_t>(page_data + data_start, page_data + data_start + max_bytes);
    }

    /**
     * @brief Extract raw key bytes from the last user record on a leaf page
     */
    std::optional<std::vector<uint8_t>> extract_max_key_bytes(const uint8_t* page_data) {
        uint16_t pos = PAGE_INFIMUM_REC_OFFSET_ABS;
        if (pos + 5 > page_size_) return std::nullopt;

        auto header = RecordHeader::parse(page_data + pos, 5);
        if (!header) return std::nullopt;

        int16_t next_off = header->get_next_record_offset();
        if (next_off == 0) return std::nullopt;

        int16_t next_pos = static_cast<int16_t>(pos) + next_off;

        uint16_t last_rec_pos = 0;
        bool found_any = false;

        // Traverse the record chain to find the last user record
        while (next_pos > 0 && static_cast<size_t>(next_pos) + 5 <= page_size_) {
            pos = static_cast<uint16_t>(next_pos);

            auto rec_header = RecordHeader::parse(page_data + pos, 5);
            if (!rec_header) break;

            // SUPREMUM record
            if (rec_header->get_rec_type() == 3) break;

            last_rec_pos = pos;
            found_any = true;

            next_off = rec_header->get_next_record_offset();
            if (next_off == 0) break;
            next_pos = static_cast<int16_t>(pos) + next_off;
        }

        if (!found_any) return std::nullopt;

        // Extract raw bytes after record header from last record
        size_t data_start = last_rec_pos + 5;
        size_t max_bytes = page_size_ - data_start;
        if (max_bytes > 256) max_bytes = 256;
        if (max_bytes == 0) return std::nullopt;

        return std::vector<uint8_t>(page_data + data_start, page_data + data_start + max_bytes);
    }

    /**
     * @brief Verify that key ranges are in correct content order
     * @return true if all leaf page key ranges are non-overlapping and sorted
     */
    bool verify_content_order(const std::vector<PageKeyRange>& key_ranges) {
        std::vector<PageKeyRange> leaf_ranges;
        for (const auto& kr : key_ranges) {
            if (kr.is_leaf) leaf_ranges.push_back(kr);
        }

        if (leaf_ranges.size() <= 1) return true;

        auto sorted = leaf_ranges;
        std::sort(sorted.begin(), sorted.end(),
            [](const PageKeyRange& a, const PageKeyRange& b) {
                return a.min_key < b.min_key;
            });

        // Check that max_key[i] <= min_key[i+1]
        for (size_t i = 0; i + 1 < sorted.size(); i++) {
            if (sorted[i].max_key > sorted[i + 1].min_key) {
                stats_.overlapping_ranges++;
                return false;
            }
        }

        return true;
    }

    /**
     * @brief Detect overlapping key ranges between pages
     * @return Vector of (page_a, page_b) pairs that overlap
     */
    std::vector<std::pair<uint32_t, uint32_t>> detect_overlapping_ranges(
        const std::vector<PageKeyRange>& key_ranges) {

        std::vector<std::pair<uint32_t, uint32_t>> overlaps;
        std::vector<PageKeyRange> leaf_ranges;
        for (const auto& kr : key_ranges) {
            if (kr.is_leaf) leaf_ranges.push_back(kr);
        }

        auto sorted = leaf_ranges;
        std::sort(sorted.begin(), sorted.end(),
            [](const PageKeyRange& a, const PageKeyRange& b) {
                return a.min_key < b.min_key;
            });

        for (size_t i = 0; i + 1 < sorted.size(); i++) {
            if (sorted[i].max_key > sorted[i + 1].min_key) {
                overlaps.emplace_back(sorted[i].page_no, sorted[i + 1].page_no);
            }
        }

        return overlaps;
    }

    /**
     * @brief Relink FIL_PAGE_PREV/FIL_PAGE_NEXT based on content-ordered page list
     * @param sorted_pages Vector of (page_no, page_data) pairs in content order
     * @return Number of pages relinked
     */
    size_t relink_pages_by_content(std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& sorted_pages) {
        if (sorted_pages.size() <= 1) return 0;

        size_t relinked = 0;

        for (size_t i = 0; i < sorted_pages.size(); i++) {
            auto& page_data = sorted_pages[i].second;
            if (page_data.size() < FIL_PAGE_NEXT_ABS + 4) continue;

            // Set FIL_PAGE_PREV
            if (i == 0) {
                write_be<uint32_t>(page_data.data(), FIL_PAGE_PREV_ABS, FIL_NULL);
            } else {
                write_be<uint32_t>(page_data.data(), FIL_PAGE_PREV_ABS, sorted_pages[i - 1].first);
            }

            // Set FIL_PAGE_NEXT
            if (i == sorted_pages.size() - 1) {
                write_be<uint32_t>(page_data.data(), FIL_PAGE_NEXT_ABS, FIL_NULL);
            } else {
                write_be<uint32_t>(page_data.data(), FIL_PAGE_NEXT_ABS, sorted_pages[i + 1].first);
            }

            relinked++;
            stats_.pages_relinked++;
        }

        stats_.chain_breaks_fixed = (relinked > 0) ? 1 : 0;
        return relinked;
    }

    /**
     * @brief Full repair: extract key ranges, sort by content, relink pages
     * @param page_nos List of page numbers belonging to the secondary index
     * @return true if at least one page was relinked
     */
    bool repair_chain_by_content(const std::vector<uint32_t>& page_nos) {
        if (page_nos.size() <= 1) return false;

        page_nos_ = page_nos;

        auto key_ranges = extract_key_ranges();
        if (key_ranges.empty()) return false;

        // Separate leaf and internal pages
        std::vector<PageKeyRange> leaf_ranges;
        std::vector<PageKeyRange> internal_ranges;

        for (auto& kr : key_ranges) {
            if (kr.is_leaf) leaf_ranges.push_back(std::move(kr));
            else internal_ranges.push_back(std::move(kr));
        }

        // Sort leaf pages by min_key (ascending)
        std::sort(leaf_ranges.begin(), leaf_ranges.end(),
            [](const PageKeyRange& a, const PageKeyRange& b) {
                return a.min_key < b.min_key;
            });

        // Sort internal pages by level (descending = root first), then by min_key
        std::sort(internal_ranges.begin(), internal_ranges.end(),
            [](const PageKeyRange& a, const PageKeyRange& b) {
                if (a.level != b.level) return a.level > b.level;
                return a.min_key < b.min_key;
            });

        // Build sorted page list: leaf pages first, then internal
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> sorted_pages;
        for (const auto& kr : leaf_ranges) {
            auto it = page_data_map_.find(kr.page_no);
            if (it != page_data_map_.end()) {
                sorted_pages.emplace_back(kr.page_no, it->second);
            }
        }
        for (const auto& kr : internal_ranges) {
            auto it = page_data_map_.find(kr.page_no);
            if (it != page_data_map_.end()) {
                sorted_pages.emplace_back(kr.page_no, it->second);
            }
        }

        // Relink pages
        relink_pages_by_content(sorted_pages);

        return true;
    }

private:
    size_t page_size_;
    catalog::TableSchema schema_;
    std::map<uint32_t, std::vector<uint8_t>> page_data_map_;
    std::vector<uint32_t> page_nos_;
    SecondaryIndexBTreeRepairStats stats_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SEC_INDEX_BTREE_REPAIR_HPP