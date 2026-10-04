/**
 * @file signature_scraper.hpp
 * @brief Signature-Based Scraper (雕刻机) for MySQL 5.6 extreme recovery
 * 
 * Implements high-performance sliding window scanning with InnoDB page feature matching.
 * Designed to find InnoDB data pages in raw disk images, even when file system metadata
 * is completely lost (rm -rf scenario).
 * 
 * Key features:
 * - Multi-threaded sliding window scan (PINNED/LOCKED memory)
 * - LSN-based page filtering (skip pages newer than known good state)
 * - InnoDB page header signature matching
 * - Checksum validation (crc32, slow, disk)
 * - Page chain reconstruction from head/tail pointers
 */

#ifndef DBRECOVERY_ENGINE_SIGNATURE_SCRAPER_HPP
#define DBRECOVERY_ENGINE_SIGNATURE_SCRAPER_HPP

#include "dbrecovery/engine/raw_disk_engine.hpp"
#include "dbrecovery/engine/schema_template.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <optional>
#include <thread>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <condition_variable>

namespace dbrecovery {
namespace engine {

// ==================== Page Signature Constants ====================

/// @brief InnoDB page signature pattern for MySQL 5.6/8.0
struct PageSignature {
    uint16_t page_type;         ///< FIL_PAGE_TYPE (offset 24)
    uint32_t space_id_mask;     ///< Mask for space_id validation
    uint32_t space_id_value;    ///< Expected space_id bits
    uint32_t page_number_mask;  ///< Mask for page_number validation
    uint32_t page_number_value; ///< Expected page_number bits
};

/// @brief Predefined page signatures for common scenarios
inline std::vector<PageSignature> get_mysql_56_signatures() {
    std::vector<PageSignature> sigs;
    
    // MySQL 5.6/8.0 Index Page (0x45BF = 17855 in big-endian)
    sigs.push_back({17855, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0});
    
    // BLOB Page (0x45BD = 17853)
    sigs.push_back({17853, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0});
    
    // SDI Page (0x45BE = 17854)  
    sigs.push_back({17854, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0});
    
    // UNDO Log Page (0x45B2 = 17842)
    sigs.push_back({17842, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0});
    
    // Allocated page (0x0000 = 0)
    sigs.push_back({0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0});
    
    return sigs;
}

/// @brief InnoDB page header checksum flags
enum class ChecksumType : uint32_t {
    CRC32 = 0x1,        ///< CRC32 checksum (MySQL 5.6+ default)
    INNODB = 0x2,       ///< Old InnoDB checksum
    NONE = 0x4,         ///< No checksum
    FULL_PAGE = 0x8    ///< Full page checksum
};

// ==================== Found Page with Signature Match ====================

/**
 * @brief Page found by signature scanning
 * 
 * Contains both the raw page data and parsed metadata about signature matches.
 */
struct FoundPage {
    uint64_t disk_offset;       ///< Offset in disk/image file
    uint32_t page_number;       ///< Page number from header
    uint32_t space_id;          ///< Tablespace ID
    uint16_t page_type;         ///< Page type (FIL_PAGE_INDEX, etc.)
    uint32_t page_size;         ///< Page size (4096, 8192, 16384, etc.)
    uint64_t lsn;               ///< Log Sequence Number
    
    // Page header fields for validation
    uint32_t prev_page;         ///< Previous page in chain
    uint32_t next_page;         ///< Next page in chain
    uint16_t heap_top;          ///< Heap top offset
    uint16_t n_records;         ///< Number of records
    
    // Signature matching information
    std::vector<PageSignature> matched_signatures;  ///< Signatures that matched
    bool has_valid_checksum;                        ///< Checksum validated
    ChecksumType checksum_type;                     ///< Checksum algorithm used
    
    // Data buffer
    std::vector<uint8_t> data;  ///< Full page data (page_size bytes)
    
