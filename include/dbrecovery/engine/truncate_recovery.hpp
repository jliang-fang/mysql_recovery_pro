/**
 * @file truncate_recovery.hpp
 * @brief TRUNCATE TABLE Recovery Module
 * 
 * Implements ibd2sql-style template matching for recovering data after TRUNCATE TABLE.
 * When a table is TRUNCATED, the .ibd file is recreated but the old data pages may
 * still exist on disk until overwritten.
 * 
 * Algorithm:
 * 1. Use known table structure as "search template"
 * 2. Scan disk for pages matching template signature
 * 3. Filter pages by LSN (keep pages older than TRUNCATE)
 * 4. Reconstruct B+Tree from filtered pages
 * 5. Extract records and export to SQL
 */

#ifndef DBRECOVERY_ENGINE_TRUNCATE_RECOVERY_HPP
#define DBRECOVERY_ENGINE_TRUNCATE_RECOVERY_HPP

#include "dbrecovery/engine/raw_disk_engine.hpp"
#include "dbrecovery/engine/signature_scraper.hpp"
#include "dbrecovery/innodb/heuristic_parser.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <optional>

namespace dbrecovery {
namespace engine {

// ==================== TRUNCATE Recovery Result ====================

/**
 * @brief Result of TRUNCATE recovery operation
 */
struct TruncateRecoveryResult {
    bool success;
    uint64_t bytes_scanned;
    uint64_t pages_found;
    uint64_t pages_validated;
    uint64_t recordsRecovered;
    uint64_t data_size_bytes;
    
    std::vector<std::map<std::string, std::string>> records;
    std::vector<FoundPage> found_pages;
    
    // B+Tree reconstruction information
    uint32_t chain_count;
    uint32_t root_page;
    
    // Confidence score (0-100)
    uint8_t confidence;
    
    std::string error_message;
    double elapsed_seconds;
};

// ==================== TRUNCATE Recovery Statistics ====================

/**
 * @brief Statistics for TRUNCATE recovery operation
 */
struct TruncateStats {
    uint64_t total_bytes_scanned;
    uint64_t pages_found;
    uint64_t pages_with_valid_signature;
    uint64_t pages_with_valid_checksum;
    uint64_t pages_filtered_by_lsn;
    uint64_t pages_rejected_by_template;
    uint64_t recordsRecovered_from_pages;
    uint64_t duplicate_records_filtered;
    
    // B+Tree stats
    uint32_t chains_found;
    uint32_t chains_reconstructed;
    uint32_t chains_incomplete;
    
    // LSN range
    uint64_t min_lsn;
    uint64_t max_lsn;
    
    double scan_time_seconds;
    double reconstruction_time_seconds;
    double recovery_time_seconds;
};

// ==================== TruncateRecovery ====================

/**
 * @brief TRUNCATE Recovery Module
 * 
 * Implements ibd2sql-style recovery for TRUNCATE TABLE scenarios.
 * Uses known table structure as template to find and validate data pages.
 */
class TruncateRecovery {
public:
    /// @brief Recovery configuration
    struct Config {
        // Template matching parameters
        SchemaTemplate template_;             // Known table structure
        uint32_t template_tolerance = 10;     // Allow % variation in record count
        
        // LSN filtering
        std::optional<uint64_t> truncate_lsn; // LSN at time of TRUNCATE
        bool use_truncate_lsn = true;         // Filter pages by LSN
        
        // Page filtering
        uint32_t min_pages_per_chain = 2;
        uint32_t max_pages_per_chain = 10000;
        
        // Validation
        bool validate_checksums = true;
        bool validate_schema = true;
        
        // Recovery mode
        bool use_heuristic_fallback = true;   // Fallback to heuristic if B+Tree fails
        
        // Output
        std::string output_format = "SQL";    // SQL, JSON, CSV
    };

    /**
     * @brief Construct a new TruncateRecovery
     * 
     * @param engine Disk engine for scanning
     * @param template_ Known table structure
     */
    TruncateRecovery(RawDiskIOEngine& engine, const SchemaTemplate& template_);

