/**
 * @file page_merger.hpp
 * @brief Page merger for recovering data from corrupted/damaged IBD files
 */

#ifndef DBRECOVERY_INNODB_PAGE_MERGER_HPP
#define DBRECOVERY_INNODB_PAGE_MERGER_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <map>
#include "dbrecovery/innodb/heuristic_parser.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Recovery Strategy ====================

/**
 * @brief RecoveryStrategy - Strategy for page recovery
 */
enum class RecoveryStrategy {
    DEFAULT,        // Try standard parsing
    HEURISTIC,      // Use heuristic parser
    PATTERN,        // Pattern-based recovery
    MERGE,          // Merge with adjacent pages
    BACKWARD        // Scan backward for records
};

// ==================== Recovery Result Structure ====================

/**
 * @brief RecoveryResult - Result of page recovery using a specific strategy
 */
struct RecoveryResult {
    uint32_t page_no;
    RecoveryStrategy strategy;
    int records_found;
    std::string notes;
};

/**
 * @brief PageMerger - Advanced page recovery and merging for corrupted IBD files
 *
 * This class provides sophisticated recovery mechanisms for damaged IBD files,
 * including:
 * - Multi-page data merging
 * - Strategy-based recovery (standard, heuristic, pattern, backward)
 * - Cross-page record reconstruction
 * - Page header repair
 * - Damaged page recovery
 *
 * Key use cases:
 * - rm -rf recovery (partial file restoration)
 * - Partial page overwrite recovery
 * - File concatenation from multiple backups
 * - Corruption recovery with strategy fallback
 */
class PageMerger {
public:
    PageMerger() = default;
    ~PageMerger() = default;

    /**
     * @brief Merge multiple pages into single record set
     * @param pages Vector of page data
     * @param page_size Size of each page (typically 16384)
     * @return All recovered records
     */
    std::vector<HeuristicRecord> merge_pages(
        const std::vector<std::vector<uint8_t>>& pages,
        uint32_t page_size);

    /**
     * @brief Merge specific chunks from file data
     * @param file_data Complete file data
     * @param page_size Size of each page
     * @param chunk_ranges Vector of (page_no, offset) pairs
     * @return Recovered records from chunks
     */
    std::vector<HeuristicRecord> merge_page_chunks(
        const std::vector<uint8_t>& file_data,
        uint32_t page_size,
        const std::vector<std::pair<uint32_t, uint32_t>>& chunk_ranges);

    /**
     * @brief Detect recovery strategy based on page analysis
     * @param page_data Page buffer
     * @param page_size Page size
     * @return Recommended recovery strategy
     */
    static RecoveryStrategy detect_recovery_strategy(
        const uint8_t* page_data, size_t page_size);

    /**
     * @brief Recover page using best strategy
     * @param page_data Page buffer
     * @param page_size Page size
     * @param page_no Page number
     * @param strategy Recovery strategy to use
     * @return Recovered records
     */
    std::vector<HeuristicRecord> recover_page_with_strategy(
        const uint8_t* page_data,
        size_t page_size,
        uint32_t page_no,
        RecoveryStrategy strategy);

    /**
     * @brief Recover from multiple files with different strategies
     * @param file_paths Vector of file paths
     * @param page_size Size of each page
     * @return All recovered records
     */
    std::vector<HeuristicRecord> recover_from_multiple_files(
        const std::vector<std::string>& file_paths,
        uint32_t page_size);

    /**
     * @brief Reconstruct record from multiple pages
     * @param pages Vector of page data
     * @param expected_offset Expected record offset
     * @param page_size Page size
     * @return Recovered record or nullopt
     */
    std::optional<HeuristicRecord> reconstruct_record_from_pages(
        const std::vector<std::vector<uint8_t>>& pages,
        uint16_t expected_offset,
        uint32_t page_size);

    /**
     * @brief Repair damaged page header
     * @param page_data Page data
     * @param page_size Expected page size
     * @return Repaired page data
     */
    std::vector<uint8_t> repair_damaged_page(
        const std::vector<uint8_t>& page_data,
        uint32_t page_size);

    /**
     * @brief Generate recovery report
     * @param records Recovered records
     * @param results Recovery results by strategy
     * @return Report string
     */
    std::string get_recovery_report(
        const std::vector<HeuristicRecord>& records,
        const std::vector<RecoveryResult>& results);

    // Getters for statistics
    uint32_t get_pages_processed() const { return stats_.pages_processed; }
    uint32_t get_records_merged() const { return stats_.records_merged; }
    const std::map<uint32_t, int>& get_pages_with_records() const { return stats_.pages_with_records; }

private:
    struct MergeStats {
        uint32_t pages_processed;
        uint32_t records_merged;
        std::map<uint32_t, int> pages_with_records;
    } stats_{0, 0, {}};

    // Individual recovery methods
    std::vector<HeuristicRecord> recover_page_standard(
        const uint8_t* page_data, size_t page_size, uint32_t page_no);
    std::vector<HeuristicRecord> recover_page_heuristic(
        const uint8_t* page_data, size_t page_size, uint32_t page_no);
    std::vector<HeuristicRecord> recover_page_pattern(
        const uint8_t* page_data, size_t page_size, uint32_t page_no);
    std::vector<HeuristicRecord> recover_page_merged(
        const uint8_t* page_data, size_t page_size, uint32_t page_no);
    std::vector<HeuristicRecord> recover_page_backward(
        const uint8_t* page_data, size_t page_size, uint32_t page_no);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_PAGE_MERGER_HPP