    // Verification flags
    bool is_valid() const {
        return has_valid_checksum && 
               page_type == 0 || 
               engine::PageScanner::is_valid_page_type(page_type);
    }
    
    bool is_index_page() const {
        return page_type == 17855;  // 0x45BF FIL_PAGE_INDEX
    }
};

// ==================== Page Entry for B+Tree chain reconstruction ====================

/**
 * @brief Page entry for B+Tree chain reconstruction
 * 
 * Used to rebuild the logical page chain from head/tail pointers in page headers.
 */
struct PageChainEntry {
    uint64_t disk_offset;       ///< Disk offset
    uint32_t page_number;       ///< Page number
    uint32_t space_id;          ///< Space ID
    uint32_t prev_page;         ///< Previous page in B+Tree
    uint32_t next_page;         ///< Next page in B+Tree
    uint64_t lsn;               ///< LSN for version ordering
    uint16_t page_type;         ///< Page type
    
    std::vector<uint8_t> data;  ///< Page data
    
    // Open chain - entry at one end
    bool is_open_chain_start() const {
        return prev_page == 0xFFFFFFFF && next_page != 0xFFFFFFFF;
    }
    
    bool is_open_chain_end() const {
        return next_page == 0xFFFFFFFF && prev_page != 0xFFFFFFFF;
    }
};

// ==================== Page Version for conflict detection ====================

/**
 * @brief Multiple versions of same page found during scan
 * 
 * Used to resolve conflicts when multiple copies of same page exist (e.g., after TRUNCATE).
 */
struct PageVersion {
    uint64_t disk_offset;           ///< Disk offset
    uint64_t lsn;                   ///< LSN of this version
    uint32_t page_number;           ///< Page number
    uint32_t space_id;              ///< Space ID
    
    std::vector<uint8_t> data;      ///< Page data
    bool is_selected;               ///< Whether this version is selected for recovery
    /// Full parse of this version, so conflict resolution can emit exactly the
    /// page that was found instead of a field-stripped reconstruction.
    FoundPage snapshot;
    
    // LSN comparison for version selection
    bool is_older_than(const PageVersion& other) const {
        // LSN is a 64-bit value that increases monotonically
        return lsn < other.lsn;
    }
    
    bool is_newer_than(const PageVersion& other) const {
        return lsn > other.lsn;
    }
};

// ==================== Page Conflict Resolution ====================

/**
 * @brief Policy for resolving conflicting page versions
 */
enum class PageConflictPolicy {
    EARLIEST_LSN,     ///< Select the version with earliest (oldest) LSN
    LATEST_LSN,       ///< Select the version with latest (newest) LSN
    BEST_CHECKSUM,    ///< Select version with best checksum score
    PREVIOUS_PAGE,    ///< Select version matching previous page in chain
};

/**
 * @brief Conflict resolution statistics
 */
struct ConflictResolutionStats {
    uint64_t total_conflicts;         ///< Total conflicts found
    uint64_t resolved_earliest;       ///< Resolved by earliest LSN
    uint64_t resolved_latest;         ///< Resolved by latest LSN
    uint64_t resolved_best_checksum;  ///< Resolved by checksum
    uint64_t unresolved;              ///< Conflicts that couldn't be resolved
};

// ==================== Schema Template for Recovery ====================
// SchemaTemplate is included from schema_template.hpp (see top of file)

// ==================== SignatureBasedScraper ====================

/**
 * @brief Signature-Based Scraper (雕刻机)
 * 
 * High-performance disk scanner for finding InnoDB pages using signature matching.
 * Implements sliding window scan with advanced filtering capabilities.
 * 
 * Key algorithms:
 * 1. Sliding window scan with configurable step size
 * 2. LSN-based filtering (skip pages newer than threshold)
 * 3. Checksum validation for data integrity
 * 4. Page chain reconstruction from head pointers
 * 5. Conflict detection for multiple page versions
 */
class SignatureBasedScraper {
public:
    /// @brief Scan statistics
    struct ScanStats {
        uint64_t total_bytes_scanned;
        uint64_t pages_checked;
        uint64_t pages_matched;
        uint64_t pages_with_valid_checksum;
        uint64_t pages_filtered_by_lsn;
        uint64_t conflict_resolutions;
        double scan_time_seconds;
        uint64_t avg_throughput_mbps;
    };

