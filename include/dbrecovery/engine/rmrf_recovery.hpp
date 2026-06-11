/**
 * @file rmrf_recovery.hpp
 * @brief rm -rf Recovery Module (TestDisk + PhotoRec Style)
 * 
 * Implements file system-independent recovery for completely deleted .ibd/.frm files.
 * Combines TestDisk-style inode reconstruction with PhotoRec-style file carving.
 * 
 * Scenarios handled:
 * 1. rm -f *.ibd - File deleted, inode removed
 * 2. rm -rf /var/lib/mysql/db - Complete directory deletion
 * 3. rm -f .ibd + .frm - Both metadata files deleted
 * 
 * Algorithm:
 * - TestDisk mode: Analyze filesystem journal/superblock to reconstruct inode chain
 * - PhotoRec mode: Identify IBD signatures and carve files from raw disk
 * - Schema inference: Reconstruct table structure from recovered data
 */

#ifndef DBRECOVERY_ENGINE_RMRF_RECOVERY_HPP
#define DBRECOVERY_ENGINE_RMRF_RECOVERY_HPP

#include "dbrecovery/engine/raw_disk_engine.hpp"
#include "dbrecovery/engine/signature_scraper.hpp"
#include "dbrecovery/engine/truncate_recovery.hpp"
#include "dbrecovery/innodb/schema_inferrer.hpp"
#include <string>
#include <vector>
#include <map>
#include <filesystem>

namespace dbrecovery {
namespace engine {

// ==================== Found File Information ====================

/**
 * @brief Information about recovered file
 */
struct RecoveredFile {
    std::string path;
    uint64_t disk_offset;
    uint64_t file_size;
    uint32_t page_size;
    uint32_t num_pages;
    uint32_t index_pages;
    uint32_t sdi_pages;
    
    // File system metadata (if available)
    struct {
        uint64_t inode;
        time_t creation_time;
        time_t modification_time;
        time_t access_time;
        uint32_t mode;
        uint32_t uid;
        uint32_t gid;
    } fs_info;
    
    // Detection method
    enum class DetectionMethod {
        TESTDISK_INODE,   // File system reconstruction
        PHOTOREC_CARVE,   // Signature-based carving
        UFSD_SCAN,        // File system specific scan
        HYBRID            // Multiple methods combined
    } method;
    
    std::vector<uint8_t> data;
    bool verified;
    std::string error_message;
};

// ==================== Inode Reconstruction ====================

/**
 * @brief Inode information for file system reconstruction
 */
struct InodeInfo {
    uint64_t inode_number;
    uint64_t block_count;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    time_t ctime;
    time_t mtime;
    time_t atime;
    
    // File type
    enum class Type {
        REGULAR,
        DIRECTORY,
        SYMBOLIC_LINK,
        DEVICE,
        SOCKET,
        FIFO
    } type;
    
    // Block list
    std::vector<uint64_t> direct_blocks;
    std::vector<uint64_t> indirect_blocks;
    std::vector<uint64_t> double_indirect_blocks;
    
    // Reference count
    uint32_t hard_links;
    
    // Flags
    bool deleted;
    bool reconstructed;
};

// ==================== File System Superblock ====================

/**
 * @brief File system superblock information
 */
struct FileSystemInfo {
    std::string type;           // ext4, xfs, btrfs, etc.
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint32_t block_size;
    uint64_t Inode_count;
    uint64_t free_Inodes;
    
    // Journal info (for Journaling FS)
    struct {
        uint64_t start_block;
        uint32_t size;
        bool has_journal;
    } journal;
    
    // Metadata locations
    struct {
        uint64_t Inode_bitmap;
        uint64_t block_bitmap;
        uint64_t superblock;
        uint64_t group_descriptors;
    } metadata;
};

// ==================== RmrfRecovery ====================

/**
 * @brief rm -rf Recovery Module (TestDisk + PhotoRec)
 * 
 * Combines multiple recovery strategies for maximum recovery rate:
 * 1. TestDisk-style: File system structure analysis
 * 2. PhotoRec-style: Signature-based file carving
 * 3. Hybrid: Combined approaches
 */
class RmrfRecovery {
public:
    /// @brief Recovery configuration
    struct Config {
        // Search parameters
        uint64_t max_scan_size = 0;           // 0 = full device
        uint32_t min_file_size = 16 * 1024;   // 16KB minimum
        uint32_t max_file_size = 0;           // 0 = no limit
        
