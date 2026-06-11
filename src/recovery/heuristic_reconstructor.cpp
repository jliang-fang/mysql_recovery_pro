/**
 * @file heuristic_reconstructor.cpp
 * @brief Implementation of Heuristic Reconstructor for B+Tree chain reconstruction
 *
 * @version 2.2.0
 */

#include "dbrecovery/recovery/heuristic_reconstructor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <queue>
#include <stack>
#include <unordered_set>

namespace dbrecovery {
namespace recovery {

// ==================== LSNVersionSelector Implementation ====================

std::vector<PageVersion> LSNVersionSelector::filter_by_lsn(
    const std::vector<PageVersion>& pages, uint64_t max_lsn) {

    std::vector<PageVersion> result;
    result.reserve(pages.size());

    for (const auto& page : pages) {
        if (page.lsn <= max_lsn) {
            result.push_back(page);
        }
    }

    return result;
}

uint64_t LSNVersionSelector::detect_truncate_boundary(const std::vector<PageVersion>& pages) {
    if (pages.empty()) {
        return 0;
    }

    auto stats = analyze_lsn_distribution(pages);

    // TRUNCATE typically creates a large LSN gap
    // Find the largest gap
    if (stats.gaps.empty()) {
        return stats.max_lsn;
    }

    // Find the largest gap
    uint64_t max_gap_size = 0;
    uint64_t boundary_lsn = stats.max_lsn;

    for (size_t i = 0; i < stats.gaps.size(); ++i) {
        if (i + 1 < pages.size()) {
            uint64_t gap = pages[i + 1].lsn - pages[i].lsn;
            if (gap > max_gap_size) {
                max_gap_size = gap;
                boundary_lsn = pages[i].lsn;
            }
        }
    }

    // Use statistical threshold: gap > 3 standard deviations
    if (max_gap_size > stats.std_dev * 3) {
        return boundary_lsn;
    }

    return stats.max_lsn;
}

std::vector<LSNGap> LSNVersionSelector::detect_lsn_gaps(const std::vector<PageVersion>& pages) {
    std::vector<LSNGap> gaps;

    if (pages.size() < 2) {
        return gaps;
    }

    // Sort by LSN
    std::vector<PageVersion> sorted_pages = pages;
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const PageVersion& a, const PageVersion& b) { return a.lsn < b.lsn; });

    // Calculate average gap
    uint64_t total_gap = 0;
    for (size_t i = 1; i < sorted_pages.size(); ++i) {
        total_gap += sorted_pages[i].lsn - sorted_pages[i - 1].lsn;
    }
    uint64_t avg_gap = total_gap / (sorted_pages.size() - 1);

    // Find gaps significantly larger than average
    for (size_t i = 1; i < sorted_pages.size(); ++i) {
        uint64_t gap_size = sorted_pages[i].lsn - sorted_pages[i - 1].lsn;
        if (gap_size > avg_gap * 3) {
            LSNGap gap;
            gap.start_lsn = sorted_pages[i - 1].lsn;
            gap.end_lsn = sorted_pages[i].lsn;
            gap.gap_size = gap_size;
            gap.missing_pages = static_cast<uint32_t>(gap_size / avg_gap);
            gaps.push_back(gap);
        }
    }

    return gaps;
}

std::map<uint32_t, std::vector<PageVersion>> LSNVersionSelector::group_by_page_number(
    const std::vector<PageVersion>& pages) {

    std::map<uint32_t, std::vector<PageVersion>> groups;

    for (const auto& page : pages) {
        groups[page.page_number].push_back(page);
    }

    return groups;
}

std::vector<PageVersion> LSNVersionSelector::select_best_versions(
    const std::vector<PageVersion>& pages, ConflictStrategy strategy) {

    auto groups = group_by_page_number(pages);
    std::vector<PageVersion> result;
    result.reserve(groups.size());

    for (auto& [page_no, versions] : groups) {
        if (versions.size() == 1) {
            result.push_back(versions[0]);
        } else {
            result.push_back(PageConflictResolver::resolve_conflict(versions, strategy));
        }
    }

    return result;
}