    /// @brief Scan configuration
    struct ScanConfig {
        // Scan parameters
        uint32_t min_page_size = 4096;
        uint32_t max_page_size = 65536;
        uint64_t max_bytes_to_scan = 0;           // 0 = scan entire device
        uint64_t max_pages = 0;                   // 0 = no limit
        
        // LSN filtering (skip pages newer than this)
        std::optional<uint64_t> max_lsn;
        
        // Checksum validation
        bool validate_checksums = true;
        bool strict_checksum = false;             // Reject pages with no checksum
        
        // Page type filtering
        std::vector<uint16_t> allowed_page_types;
        bool filter_to_index_pages = false;       // Only index pages
        
        // Conflict resolution
        // Newest wins: a doublewrite or stale copy of a logical page carries
        // the same (space_id, page_number) but an older LSN and fewer records.
        // Picking the oldest copy silently truncates the row set.
        PageConflictPolicy conflict_policy = PageConflictPolicy::LATEST_LSN;
        
        // Multi-threading
        unsigned num_threads = 4;
        size_t chunk_size = 1024 * 1024;          // 1MB chunks
        
        // B+Tree chain reconstruction
        bool reconstruct_chains = true;
        bool validate_chain_links = true;
    };

    /**
     * @brief Construct a new SignatureBasedScraper
     * 
     * @param engine Reference to the disk engine to scan
     */
    explicit SignatureBasedScraper(RawDiskIOEngine& engine);

    /// @brief Destroy the SignatureBasedScraper
    ~SignatureBasedScraper();

    // Disable copy
    SignatureBasedScraper(const SignatureBasedScraper&) = delete;
    SignatureBasedScraper& operator=(const SignatureBasedScraper&) = delete;

    /**
     * @brief Scan for InnoDB pages
     * 
     * @param config Scan configuration
     * @param callback Callback for each found page
     * @return Scan statistics
     */
    ScanStats scan(const ScanConfig& config,
                   std::function<void(const FoundPage&)> callback);

    /**
     * @brief Scan with schema template filtering
     * 
     * Uses table structure to verify pages match expected format.
     * Useful for TRUNCATE recovery where table structure is known.
     * 
     * @param config Scan configuration
     * @param schema_template Expected table structure
     * @param callback Callback for each valid page
     * @return Scan statistics
     */
    ScanStats scan_with_template(const ScanConfig& config,
                                  const SchemaTemplate& schema_template,
                                  std::function<void(const FoundPage&)> callback);

    /**
     * @brief Reconstruct B+Tree page chains
     * 
     * Uses prev_page/next_page pointers to reconstruct logical page chains.
     * 
     * @param pages Found pages to reconstruct
     * @return Vector of reconstructed chains
     */
    std::vector<std::vector<FoundPage>> reconstruct_chains(
        const std::vector<FoundPage>& pages);

    /**
     * @brief Resolve page version conflicts
     * 
     * When multiple versions of same page are found, select best version.
     * 
     * @param versions Multiple versions of same page
     * @param policy Conflict resolution policy
     * @param stats Statistics output
     * @return Selected pages
     */
    std::vector<FoundPage> resolve_conflicts(
        const std::vector<std::vector<PageVersion>>& versions,
        PageConflictPolicy policy,
        ConflictResolutionStats& stats);