        // Detection methods
        bool enable_testdisk = true;          // File system reconstruction
        bool enable_photorec = true;          // Signature carving
        bool enable_hybrid = true;            // Combined approach
        
        // Page filters
        uint32_t min_pages = 2;
        uint32_t max_pages = 0;
        std::vector<uint32_t> page_sizes = {4096, 8192, 16384, 32768, 65536};
        
        // Schema inference
        bool auto_infer_schema = true;
        double schema_confidence_threshold = 0.5;
        
        // Output
        std::string output_dir;
        bool export_ibd = true;               // Export recovered .ibd files
        bool export_json = true;
        bool export_csv = true;
        bool export_sql = true;
    };

    /// @brief Recovery statistics
    struct RecoveryStats {
        uint64_t total_bytes_scanned;
        uint64_t files_candidates;
        uint32_t files_detected;
        uint32_t files_recovered;
        uint32_t files_failed;
        
        // By detection method
        uint32_t testdisk_found;
        uint32_t photorec_found;
        uint32_t hybrid_found;
        
        // Page statistics
        uint64_t total_pages_found;
        uint64_t valid_ibd_pages;
        uint64_t index_pages;
        uint64_t sdi_pages;
        
        // Records
        uint64_t total_recordsRecovered;
        uint64_t duplicate_records;
        
        // Time
        double scan_time_seconds;
        double recovery_time_seconds;
    };

    /**
     * @brief Construct a new RmrfRecovery
     * 
     * @param engine Disk engine for scanning
     */
    explicit RmrfRecovery(RawDiskIOEngine& engine);

    /// @brief Destroy the RmrfRecovery
    ~RmrfRecovery();

    /**
     * @brief Run full recovery
     * 
     * @param config Recovery configuration
     * @param callback Progress callback
     * @return Recovery result
     */
    TruncateRecoveryResult recover(
        const Config& config,
        std::function<void(double)> callback = nullptr);

    /**
     * @brief Scan for files using TestDisk method
     * 
     * Analyzes file system structures to find deleted files.
     * 
     * @param device_path Path to device
     * @param min_size Minimum file size
     * @return Found files
     */
    std::vector<RecoveredFile> recover_by_testdisk(
        const std::string& device_path,
        uint64_t min_size = 16384);

    /**
     * @brief Scan for files using PhotoRec method
     * 
     * Identifies IBD signatures and carves files from raw disk.
     * 
     * @param device_path Path to device
     * @param min_pages Minimum pages per file
     * @return Found files
     */
    std::vector<RecoveredFile> recover_by_photorec(
        const std::string& device_path,
        uint32_t min_pages = 2);

    /**
     * @brief Hybrid recovery (TestDisk + PhotoRec)
     * 
     * Combines both approaches for maximum recovery rate.
     * 
     * @param device_path Path to device
     * @param config Recovery configuration
     * @return Found files
     */
    std::vector<RecoveredFile> recover_hybrid(
        const std::string& device_path,
        const Config& config);

    /**
     * @brief Parse file system superblock
     * 
     * @param device_path Path to device
     * @param fs_type File system type
     * @return File system information
     */
    static std::optional<FileSystemInfo> parse_superblock(
        const std::string& device_path,
        const std::string& fs_type = "ext4");

    /**
     * @brief Reconstruct deleted Inode
     * 
     * @param device_path Path to device
     * @param Inode_number Inode number
     * @return Inode information
     */
    static std::optional<InodeInfo> reconstruct_deleted_Inode(
        const std::string& device_path,
        uint64_t Inode_number);

    /**
     * @brief Export recovered files
     * 
     * @param files Files to export
     * @param output_dir Output directory
     * @param stats Statistics output
     */
    void export_files(
        const std::vector<RecoveredFile>& files,
        const std::string& output_dir,
        RecoveryStats& stats);

