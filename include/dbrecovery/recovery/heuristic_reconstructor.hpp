/**
 * @file heuristic_reconstructor.hpp
 * @brief Heuristic Reconstructor for B+Tree chain reconstruction
 *
 * Implements:
 * - B+Tree chain reconstruction from page trailers
 * - LSN-based version selection for TRUNCATE recovery
 * - Gap bridging for missing pages
 * - Conflict resolution for duplicate pages
 *
 * @version 2.2.0
 */

#ifndef DBRECOVERY_RECOVERY_HEURISTIC_RECONSTRUCTOR_HPP
#define DBRECOVERY_RECOVERY_HEURISTIC_RECONSTRUCTOR_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "dbrecovery/engine/advanced_signature_scraper.hpp"

namespace dbrecovery {
namespace recovery {

// ==================== Page Version Info ====================

/**
 * @brief Page version information for conflict resolution
 */
struct PageVersion {
    uint64_t disk_offset;           ///< Disk offset
    uint32_t page_number;           ///< Page number
    uint32_t space_id;              ///< Tablespace ID
    uint64_t lsn;                   ///< Log Sequence Number
    uint16_t page_type;             ///< Page type
    uint16_t page_level;           ///< B-tree level
    uint64_t index_id;             ///< Index ID
    std::vector<uint8_t> data;      ///< Page data
    double confidence;              ///< Confidence score
    bool checksum_valid;            ///< Checksum validation result

    /**
     * @brief Compare by LSN (for sorting)
     */
    bool operator<(const PageVersion& other) const {
        return lsn < other.lsn;
    }
};

// ==================== LSN Gap ====================

/**
 * @brief LSN gap information
 */
struct LSNGap {
    uint64_t start_lsn;             ///< Start LSN
    uint64_t end_lsn;               ///< End LSN
    uint32_t gap_size;              ///< Size of gap
    uint32_t missing_pages;         ///< Estimated missing pages
};

// ==================== Chain Link ====================

/**
 * @brief B+Tree chain link
 */
struct ChainLink {
    uint32_t from_page;             ///< Source page number
    uint32_t to_page;               ///< Target page number
    bool is_prev;                   ///< Is previous link (vs next)
    uint64_t lsn;                   ///< LSN at link time
    bool is_valid;                  ///< Link validity
};

// ==================== Reconstructed B+Tree ====================

/**
 * @brief Reconstructed B+Tree structure
 */
struct ReconstructedBTree {
    uint64_t index_id;              ///< Index ID
    uint32_t root_page;             ///< Root page number
    uint32_t total_pages;          ///< Total pages in tree
    uint32_t leaf_pages;           ///< Leaf pages count
    uint32_t non_leaf_pages;        ///< Non-leaf pages count
    std::vector<uint32_t> page_order; ///< Pages in traversal order
    std::map<uint32_t, std::vector<uint32_t>> children; ///< Page -> children mapping
    double confidence;              ///< Overall confidence
    bool is_complete;               ///< All pages found
    std::vector<uint32_t> missing_pages; ///< Missing page numbers
};

// ==================== Reconstruction Configuration ====================

/**
 * @brief Configuration for heuristic reconstruction
 */
struct ReconstructionConfig {
    uint32_t max_gap_size = 10;     ///< Maximum gap to bridge
    bool use_lsn_ordering = true;   ///< Use LSN for ordering
    bool strict_chain_validation = false; ///< Strict chain validation
    double min_confidence = 0.5;    ///< Minimum confidence threshold
    bool resolve_conflicts = true;  ///< Resolve page conflicts
    bool infer_missing = true;      ///< Infer missing pages
};

// ==================== Conflict Resolution Strategy ====================

/**
 * @brief Strategy for resolving page conflicts
 */
enum class ConflictStrategy {
    NEWEST_LSN,                     ///< Use page with newest LSN
    OLDEST_LSN,                     ///< Use page with oldest LSN
    BEST_CHECKSUM,                  ///< Use page with valid checksum
    HIGHEST_CONFIDENCE,             ///< Use page with highest confidence
    MERGE                           ///< Merge data from all versions
};

/**
 * @brief Convert ConflictStrategy to string
 */
inline std::string conflict_strategy_to_string(ConflictStrategy strategy) {
    switch (strategy) {
        case ConflictStrategy::NEWEST_LSN: return "NEWEST_LSN";
        case ConflictStrategy::OLDEST_LSN: return "OLDEST_LSN";
        case ConflictStrategy::BEST_CHECKSUM: return "BEST_CHECKSUM";
        case ConflictStrategy::HIGHEST_CONFIDENCE: return "HIGHEST_CONFIDENCE";
        case ConflictStrategy::MERGE: return "MERGE";
        default: return "UNKNOWN";
    }
}

// ==================== LSN Version Selector ====================

/**
 * @brief LSN-based version selector for TRUNCATE recovery
 *
 * When TRUNCATE TABLE is executed, old data pages remain on disk
 * but have older LSNs than the new table. This class filters
 * pages to recover pre-TRUNCATE data.
 */
class LSNVersionSelector {
public:
    /**
     * @brief Filter pages by maximum LSN
     *
     * @param pages All page versions
     * @param max_lsn Maximum LSN threshold
     * @return Pages with LSN <= max_lsn
     */
    static std::vector<PageVersion> filter_by_lsn(
        const std::vector<PageVersion>& pages,
        uint64_t max_lsn);