    /**
     * @brief Validate page checksum
     * 
     * @param page_data Page data
     * @param page_size Page size
     * @param expected_checksum Expected checksum (0 = auto-detect)
     * @return True if checksum is valid
     */
    static bool validate_checksum(const uint8_t* page_data, uint32_t page_size,
                                   uint32_t expected_checksum = 0);

    /**
     * @brief Get scan statistics
     * 
     * @return Current scan statistics
     */
    const ScanStats& get_stats() const { return stats_; }

    /**
     * @brief Get discovered page chains
     * 
     * @return Vector of page chains
     */
    const std::vector<std::vector<FoundPage>>& get_chains() const {
        return discovered_chains_;
    }

    /**
     * @brief Set the progress callback
     * 
     * @param callback Progress callback
     */
    void set_progress_callback(std::function<void(double)> callback) {
        progress_callback_ = callback;
    }

    /**
     * @brief Set the schema template (for TRUNCATE recovery)
     * 
     * @param template Schema template
     */
    void set_schema_template(const SchemaTemplate& temp) { schema_template_ = temp; }

private:
    RawDiskIOEngine& engine_;
    ScanStats stats_;
    std::vector<std::vector<FoundPage>> discovered_chains_;
    std::function<void(double)> progress_callback_;
    SchemaTemplate schema_template_;
    
    std::atomic<uint64_t> bytes_scanned_;
    std::atomic<uint64_t> pages_scanned_;
    std::mutex stats_mutex_;
    
    // Thread pool for multi-threaded scanning
    struct ThreadPool;
    std::unique_ptr<ThreadPool> thread_pool_;
    
    // Page mapping for conflict detection
    std::unordered_map<uint64_t, std::vector<PageVersion>> page_versions_;
    std::shared_mutex page_versions_mutex_;
    
    // Configuration
    ScanConfig current_config_;
    
    // ==================== Private Methods ====================
    
    /**
     * @brief Scan a single chunk of data
     */
    void scan_chunk(uint64_t offset, const std::vector<uint8_t>& chunk,
                    std::function<void(const FoundPage&)> callback);

    /**
     * @brief Check if page matches any signature
     */
    std::vector<PageSignature> match_signatures(const uint8_t* page_data,
                                                 uint32_t page_size);

    /**
     * @brief Parse page header
     */
    bool parse_page_header(const uint8_t* page_data, uint32_t page_size,
                           FoundPage& page);

    /**
     * @brief Check if page passes LSN filter
     */
    bool passes_lsn_filter(uint64_t lsn);

    /**
     * @brief Validate page against schema template
     */
    bool matches_schema_template(const FoundPage& page);

    /**
     * @brief Worker thread function
     */
    void worker_thread(uint64_t start_offset, uint64_t end_offset,
                       std::function<void(const FoundPage&)> callback);

    /**
     * @brief Process a single page entry
     */
    /// @param emit_immediately deliver to the caller right away; the batched
    ///        scan() path passes false and delivers once per (space_id,
    ///        page_number) after conflict resolution instead, which is what
    ///        keeps duplicate copies of the same page from yielding duplicate
    ///        rows downstream.
    bool process_page(uint64_t offset, const uint8_t* page_data,
                      uint32_t page_size, std::function<void(const FoundPage&)> callback,
                      bool emit_immediately = true);

    /**
     * @brief Register page version for conflict detection
     */
    void register_page_version(const PageVersion& version);
};

// ==================== Inline Implementations ====================

inline bool SignatureBasedScraper::validate_checksum(const uint8_t* page_data,
                                                       uint32_t page_size,
                                                       uint32_t expected_checksum) {
    if (!page_data || page_size < 46) {
        return false;
    }

    // Use the canonical InnoDB checksum validation
    return ::dbrecovery::innodb::is_page_checksum_valid(page_data, page_size);
}

inline bool SignatureBasedScraper::passes_lsn_filter(uint64_t lsn) {
    if (!current_config_.max_lsn.has_value()) {
        return true;
    }
    return lsn <= current_config_.max_lsn.value();
}

// ==================== EnhancedPageReconstructor ====================

/**
 * @brief Enhanced page reconstructor with B+Tree chain support
 * 
 * Reconstructs logical B+Tree structure from physical page data.
 */
class EnhancedPageReconstructor {
public:
    /// @brief Reconstructed B+Tree page
    struct ReconstructedPage {
        uint64_t disk_offset;
        uint32_t page_number;
        uint32_t space_id;
        uint16_t page_type;
        uint64_t lsn;
        