    /**
     * @brief Get statistics
     * 
     * @return Current statistics
     */
    const RecoveryStats& get_stats() const { return stats_; }

    /**
     * @brief Get found files
     * 
     * @return Found files
     */
    const std::vector<RecoveredFile>& get_found_files() const {
        return found_files_;
    }

    /**
     * @brief Recover schema from file
     * 
     * @param file Recovered file
     * @return Recovered schema
     */
    static catalog::TableSchema recover_schema(
        const RecoveredFile& file);

    /**
     * @brief Infer schema from recovered records
     * 
     * @param records Recovered records
     * @return Inferred schema
     */
    static catalog::TableSchema infer_schema(
        const std::vector<std::map<std::string, std::string>>& records);

    /**
     * @brief Create IBD file from recovered data
     * 
     * @param data Raw IBD data
     * @param page_size Page size
     * @param output_path Output path
     * @return True if successful
     */
    static bool create_ibd_file(
        const std::vector<uint8_t>& data,
        uint32_t page_size,
        const std::string& output_path);

    /**
     * @brief Verify recovered file
     * 
     * @param file File to verify
     * @return True if valid
     */
    static bool verify_recovered_file(const RecoveredFile& file);

private:
    RawDiskIOEngine& engine_;
    RecoveryStats stats_;
    std::vector<RecoveredFile> found_files_;
    
    std::unique_ptr<innodb::SchemaInferrer> schema_inferrer_;
    
    /**
     * @brief Detect file system type
     */
    static std::string detect_filesystem(const std::string& device_path);

    /**
     * @brief Find IBD file boundaries in raw data
     */
    std::vector<std::pair<uint64_t, uint64_t>> find_ibd_boundaries(
        const std::vector<uint8_t>& data,
        uint32_t page_size);

    /**
     * @brief Import records from recovered file
     */
    std::vector<std::map<std::string, std::string>> import_records(
        const RecoveredFile& file,
        const catalog::TableSchema& schema);

    /**
     * @brief Apply PhotoRec-style carving to disk
     */
    std::vector<RecoveredFile> photorec_carve(
        uint64_t disk_offset,
        uint64_t disk_size,
        uint32_t min_pages);

    /**
     * @brief Process found IBD block
     */
    RecoveredFile process_ibd_block(
        const FoundPage& page,
        const std::vector<FoundPage>& related_pages,
        const SchemaTemplate& template_);
};

// ==================== IBD File Carver ====================

/**
 * @brief PhotoRec-style IBD file carver
 * 
 * Scans raw disk for IBD file signatures and carves complete files.
 */
class IbdFileCarver {
public:
    /// @brief Found file chunk
    struct FileChunk {
        uint64_t offset;
        uint32_t page_size;
        uint32_t page_number;
        uint32_t space_id;
        std::vector<uint8_t> data;
    };

    /// @brief Carved file
    struct CarvedFile {
        uint64_t disk_offset;
        uint64_t file_size;
        uint32_t page_size;
        uint32_t space_id;
        uint32_t num_pages;
        uint32_t index_pages;
        
        std::vector<uint8_t> data;
        std::vector<uint32_t> page_numbers;
    };

    /**
     * @brief Carve files from disk
     * 
     * @param engine Disk engine
     * @param page_sizes Page sizes to scan for
     * @param min_pages Minimum pages per file
     * @return Carved files
     */
    static std::vector<CarvedFile> carve(
        RawDiskIOEngine& engine,
        const std::vector<uint32_t>& page_sizes,
        uint32_t min_pages = 2);

    /**
     * @brief Aggregate chunks into files
     * 
     * @param chunks Found chunks
     * @param page_size Page size
     * @param min_pages Minimum pages
     * @return Carved files
     */
    static std::vector<CarvedFile> aggregate_chunks(
        const std::vector<FileChunk>& chunks,
        uint32_t page_size,
        uint32_t min_pages = 2);