    /// @brief Destroy the TruncateRecovery
    ~TruncateRecovery();

    /**
     * @brief Recovery using disk scan (ibd2sql-style)
     * 
     * @param config Recovery configuration
     * @param callback Callback for progress
     * @return Recovery result
     */
    TruncateRecoveryResult recover_from_disk(
        const Config& config,
        std::function<void(double)> callback = nullptr);

    /**
     * @brief Recovery from existing IBD file with LSN filtering
     * 
     * @param ibd_path Path to the (potentially corrupted) IBD file
     * @param schema Table schema
     * @param max_lsn Maximum LSN to include
     * @return Recovery result
     */
    TruncateRecoveryResult recover_from_ibd_file(
        const std::string& ibd_path,
        const catalog::TableSchema& schema,
        std::optional<uint64_t> max_lsn = std::nullopt);

    /**
     * @brief Find and validate data pages matching template
     * 
     * @param config Scan configuration
     * @param callback Callback for each found page
     * @return Statistics
     */
    TruncateStats scan_for_matching_pages(
        const SignatureBasedScraper::ScanConfig& config,
        std::function<void(const FoundPage&)> callback);

    /**
     * @brief Verify page matches schema template
     * 
     * @param page Found page
     * @param template_ Schema template
     * @return True if page matches template
     */
    static bool verify_page_matches_template(
        const FoundPage& page,
        const SchemaTemplate& template_);

    /**
     * @brief Extract records from pages using template
     * 
     * @param pages Found pages
     * @param template_ Schema template
     * @param schema MySQL table schema
     * @return Extracted records
     */
    static std::vector<std::map<std::string, std::string>> extract_records(
        const std::vector<FoundPage>& pages,
        const SchemaTemplate& template_,
        const catalog::TableSchema& schema);

    /**
     * @brief Get statistics
     * 
     * @return Current statistics
     */
    const TruncateStats& get_stats() const { return stats_; }

    /**
     * @brief Get found pages
     * 
     * @return Found pages
     */
    const std::vector<FoundPage>& get_found_pages() const {
        return found_pages_;
    }

private:
    RawDiskIOEngine& engine_;
    SchemaTemplate template_;
    TruncateStats stats_;
    std::vector<FoundPage> found_pages_;
    
    SignatureBasedScraper scraper_;
    
    /**
     * @brief Scan disk for pages matching template signature
     */
    std::vector<FoundPage> scan_with_template_signature(
        const Config& config,
        std::function<void(double)> callback);

    /**
     * @brief Filter pages by LSN
     */
    std::vector<FoundPage> filter_pages_by_lsn(
        const std::vector<FoundPage>& pages,
        uint64_t truncate_lsn);

    /**
     * @brief Reconstruct B+Tree from validated pages
     */
    std::vector<FoundPage> reconstruct_btree(
        const std::vector<FoundPage>& pages,
        Config& config);

    /**
     * @brief Resolve duplicate records
     */
    std::vector<std::map<std::string, std::string>> deduplicate_records(
        const std::vector<std::map<std::string, std::string>>& records);
};

// ==================== Im计算器 Recovery ====================

/**
 * @brief Recovery after IM Calculator 损坏
 * 
 * Handles recovery when only logical records remain (header corrupted).
 * Uses data pattern analysis to reconstruct table structure.
 */
class HeuristicTruncateRecovery {
public:
    /// @brief Recovery result
    struct Result {
        bool success;
        uint64_t recordsRecovered;
        std::vector<std::map<std::string, std::string>> records;
        catalog::TableSchema inferred_schema;
        std::vector<uint32_t> page_numbers;
        
        double confidence;
        double elapsed_seconds;
    };

    /**
     * @brief Recovery from IBD file with corrupted header
     * 
     * @param ibd_path Path to corrupted IBD file
     * @param page_size Page size
     * @return Recovery result
     */
    static Result recover(
        const std::string& ibd_path,
        uint32_t page_size = 16384);