        // Page content
        uint16_t n_records;           ///< Number of user records
        uint16_t heap_top;            ///< Heap top offset
        uint32_t max_transaction_id;  ///< Transaction ID (dynamic records)
        
        // Chain links
        uint32_t prev_page;
        uint32_t next_page;
        
        // Recomputed checksum
        bool checksum_valid;
        
        std::vector<uint8_t> data;
    };

    /// @brief B+Tree chain
    struct BPlusTreeChain {
        uint32_t space_id;
        uint32_t root_page;
        uint64_t index_id;
        
        std::vector<ReconstructedPage> pages;
        
        // Chain properties
        bool is_complete;       ///< All pages in chain found
        bool is_sorted;         ///< Pages sorted by page_number
    };

    /**
     * @brief Reconstruct pages from raw data
     * 
     * @param engine Disk engine
     * @param page_size Page size
     * @param max_pages Maximum pages to reconstruct
     * @return Reconstructed pages
     */
    static std::vector<ReconstructedPage> reconstruct(
        RawDiskIOEngine& engine,
        uint32_t page_size,
        uint64_t max_pages = 0);

    /**
     * @brief Reconstruct B+Tree chains
     * 
     * @param pages Reconstructed pages
     * @return B+Tree chains
     */
    static std::vector<BPlusTreeChain> reconstruct_chains(
        const std::vector<ReconstructedPage>& pages);
};

// ==================== SlidingWindowScanner ====================

/**
 * @brief Low-level sliding window scanner
 * 
 * Implements the actual sliding window algorithm with PINNED memory.
 */
class SlidingWindowScanner {
public:
    /// @brief Window configuration
    struct WindowConfig {
        size_t window_size;         ///< Size of each window (default: 4MB)
        size_t step_size;           ///< Step between windows (default: window_size/2)
        size_t page_size;           ///< Page size to scan for (default: 16KB)
        uint64_t max_offset;        ///< Maximum offset to scan (0 = no limit)
        unsigned num_threads;       ///< Number of threads (default: 4)
    };

    /// @brief Scan result
    struct ScanResult {
        uint64_t offset;            ///< Offset in file
        uint32_t page_number;
        uint32_t space_id;
        uint16_t page_type;
        uint64_t lsn;
        
        // Confidence score (0-100)
        uint8_t confidence;
        
        std::vector<uint8_t> page_data;
    };

    /**
     * @brief Construct a new SlidingWindowScanner
     * 
     * @param engine Disk engine to scan
     */
    explicit SlidingWindowScanner(RawDiskIOEngine& engine);

    /**
     * @brief Scan using sliding window
     * 
     * @param config Scanner configuration
     * @param callback Callback for each found page
     * @return Number of pages found
     */
    uint64_t scan(const WindowConfig& config,
                  std::function<void(const ScanResult&)> callback);

    /**
     * @brief PINNED memory implementation
     * 
     * Uses mlock() to prevent page eviction during scanning.
     */
    class PinnedBuffer {
    public:
        explicit PinnedBuffer(size_t size);
        ~PinnedBuffer();
        
        // Disable copy
        PinnedBuffer(const PinnedBuffer&) = delete;
        PinnedBuffer& operator=(const PinnedBuffer&) = delete;
        
        // Move
        PinnedBuffer(PinnedBuffer&& other) noexcept;
        PinnedBuffer& operator=(PinnedBuffer&& other) noexcept;
        