    /**
     * @brief Find IBD signature in data
     * 
     * @param data Raw data
     * @param size Data size
     * @param page_size Page size
     * @return Found signatures (offsets)
     */
    static std::vector<uint64_t> find_signatures(
        const uint8_t* data,
        size_t size,
        uint32_t page_size);
};

// ==================== Schema Recovery ====================

/**
 * @brief Schema recovery from recovered data
 * 
 * Attempts to reconstruct table structure when .frm is lost.
 */
class SchemaRecovery {
public:
    /// @brief Recovery confidence levels
    enum class Confidence {
        HIGH = 3,      // Schema recovered from SDI or .frm
        MEDIUM = 2,    // Schema inferred from data patterns
        LOW = 1,       // Generic schema generated
        NONE = 0       // No schema recovered
    };

    /// @brief Recovery result
    struct Result {
        catalog::TableSchema schema;
        Confidence confidence;
        std::vector<std::string> warnings;
        std::string recovery_method;
    };

    /**
     * @brief Recover schema from SDI
     * 
     * @param data IBD file data
     * @param page_size Page size
     * @return Recovery result
     */
    static Result recover_from_sdi(
        const std::vector<uint8_t>& data,
        uint32_t page_size);

    /**
     * @brief Infer schema from records
     * 
     * @param records Recovered records
     * @param table_name Expected table name
     * @return Recovery result
     */
    static Result infer_from_records(
        const std::vector<std::map<std::string, std::string>>& records,
        const std::string& table_name = "recovered_table");

    /**
     * @brief Generate generic schema
     * 
     * @param table_name Table name
     * @param column_count Number of columns
     * @return Schema
     */
    static catalog::TableSchema generate_generic_schema(
        const std::string& table_name,
        uint32_t column_count = 4);

    /**
     * @brief Get confidence for column type
     * 
     * @param values Column values
     * @return Confidence level
     */
    static Confidence get_type_confidence(const std::vector<std::string>& values);
};

// ==================== Multi-Stage Recovery ====================

/**
 * @brief Complete rm -rf recovery pipeline
 * 
 * Implements multi-stage recovery with verification at each stage.
 */
class RmrfRecoveryPipeline {
public:
    /// @brief Pipeline configuration
    struct Config {
        std::string device_path;
        std::string output_dir;
        uint32_t min_pages = 2;
        unsigned num_threads = 4;
        bool verbose = true;
    };

    /// @brief Pipeline result
    struct Result {
        bool success;
        uint32_t files_recovered;
        uint64_t total_bytes_recovered;
        std::vector<RecoveredFile> files;
        RmrfRecovery::RecoveryStats stats;
    };

    /**
     * @brief Run complete recovery pipeline
     * 
     * @param config Configuration
     * @param callback Progress callback
     * @return Pipeline result
     */
    static Result run(
        const Config& config,
        std::function<void(double)> callback = nullptr);

private:
    static std::vector<RecoveredFile> stage_photorec_scan(
        const std::string& device_path,
        uint32_t min_pages);

    static std::vector<RecoveredFile> stage_testdisk_scan(
        const std::string& device_path);

    static std::vector<RecoveredFile> stage_hybrid_verification(
        const std::vector<RecoveredFile>& candidates);

    static void stage_schema_recovery(
        std::vector<RecoveredFile>& files);

    static void stage_export(
        const std::vector<RecoveredFile>& files,
        const std::string& output_dir);
};

// ====================辅助 Configuration ====================

/**
 * @brief Recovery configuration helpers
 */
class RmrfConfig {
public:
    /// @brief Create default recovery configuration
    static RmrfRecovery::Config default_config() {
        RmrfRecovery::Config config;
        config.enable_testdisk = true;
        config.enable_photorec = true;
        config.enable_hybrid = true;
        config.min_pages = 2;
        config.auto_infer_schema = true;
        return config;
    }

    /// @brief Create strict recovery configuration
    static RmrfRecovery::Config strict_config() {
        RmrfRecovery::Config config = default_config();
        config.min_pages = 5;
        config.schema_confidence_threshold = 0.8;
        config.export_ibd = true;
        return config;
    }

    /// @brief Create fast recovery configuration
    static RmrfRecovery::Config fast_config() {
        RmrfRecovery::Config config = default_config();
        config.enable_testdisk = false;
        config.page_sizes = {16384};  // Only scan 16KB pages
        config.auto_infer_schema = false;
        return config;
    }
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_RMRF_RECOVERY_HPP