LSNVersionSelector::LSNStats LSNVersionSelector::analyze_lsn_distribution(
    const std::vector<PageVersion>& pages) {

    LSNStats stats = {};

    if (pages.empty()) {
        return stats;
    }

    // Sort by LSN
    std::vector<PageVersion> sorted_pages = pages;
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const PageVersion& a, const PageVersion& b) { return a.lsn < b.lsn; });

    stats.min_lsn = sorted_pages.front().lsn;
    stats.max_lsn = sorted_pages.back().lsn;

    // Calculate mean
    uint64_t sum = 0;
    for (const auto& page : sorted_pages) {
        sum += page.lsn;
    }
    stats.mean_lsn = static_cast<double>(sum) / sorted_pages.size();

    // Calculate median
    size_t mid = sorted_pages.size() / 2;
    if (sorted_pages.size() % 2 == 0) {
        stats.median_lsn = (sorted_pages[mid - 1].lsn + sorted_pages[mid].lsn) / 2;
    } else {
        stats.median_lsn = sorted_pages[mid].lsn;
    }

    // Calculate standard deviation
    double variance = 0;
    for (const auto& page : sorted_pages) {
        double diff = static_cast<double>(page.lsn) - stats.mean_lsn;
        variance += diff * diff;
    }
    stats.std_dev = std::sqrt(variance / sorted_pages.size());

    // Find gaps
    for (size_t i = 1; i < sorted_pages.size(); ++i) {
        uint64_t gap = sorted_pages[i].lsn - sorted_pages[i - 1].lsn;
        if (gap > 0) {
            stats.gaps.push_back(gap);
        }
    }

    return stats;
}

// ==================== PageConflictResolver Implementation ====================

PageVersion PageConflictResolver::resolve_conflict(
    const std::vector<PageVersion>& versions, ConflictStrategy strategy) {

    if (versions.empty()) {
        return PageVersion{};
    }

    if (versions.size() == 1) {
        return versions[0];
    }

    switch (strategy) {
        case ConflictStrategy::NEWEST_LSN:
            return select_newest_lsn(versions);
        case ConflictStrategy::OLDEST_LSN:
            return select_oldest_lsn(versions);
        case ConflictStrategy::BEST_CHECKSUM:
            return select_best_checksum(versions);
        case ConflictStrategy::HIGHEST_CONFIDENCE:
            return select_highest_confidence(versions);
        case ConflictStrategy::MERGE:
            return merge_versions(versions);
        default:
            return select_newest_lsn(versions);
    }
}

std::vector<PageVersion> PageConflictResolver::resolve_all_conflicts(
    const std::vector<PageVersion>& pages, ConflictStrategy strategy) {

    return LSNVersionSelector::select_best_versions(pages, strategy);
}

bool PageConflictResolver::has_conflicts(const std::vector<PageVersion>& pages) {
    auto groups = LSNVersionSelector::group_by_page_number(pages);
    for (const auto& [page_no, versions] : groups) {
        if (versions.size() > 1) {
            return true;
        }
    }
    return false;
}

size_t PageConflictResolver::count_conflicts(const std::vector<PageVersion>& pages) {
    auto groups = LSNVersionSelector::group_by_page_number(pages);
    size_t conflicts = 0;
    for (const auto& [page_no, versions] : groups) {
        if (versions.size() > 1) {
            conflicts += versions.size() - 1;
        }
    }
    return conflicts;
}

PageVersion PageConflictResolver::select_newest_lsn(const std::vector<PageVersion>& versions) {
    return *std::max_element(versions.begin(), versions.end(),
        [](const PageVersion& a, const PageVersion& b) { return a.lsn < b.lsn; });
}

PageVersion PageConflictResolver::select_oldest_lsn(const std::vector<PageVersion>& versions) {
    return *std::min_element(versions.begin(), versions.end(),
        [](const PageVersion& a, const PageVersion& b) { return a.lsn < b.lsn; });
}