    /**
     * @brief Detect LSN boundary (TRUNCATE point)
     *
     * Analyzes LSN distribution to find the TRUNCATE boundary.
     * TRUNCATE typically creates a gap in LSN sequence.
     *
     * @param pages All page versions
     * @return Detected boundary LSN
     */
    static uint64_t detect_truncate_boundary(const std::vector<PageVersion>& pages);

    /**
     * @brief Detect LSN gaps
     *
     * @param pages All page versions
     * @return Detected LSN gaps
     */
    static std::vector<LSNGap> detect_lsn_gaps(const std::vector<PageVersion>& pages);

    /**
     * @brief Group pages by page number
     */
    static std::map<uint32_t, std::vector<PageVersion>> group_by_page_number(
        const std::vector<PageVersion>& pages);

    /**
     * @brief Select best version for each page
     */
    static std::vector<PageVersion> select_best_versions(
        const std::vector<PageVersion>& pages,
        ConflictStrategy strategy = ConflictStrategy::NEWEST_LSN);

private:
    /**
     * @brief Statistical analysis of LSN distribution
     */
    struct LSNStats {
        uint64_t min_lsn;
        uint64_t max_lsn;
        uint64_t median_lsn;
        double mean_lsn;
        double std_dev;
        std::vector<uint64_t> gaps;
    };

    static LSNStats analyze_lsn_distribution(const std::vector<PageVersion>& pages);

    // Make analyze_lsn_distribution accessible to helper classes
    friend class TruncateRecoveryHelper;
};

// ==================== Page Conflict Resolver ====================

/**
 * @brief Resolver for page conflicts
 *
 * When multiple versions of the same page are found (same page_no),
 * this class resolves the conflict using various strategies.
 */
class PageConflictResolver {
public:
    /**
     * @brief Resolve conflict between page versions
     *
     * @param versions All versions of the same page
     * @param strategy Resolution strategy
     * @return Best version
     */
    static PageVersion resolve_conflict(
        const std::vector<PageVersion>& versions,
        ConflictStrategy strategy = ConflictStrategy::NEWEST_LSN);

    /**
     * @brief Resolve all conflicts in page set
     *
     * @param pages All pages (may have duplicates)
     * @param strategy Resolution strategy
     * @return Pages with conflicts resolved
     */
    static std::vector<PageVersion> resolve_all_conflicts(
        const std::vector<PageVersion>& pages,
        ConflictStrategy strategy = ConflictStrategy::NEWEST_LSN);

    /**
     * @brief Check if pages have conflicts
     */
    static bool has_conflicts(const std::vector<PageVersion>& pages);

    /**
     * @brief Count conflicts
     */
    static size_t count_conflicts(const std::vector<PageVersion>& pages);

private:
    /**
     * @brief Select by newest LSN
     */
    static PageVersion select_newest_lsn(const std::vector<PageVersion>& versions);

    /**
     * @brief Select by oldest LSN
     */
    static PageVersion select_oldest_lsn(const std::vector<PageVersion>& versions);

    /**
     * @brief Select by best checksum
     */
    static PageVersion select_best_checksum(const std::vector<PageVersion>& versions);

    /**
     * @brief Select by highest confidence
     */
    static PageVersion select_highest_confidence(const std::vector<PageVersion>& versions);

    /**
     * @brief Merge versions
     */
    static PageVersion merge_versions(const std::vector<PageVersion>& versions);
};

// ==================== Heuristic Reconstructor ====================

/**
 * @brief Heuristic reconstructor for B+Tree chains
 *
 * Reconstructs B+Tree structures from scattered pages using:
 * - Page trailer pointers (FIL_PAGE_PREV/NEXT)
 * - Page directory entries
 * - LSN ordering
 * - Gap inference
 */
class HeuristicReconstructor {
public:
    /**
     * @brief Construct with configuration
     */
    explicit HeuristicReconstructor(const ReconstructionConfig& config = {});

    /**
     * @brief Destructor
     */
    ~HeuristicReconstructor();

    // ==================== Chain Reconstruction ====================

    /**
     * @brief Rebuild B+Tree chain from pages
     *
     * @param pages Carved pages
     * @return Reconstructed B+Tree
     */
    std::optional<ReconstructedBTree> rebuild_btree_chain(
        const std::vector<engine::CarvedPage>& pages);

    /**
     * @brief Rebuild B+Tree from page versions
     */
    std::optional<ReconstructedBTree> rebuild_btree_chain_v2(
        const std::vector<PageVersion>& pages);

    /**
     * @brief Rebuild multiple B+Trees (one per index)
     */
    std::vector<ReconstructedBTree> rebuild_all_btrees(
        const std::vector<engine::CarvedPage>& pages);

    // ==================== Gap Bridging ====================