        uint8_t* data() { return buffer_; }
        size_t size() const { return size_; }
        
    private:
        uint8_t* buffer_;
        size_t size_;
    };

private:
    RawDiskIOEngine& engine_;
    
    /**
     * @brief Scan a single window
     */
    void scan_window(uint64_t window_offset, size_t window_size,
                     std::function<void(const ScanResult&)> callback);
};

// ==================== LSNFilter ====================

/**
 * @brief LSN-based filtering for page selection
 * 
 * Filters pages based on Log Sequence Number to:
 * - Skip pages newer than a threshold (TRUNCATE recovery)
 * - Select pages within a time range
 * - Find pages from specific recovery points
 */
class LSNFilter {
public:
    /// @brief Filter configuration
    struct Config {
        std::optional<uint64_t> min_lsn;    ///< Minimum LSN (inclusive)
        std::optional<uint64_t> max_lsn;    ///< Maximum LSN (inclusive)
        bool exclude_newer = false;         ///< Exclude pages newer than max
        bool exclude_older = false;         ///< Exclude pages older than min
    };

    /**
     * @brief Check if LSN passes filter
     * 
     * @param lsn LSN value
     * @param config Filter configuration
     * @return True if LSN passes filter
     */
    static bool passes_filter(uint64_t lsn, const Config& config) {
        if (config.exclude_newer && config.max_lsn.has_value()) {
            if (lsn > config.max_lsn.value()) {
                return false;
            }
        }
        
        if (config.exclude_older && config.min_lsn.has_value()) {
            if (lsn < config.min_lsn.value()) {
                return false;
            }
        }
        
        if (config.min_lsn.has_value() && lsn < config.min_lsn.value()) {
            return false;
        }
        
        if (config.max_lsn.has_value() && lsn > config.max_lsn.value()) {
            return false;
        }
        
        return true;
    }

    /**
     * @brief Find LSN range in set of pages
     * 
     * @param pages Found pages
     * @return LSN range (min, max)
     */
    static std::pair<uint64_t, uint64_t> find_range(
        const std::vector<FoundPage>& pages) {
        if (pages.empty()) {
            return {0, 0};
        }
        
        uint64_t min_lsn = pages[0].lsn;
        uint64_t max_lsn = pages[0].lsn;
        
        for (const auto& page : pages) {
            if (page.lsn < min_lsn) min_lsn = page.lsn;
            if (page.lsn > max_lsn) max_lsn = page.lsn;
        }
        
        return {min_lsn, max_lsn};
    }
};

// ==================== InnoDBPageSignatures ====================

/**
 * @brief InnoDB page signature definitions
 * 
 * Contains all signature patterns for different MySQL versions and page types.
 */
class InnoDBPageSignatures {
public:
    /// @brief Base signature structure
    struct Signature {
        uint32_t offset;
        uint32_t size;
        uint8_t data[16];
        uint8_t mask[16];
        
        bool matches(const uint8_t* data, size_t size) const {
            if (offset + size > size) return false;
            for (uint32_t i = 0; i < size; ++i) {
                if (mask[i] && data[offset + i] != data[i]) {
                    return false;
                }
            }
            return true;
        }
    };

    /// @brief Full page signature
    struct FullSignature {
        std::vector<Signature> signatures;
        uint8_t min_pages;      ///< Minimum pages in chain
        uint8_t max_pages;      ///< Maximum pages in chain
    };

    /**
     * @brief Get signatures for MySQL 5.6
     */
    static std::vector<FullSignature> get_mysql_56_signatures();

    /**
     * @brief Get signatures for MySQL 8.0
     */
    static std::vector<FullSignature> get_mysql_80_signatures();

    /**
     * @brief Get all known signatures
     */
    static std::vector<FullSignature> get_all_signatures();
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_SIGNATURE_SCRAPER_HPP