PageVersion PageConflictResolver::select_best_checksum(const std::vector<PageVersion>& versions) {
    // First try to find one with valid checksum
    for (const auto& v : versions) {
        if (v.checksum_valid) {
            return v;
        }
    }
    // Fall back to highest confidence
    return select_highest_confidence(versions);
}

PageVersion PageConflictResolver::select_highest_confidence(const std::vector<PageVersion>& versions) {
    return *std::max_element(versions.begin(), versions.end(),
        [](const PageVersion& a, const PageVersion& b) { return a.confidence < b.confidence; });
}

PageVersion PageConflictResolver::merge_versions(const std::vector<PageVersion>& versions) {
    // For now, just use highest confidence version
    // Full merge would require more sophisticated logic
    return select_highest_confidence(versions);
}

// ==================== HeuristicReconstructor Implementation ====================

HeuristicReconstructor::HeuristicReconstructor(const ReconstructionConfig& config)
    : config_(config) {
    memset(&stats_, 0, sizeof(stats_));
}

HeuristicReconstructor::~HeuristicReconstructor() = default;

std::optional<ReconstructedBTree> HeuristicReconstructor::rebuild_btree_chain(
    const std::vector<engine::CarvedPage>& pages) {

    auto start_time = std::chrono::steady_clock::now();

    stats_.total_pages = pages.size();

    // Build page map
    auto page_map = build_page_map(pages);
    stats_.unique_pages = page_map.size();

    // Find root page
    auto root_page_opt = find_root_page(page_map);
    if (!root_page_opt.has_value()) {
        return std::nullopt;
    }

    uint32_t root_page = *root_page_opt;

    // Build B+Tree structure
    ReconstructedBTree btree;
    btree.root_page = root_page;
    btree.total_pages = 0;
    btree.leaf_pages = 0;
    btree.non_leaf_pages = 0;
    btree.confidence = 0.0;
    btree.is_complete = true;

    // Traverse B+Tree
    std::unordered_set<uint32_t> visited;
    std::queue<uint32_t> to_visit;
    to_visit.push(root_page);

    while (!to_visit.empty()) {
        uint32_t page_no = to_visit.front();
        to_visit.pop();

        if (visited.count(page_no) > 0) {
            continue;
        }
        visited.insert(page_no);

        auto it = page_map.find(page_no);
        if (it == page_map.end() || it->second.empty()) {
            btree.missing_pages.push_back(page_no);
            btree.is_complete = false;
            continue;
        }

        const auto* page = it->second.front();
        btree.page_order.push_back(page_no);
        btree.total_pages++;

        if (page->is_leaf) {
            btree.leaf_pages++;
        } else {
            btree.non_leaf_pages++;
            // Parse child pages
            auto children = parse_child_pages(page->data.data(), page->data.size());
            btree.children[page_no] = children;
            for (uint32_t child : children) {
                if (visited.count(child) == 0) {
                    to_visit.push(child);
                }
            }
        }

        btree.confidence += page->confidence;
    }

    if (btree.total_pages > 0) {
        btree.confidence /= btree.total_pages;
    }

    // Find index ID
    if (!page_map.empty() && !page_map.begin()->second.empty()) {
        btree.index_id = page_map.begin()->second.front()->index_id;
    }

    auto end_time = std::chrono::steady_clock::now();
    stats_.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();

    return btree;
}

std::optional<ReconstructedBTree> HeuristicReconstructor::rebuild_btree_chain_v2(
    const std::vector<PageVersion>& pages) {

    // Convert PageVersion to CarvedPage
    std::vector<engine::CarvedPage> carved_pages;
    carved_pages.reserve(pages.size());

    for (const auto& pv : pages) {
        engine::CarvedPage cp;
        cp.disk_offset = pv.disk_offset;
        cp.page_size = pv.data.size();
        cp.page_type = pv.page_type;
        cp.page_number = pv.page_number;
        cp.space_id = pv.space_id;
        cp.lsn = pv.lsn;
        cp.confidence = pv.confidence;
        cp.checksum_valid = pv.checksum_valid;
        cp.index_id = pv.index_id;
        cp.page_level = pv.page_level;
        cp.is_leaf = (pv.page_level == 0);
        cp.data = pv.data;
        carved_pages.push_back(std::move(cp));
    }

    return rebuild_btree_chain(carved_pages);
}