    /**
     * @brief Bridge gaps in page chain
     *
     * Attempts to infer missing pages from context.
     *
     * @param pages Existing pages
     * @param max_gap Maximum gap size to bridge
     * @return Bridged pages (including inferred)
     */
    std::vector<PageVersion> bridge_gaps(
        const std::vector<PageVersion>& pages,
        uint32_t max_gap = 10);

    /**
     * @brief Detect gaps in page sequence
     */
    std::vector<uint32_t> detect_page_gaps(
        const std::vector<PageVersion>& pages);

    /**
     * @brief Infer missing page numbers
     */
    std::vector<uint32_t> infer_missing_pages(
        const std::vector<PageVersion>& pages);

    // ==================== Chain Validation ====================

    /**
     * @brief Validate B+Tree chain
     *
     * @param btree Reconstructed B+Tree
     * @return Validation result
     */
    struct ValidationResult {
        bool is_valid;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        double confidence;
    };
    ValidationResult validate_chain(const ReconstructedBTree& btree);

    /**
     * @brief Check for circular references
     */
    bool has_circular_reference(const ReconstructedBTree& btree);

    /**
     * @brief Check for broken links
     */
    std::vector<ChainLink> find_broken_links(const ReconstructedBTree& btree);

    // ==================== Statistics ====================

    /**
     * @brief Get reconstruction statistics
     */
    struct ReconstructionStats {
        uint32_t total_pages;
        uint32_t unique_pages;
        uint32_t conflicts_resolved;
        uint32_t gaps_bridged;
        uint32_t pages_inferred;
        uint32_t invalid_links;
        double elapsed_seconds;
    };
    ReconstructionStats get_stats() const { return stats_; }

private:
    ReconstructionConfig config_;
    ReconstructionStats stats_;

    // Make analyze_lsn_distribution accessible to helper classes
    friend class TruncateRecoveryHelper;

    // ==================== Internal Helpers ====================

    /**
     * @brief Build page number to page mapping
     */
    std::map<uint32_t, std::vector<const engine::CarvedPage*>> build_page_map(
        const std::vector<engine::CarvedPage>& pages);

    /**
     * @brief Find root page
     */
    std::optional<uint32_t> find_root_page(
        const std::map<uint32_t, std::vector<const engine::CarvedPage*>>& page_map);

    /**
     * @brief Traverse B+Tree from root
     */
    std::vector<uint32_t> traverse_btree(
        uint32_t root_page,
        const std::map<uint32_t, std::vector<const engine::CarvedPage*>>& page_map);

    /**
     * @brief Parse page chain pointers
     */
    struct PagePointers {
        uint32_t prev_page;
        uint32_t next_page;
        bool prev_valid;
        bool next_valid;
    };
    static PagePointers parse_page_pointers(const uint8_t* page_data, uint32_t page_size);

    /**
     * @brief Parse child page numbers from non-leaf page
     */
    static std::vector<uint32_t> parse_child_pages(
        const uint8_t* page_data, uint32_t page_size);
};

// ==================== TRUNCATE Recovery Helper ====================

/**
 * @brief Helper class for TRUNCATE TABLE recovery
 */
class TruncateRecoveryHelper {
public:
    /**
     * @brief Recover data after TRUNCATE TABLE
     *
     * @param disk_path Disk or image file path
     * @param schema Table schema (pre-TRUNCATE)
     * @param config Reconstruction config
     * @return Recovered pages
     */
    static std::vector<PageVersion> recover_truncated_data(
        const std::string& disk_path,
        const ReconstructionConfig& config = {});

    /**
     * @brief Find truncate point in LSN sequence
     */
    static uint64_t find_truncate_point(const std::vector<PageVersion>& pages);

    /**
     * @brief Filter pages to pre-TRUNCATE versions
     */
    static std::vector<PageVersion> filter_pre_truncate(
        const std::vector<PageVersion>& pages,
        uint64_t truncate_lsn);

private:
    /**
     * @brief Analyze LSN distribution for truncate detection
     */
    static double calculate_truncate_probability(
        const std::vector<PageVersion>& pages,
        uint64_t candidate_lsn);
};

// ==================== rm -rf Recovery Helper ====================

/**
 * @brief Helper class for rm -rf recovery
 */
class RmrfRecoveryHelper {
public:
    /**
     * @brief Recover deleted .ibd file
     *
     * @param disk_path Disk or image file path
     * @param config Reconstruction config
     * @return Recovered files
     */
    static std::vector<engine::CarvedFile> recover_deleted_ibd(
        const std::string& disk_path,
        const ReconstructionConfig& config = {});

    /**
     * @brief Reconstruct table from scattered pages
     */
    static std::optional<ReconstructedBTree> reconstruct_table(
        const std::vector<engine::CarvedPage>& pages);

    /**
     * @brief Infer table structure from data
     */
    static std::optional<std::string> infer_table_name(
        const std::vector<engine::CarvedPage>& pages);
};

} // namespace recovery
} // namespace dbrecovery

#endif // DBRECOVERY_RECOVERY_HEURISTIC_RECONSTRUCTOR_HPP