    /**
     * @brief Infer schema from recovered records
     * 
     * @param records Recovered records
     * @param table_name Expected table name
     * @return Inferred schema
     */
    static catalog::TableSchema infer_schema(
        const std::vector<std::map<std::string, std::string>>& records,
        const std::string& table_name = "recovered_table");
};

// ==================== MySQL56TruncateRecovery ====================

/**
 * @brief MySQL 5.6 specific TRUNCATE recovery
 * 
 * Implements recovery for MySQL 5.6's different page format and LSN handling.
 */
class MySQL56TruncateRecovery {
public:
    /// @brief MySQL 5.6 specific page header
    struct PageHeader56 {
        uint32_t space_id;
        uint32_t page_number;
        uint16_t page_type;
        uint64_t lsn;
        uint32_t prev_page;
        uint32_t next_page;
        uint16_t heap_top;
        uint16_t n_records;
    };

    /// @brief Recovery configuration
    struct Config {
        uint32_t page_size = 16384;
        std::optional<uint64_t> max_lsn;
        bool use_checksum = true;
        bool use_chain_validation = true;
        unsigned num_threads = 4;
    };

    /**
     * @brief Recovery from disk
     * 
     * @param engine Disk engine
     * @param table_name Table name
     * @param columnDefinitions Column definitions
     * @param config Configuration
     * @return Recovery result
     */
    static TruncateRecoveryResult recover_from_disk(
        RawDiskIOEngine& engine,
        const std::string& table_name,
        const std::vector<std::pair<std::string, uint8_t>>& columnDefinitions,
        const Config& config);

    /**
     * @brief Parse MySQL 5.6 page header
     * 
     * @param data Page data
     * @param page_size Page size
     * @return Parsed header
     */
    static std::optional<PageHeader56> parse_page_header(
        const uint8_t* data,
        uint32_t page_size);

    /**
     * @brief Validate page against MySQL 5.6 criteria
     * 
     * @param page Page to validate
     * @param template_ Schema template
     * @return True if valid
     */
    static bool validate_page(
        const FoundPage& page,
        const SchemaTemplate& template_);
};

// ==================== Recovery Pipeline ====================

/**
 * @brief Complete recovery pipeline for TRUNCATE scenarios
 * 
 * Implements multi-stage recovery:
 * 1. Quick signature scan
 * 2. LSN filtering
 * 3. B+Tree reconstruction
 * 4. Record extraction
 * 5. Deduplication
 * 6. Completion
 */
class TruncateRecoveryPipeline {
public:
    /// @brief Pipeline configuration
    struct Config {
        SchemaTemplate template_;
        std::optional<uint64_t> max_lsn;
        uint32_t min_pages = 2;
        uint32_t max_pages = 0;
        unsigned num_threads = 4;
        std::string output_format = "SQL";
    };

    /**
     * @brief Run full recovery pipeline
     * 
     * @param engine Disk engine
     * @param config Configuration
     * @param callback Progress callback
     * @return Final recovery result
     */
    static TruncateRecoveryResult run(
        RawDiskIOEngine& engine,
        const Config& config,
        std::function<void(int stage, double progress)> callback = nullptr);

    /// @brief Pipeline stages
    enum class Stage {
        SIGNATURE_SCAN = 1,
        LSN_FILTERING,
        BTREE_RECONSTRUCTION,
        RECORD_EXTRACTION,
        DEDUPLICATION,
        COMPLETION
    };

private:
    static std::vector<FoundPage> stage_signature_scan(
        RawDiskIOEngine& engine,
        const Config& config);

    static std::vector<FoundPage> stage_lsn_filtering(
        const std::vector<FoundPage>& pages,
        const Config& config);

    static std::vector<FoundPage> stage_btree_reconstruction(
        const std::vector<FoundPage>& pages,
        const Config& config);

    static std::vector<std::map<std::string, std::string>> stage_record_extraction(
        const std::vector<FoundPage>& pages,
        const SchemaTemplate& template_,
        const catalog::TableSchema& schema);

    static std::vector<std::map<std::string, std::string>> stage_deduplication(
        const std::vector<std::map<std::string, std::string>>& records);
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_TRUNCATE_RECOVERY_HPP