std::vector<ReconstructedBTree> HeuristicReconstructor::rebuild_all_btrees(
    const std::vector<engine::CarvedPage>& pages) {

    // Group by index ID
    std::map<uint64_t, std::vector<engine::CarvedPage>> index_groups;

    for (const auto& page : pages) {
        if (page.page_type == engine::InnoDBSignatureLibrary::FIL_PAGE_INDEX) {
            index_groups[page.index_id].push_back(page);
        }
    }

    std::vector<ReconstructedBTree> btrees;

    for (auto& [index_id, index_pages] : index_groups) {
        auto btree = rebuild_btree_chain(index_pages);
        if (btree.has_value()) {
            btrees.push_back(*btree);
        }
    }

    return btrees;
}

std::vector<PageVersion> HeuristicReconstructor::bridge_gaps(
    const std::vector<PageVersion>& pages, uint32_t max_gap) {

    if (pages.empty()) {
        return pages;
    }

    // Sort by page number
    std::vector<PageVersion> sorted_pages = pages;
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const PageVersion& a, const PageVersion& b) {
                  return a.page_number < b.page_number;
              });

    std::vector<PageVersion> result;
    result.reserve(sorted_pages.size() + max_gap * 10);

    for (size_t i = 0; i < sorted_pages.size(); ++i) {
        result.push_back(sorted_pages[i]);

        // Check for gap to next page
        if (i + 1 < sorted_pages.size()) {
            uint32_t current_page = sorted_pages[i].page_number;
            uint32_t next_page = sorted_pages[i + 1].page_number;

            if (next_page > current_page + 1 && next_page - current_page <= max_gap) {
                // Bridge the gap with inferred pages
                for (uint32_t p = current_page + 1; p < next_page; ++p) {
                    PageVersion inferred;
                    inferred.page_number = p;
                    inferred.space_id = sorted_pages[i].space_id;
                    inferred.page_type = sorted_pages[i].page_type;
                    inferred.index_id = sorted_pages[i].index_id;
                    inferred.confidence = 0.3; // Low confidence for inferred
                    inferred.checksum_valid = false;
                    stats_.pages_inferred++;
                    result.push_back(inferred);
                }
                stats_.gaps_bridged++;
            }
        }
    }

    return result;
}

std::vector<uint32_t> HeuristicReconstructor::detect_page_gaps(
    const std::vector<PageVersion>& pages) {

    if (pages.size() < 2) {
        return {};
    }

    // Sort by page number
    std::vector<PageVersion> sorted_pages = pages;
    std::sort(sorted_pages.begin(), sorted_pages.end(),
              [](const PageVersion& a, const PageVersion& b) {
                  return a.page_number < b.page_number;
              });

    std::vector<uint32_t> gaps;

    for (size_t i = 1; i < sorted_pages.size(); ++i) {
        uint32_t prev_page = sorted_pages[i - 1].page_number;
        uint32_t curr_page = sorted_pages[i].page_number;

        for (uint32_t p = prev_page + 1; p < curr_page; ++p) {
            gaps.push_back(p);
        }
    }

    return gaps;
}

std::vector<uint32_t> HeuristicReconstructor::infer_missing_pages(
    const std::vector<PageVersion>& pages) {

    return detect_page_gaps(pages);
}

HeuristicReconstructor::ValidationResult HeuristicReconstructor::validate_chain(
    const ReconstructedBTree& btree) {

    ValidationResult result;
    result.is_valid = true;
    result.confidence = btree.confidence;

    // Check for root page
    if (btree.root_page == 0 || btree.root_page == 0xFFFFFFFF) {
        result.errors.push_back("Invalid root page number");
        result.is_valid = false;
    }

    // Check for circular references
    if (has_circular_reference(btree)) {
        result.errors.push_back("Circular reference detected");
        result.is_valid = false;
    }

    // Check for missing pages
    if (!btree.missing_pages.empty()) {
        result.warnings.push_back("Missing pages: " + std::to_string(btree.missing_pages.size()));
        result.confidence *= 0.8;
    }

    // Check leaf/non-leaf ratio
    if (btree.non_leaf_pages > 0 && btree.leaf_pages == 0) {
        result.warnings.push_back("No leaf pages found");
        result.confidence *= 0.5;
    }

    return result;
}

bool HeuristicReconstructor::has_circular_reference(const ReconstructedBTree& btree) {
    std::unordered_set<uint32_t> visited;
    std::unordered_set<uint32_t> in_stack;

    std::function<bool(uint32_t)> dfs = [&](uint32_t page) -> bool {
        if (in_stack.count(page) > 0) {
            return true; // Circular reference found
        }
        if (visited.count(page) > 0) {
            return false;
        }

        visited.insert(page);
        in_stack.insert(page);

        auto it = btree.children.find(page);
        if (it != btree.children.end()) {
            for (uint32_t child : it->second) {
                if (dfs(child)) {
                    return true;
                }
            }
        }

        in_stack.erase(page);
        return false;
    };

    return dfs(btree.root_page);
}

std::vector<ChainLink> HeuristicReconstructor::find_broken_links(
    const ReconstructedBTree& btree) {

    std::vector<ChainLink> broken_links;

    for (const auto& [page_no, children] : btree.children) {
        for (uint32_t child : children) {
            // Check if child exists
            bool found = false;
            for (uint32_t p : btree.page_order) {
                if (p == child) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                ChainLink link;
                link.from_page = page_no;
                link.to_page = child;
                link.is_valid = false;
                broken_links.push_back(link);
            }
        }
    }

    return broken_links;
}

std::map<uint32_t, std::vector<const engine::CarvedPage*>> HeuristicReconstructor::build_page_map(
    const std::vector<engine::CarvedPage>& pages) {

    std::map<uint32_t, std::vector<const engine::CarvedPage*>> page_map;

    for (const auto& page : pages) {
        page_map[page.page_number].push_back(&page);
    }

    return page_map;
}

std::optional<uint32_t> HeuristicReconstructor::find_root_page(
    const std::map<uint32_t, std::vector<const engine::CarvedPage*>>& page_map) {

    // Root page is the non-leaf page with highest level
    uint32_t root_page = 0;
    uint16_t max_level = 0;

    for (const auto& [page_no, versions] : page_map) {
        for (const auto* page : versions) {
            if (!page->is_leaf && page->page_level > max_level) {
                max_level = page->page_level;
                root_page = page_no;
            }
        }
    }

    // If no non-leaf pages, use first page
    if (root_page == 0 && !page_map.empty()) {
        root_page = page_map.begin()->first;
    }

    if (root_page == 0) {
        return std::nullopt;
    }

    return root_page;
}

std::vector<uint32_t> HeuristicReconstructor::traverse_btree(
    uint32_t root_page,
    const std::map<uint32_t, std::vector<const engine::CarvedPage*>>& page_map) {

    std::vector<uint32_t> order;
    std::unordered_set<uint32_t> visited;

    std::function<void(uint32_t)> dfs = [&](uint32_t page_no) {
        if (visited.count(page_no) > 0) {
            return;
        }
        visited.insert(page_no);
        order.push_back(page_no);

        auto it = page_map.find(page_no);
        if (it == page_map.end() || it->second.empty()) {
            return;
        }

        const auto* page = it->second.front();
        if (!page->is_leaf) {
            auto children = parse_child_pages(page->data.data(), page->data.size());
            for (uint32_t child : children) {
                dfs(child);
            }
        }
    };

    dfs(root_page);
    return order;
}

HeuristicReconstructor::PagePointers HeuristicReconstructor::parse_page_pointers(
    const uint8_t* page_data, uint32_t page_size) {

    PagePointers ptrs = {};

    if (page_size < 16) {
        return ptrs;
    }

    // FIL_PAGE_PREV at offset 8
    ptrs.prev_page = (page_data[8] << 24) | (page_data[9] << 16) |
                     (page_data[10] << 8) | page_data[11];
    ptrs.prev_valid = (ptrs.prev_page != 0xFFFFFFFF);

    // FIL_PAGE_NEXT at offset 12
    ptrs.next_page = (page_data[12] << 24) | (page_data[13] << 16) |
                     (page_data[14] << 8) | page_data[15];
    ptrs.next_valid = (ptrs.next_page != 0xFFFFFFFF);

    return ptrs;
}

std::vector<uint32_t> HeuristicReconstructor::parse_child_pages(
    const uint8_t* page_data, uint32_t page_size) {

    std::vector<uint32_t> children;

    // For non-leaf pages, children are stored in records
    // This is a simplified implementation

    // Check page directory slots
    if (page_size < engine::InnoDBSignatureLibrary::PAGE_HEADER_OFFSET + 4) {
        return children;
    }

    const uint8_t* ph = page_data + engine::InnoDBSignatureLibrary::PAGE_HEADER_OFFSET;
    uint16_t n_dir_slots = (ph[0] << 8) | ph[1];

    // Directory slots are at the end of the page
    // Each slot is 2 bytes pointing to a record
    for (uint16_t i = 0; i < n_dir_slots; ++i) {
        uint32_t slot_offset = page_size - 2 - (i * 2);
        if (slot_offset < 100 || slot_offset + 2 > page_size) {
            continue;
        }

        uint16_t record_offset = (page_data[slot_offset] << 8) | page_data[slot_offset + 1];
        if (record_offset < 100 || record_offset >= page_size - 8) {
            continue;
        }

        // For non-leaf pages, child page number is stored in the record
        // This is a simplified extraction
        // In reality, need to parse the record properly
    }

    return children;
}

// ==================== TruncateRecoveryHelper Implementation ====================

std::vector<PageVersion> TruncateRecoveryHelper::recover_truncated_data(
    const std::string& disk_path, const ReconstructionConfig& config) {

    // This would use the disk scanner to find pages
    // For now, return empty
    return {};
}

uint64_t TruncateRecoveryHelper::find_truncate_point(const std::vector<PageVersion>& pages) {
    return LSNVersionSelector::detect_truncate_boundary(pages);
}

std::vector<PageVersion> TruncateRecoveryHelper::filter_pre_truncate(
    const std::vector<PageVersion>& pages, uint64_t truncate_lsn) {

    return LSNVersionSelector::filter_by_lsn(pages, truncate_lsn);
}

double TruncateRecoveryHelper::calculate_truncate_probability(
    const std::vector<PageVersion>& pages, uint64_t candidate_lsn) {

    auto stats = LSNVersionSelector::analyze_lsn_distribution(
        LSNVersionSelector::filter_by_lsn(pages, candidate_lsn));

    // Higher probability if there's a significant LSN gap
    double probability = 0.0;

    for (uint64_t gap : stats.gaps) {
        if (gap > stats.std_dev * 2) {
            probability += 0.2;
        }
    }

    return std::min(probability, 1.0);
}

// ==================== RmrfRecoveryHelper Implementation ====================

std::vector<engine::CarvedFile> RmrfRecoveryHelper::recover_deleted_ibd(
    const std::string& disk_path, const ReconstructionConfig& config) {

    // This would use the page carver
    // For now, return empty
    return {};
}

std::optional<ReconstructedBTree> RmrfRecoveryHelper::reconstruct_table(
    const std::vector<engine::CarvedPage>& pages) {

    HeuristicReconstructor reconstructor;
    return reconstructor.rebuild_btree_chain(pages);
}

std::optional<std::string> RmrfRecoveryHelper::infer_table_name(
    const std::vector<engine::CarvedPage>& pages) {

    // Try to find table name hints in data
    // This is a heuristic approach

    for (const auto& page : pages) {
        // Look for common patterns in data
        // This would need more sophisticated implementation
    }

    return std::nullopt;
}

} // namespace recovery
} // namespace dbrecovery
