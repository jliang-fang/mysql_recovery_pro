/**
 * @file drop_table_recovery.hpp
 * @brief DROP TABLE Recovery Module
 *
 * Recovers dropped tables by scanning system tablespace (ibdata1/mysql.ibd)
 * for deleted metadata and finding orphaned index pages.
 *
 * Algorithm:
 * 1. Read system tablespace (ibdata1 or mysql.ibd)
 * 2. Find deleted records in system tables
 * 3. Extract table_id, index_id, column definitions
 * 4. Scan disk for orphaned INDEX pages with matching index_id
 * 5. Reconstruct table schema from recovered metadata
 * 6. Extract data from orphaned pages
 */

#ifndef DBRECOVERY_ENGINE_DROP_TABLE_RECOVERY_HPP
#define DBRECOVERY_ENGINE_DROP_TABLE_RECOVERY_HPP

#include "dbrecovery/engine/raw_disk_engine.hpp"
#include "dbrecovery/safety/safety_gate.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>
#include <cstdint>

namespace dbrecovery {
namespace engine {

// ==================== System Table Definitions ====================

/**
 * @brief System table entry (from SYS_TABLES/mysql.tables)
 */
struct SysTableEntry {
    uint64_t table_id;
    std::string schema_name;
    std::string table_name;
    uint32_t space_id;
    uint32_t n_cols;
    uint32_t type;          // Table flags
    uint32_t row_format;    // REDUNDANT, COMPACT, DYNAMIC, COMPRESSED
    bool deleted;           // Marked as deleted
    bool dropped;           // Actually dropped
};

/**
 * @brief System column entry (from SYS_COLUMNS/mysql.columns)
 */
struct SysColumnEntry {
    uint64_t table_id;
    std::string name;
    uint32_t pos;           // Column position
    uint32_t mtype;         // MySQL type
    uint32_t prtype;        // Precise type (includes charset, nullability)
    uint32_t len;           // Column length
    uint32_t collation_id;
    bool is_nullable;
    bool is_unsigned;
    bool is_auto_increment;
};

/**
 * @brief System index entry (from SYS_INDEXES/mysql.indexes)
 */
struct SysIndexEntry {
    uint64_t table_id;
    uint64_t index_id;
    std::string name;
    uint32_t type;          // PRIMARY, UNIQUE, MULTIPLE
    uint32_t space_id;
    uint32_t page_no;       // Root page number
    bool deleted;
};

/**
 * @brief Recovered table metadata
 */
struct RecoveredTableMetadata {
    uint64_t table_id;
    uint64_t primary_index_id;
    std::string schema_name;
    std::string table_name;
    catalog::TableSchema schema;
    std::vector<SysColumnEntry> columns;
    std::vector<SysIndexEntry> indexes;
    uint32_t space_id;
    uint32_t root_page_no;
    bool recovered_from_deleted;
    double confidence;
};

// ==================== System Tablespace Scanner ====================

/**
 * @brief Scans system tablespace for deleted table metadata
 *
 * Supports both MySQL 5.7 (ibdata1) and MySQL 8.0+ (mysql.ibd)
 */
class SystemTablespaceScanner {
public:
    /**
     * @brief Construct scanner
     * @param path Path to system tablespace (ibdata1 or mysql.ibd)
     * @param page_size Page size (default 16384)
     */
    explicit SystemTablespaceScanner(const std::string& path, uint32_t page_size = 16384);

    /**
     * @brief Scan for deleted tables
     * @return List of deleted table metadata
     */
    std::vector<RecoveredTableMetadata> scan_deleted_tables();

    /**
     * @brief Get all tables (including active)
     * @return All tables found
     */
    std::vector<SysTableEntry> get_all_tables();

    /**
     * @brief Get columns for table
     * @param table_id Table ID
     * @return Column definitions
     */
    std::vector<SysColumnEntry> get_columns(uint64_t table_id);

    /**
     * @brief Get indexes for table
     * @param table_id Table ID
     * @return Index definitions
     */
    std::vector<SysIndexEntry> get_indexes(uint64_t table_id);

    /**
     * @brief Detect MySQL version
     * @return Version string (5.6, 5.7, 8.0, etc.)
     */
    std::string detect_mysql_version();

    /**
     * @brief Check if file is MySQL 8.0+ format
     */
    bool is_mysql80_format();

    /**
     * @brief Get last error
     */
    const std::string& get_error() const { return error_; }

private:
    std::string path_;
    uint32_t page_size_;
    std::string error_;
    std::string mysql_version_;

    // Cached data
    std::vector<SysTableEntry> tables_;
    std::vector<SysColumnEntry> columns_;
    std::vector<SysIndexEntry> indexes_;

    // System table index IDs
    static constexpr uint64_t SYS_TABLES_INDEX_ID = 1;
    static constexpr uint64_t SYS_COLUMNS_INDEX_ID = 2;
    static constexpr uint64_t SYS_INDEXES_INDEX_ID = 3;
    static constexpr uint64_t SYS_FIELDS_INDEX_ID = 4;

    // MySQL 8.0 DD table index IDs (different from 5.7)
    static constexpr uint64_t DD_TABLES_INDEX_ID = 1;

    /**
     * @brief Parse SYS_TABLES index page (MySQL 5.7)
     */
    void parse_sys_tables_page(const uint8_t* data);

    /**
     * @brief Parse SYS_COLUMNS index page (MySQL 5.7)
     */
    void parse_sys_columns_page(const uint8_t* data);

    /**
     * @brief Parse SYS_INDEXES index page (MySQL 5.7)
     */
    void parse_sys_indexes_page(const uint8_t* data);

    /**
     * @brief Parse INDEX page for records
     */
    bool parse_index_page(const uint8_t* data, uint64_t index_id,
                          std::function<void(const std::map<std::string, std::string>&)> callback);

    /**
     * @brief Parse SYS_TABLES record (MySQL 5.7)
     */
    SysTableEntry parse_sys_tables_record(const std::map<std::string, std::string>& record);

    /**
     * @brief Parse SYS_COLUMNS record (MySQL 5.7)
     */
    SysColumnEntry parse_sys_columns_record(const std::map<std::string, std::string>& record);

    /**
     * @brief Parse SYS_INDEXES record (MySQL 5.7)
     */
    SysIndexEntry parse_sys_indexes_record(const std::map<std::string, std::string>& record);

    /**
     * @brief Parse DD tables record (MySQL 8.0+)
     */
    SysTableEntry parse_dd_tables_record(const std::map<std::string, std::string>& record);

    /**
     * @brief Parse DD columns record (MySQL 8.0+)
     */
    SysColumnEntry parse_dd_columns_record(const std::map<std::string, std::string>& record);

    /**
     * @brief Build table schema from system data
     */
    catalog::TableSchema build_schema(const SysTableEntry& table,
                                       const std::vector<SysColumnEntry>& cols);

    /**
     * @brief Parse SYS_TABLES page for deleted records (MySQL 5.7)
     */
    void parse_sys_tables_page_deleted(const uint8_t* data);

    /**
     * @brief Parse SYS_COLUMNS page for deleted records (MySQL 5.7)
     */
    void parse_sys_columns_page_deleted(const uint8_t* data);

    /**
     * @brief Parse SYS_INDEXES page for deleted records (MySQL 5.7)
     */
    void parse_sys_indexes_page_deleted(const uint8_t* data);

    /**
     * @brief Parse DD records from deleted list (MySQL 8.0+)
     */
    void parse_dd_records_deleted(const uint8_t* data, const std::string& table_name,
                                   std::map<uint64_t, std::map<std::string, std::string>>& records);

    /**
     * @brief Parse DD columns records from deleted list (MySQL 8.0+)
     */
    void parse_dd_records_deleted_columns(const uint8_t* data,
                                           std::map<uint64_t, std::vector<std::map<std::string, std::string>>>& columns_data);

    /**
     * @brief Parse DD indexes records from deleted list (MySQL 8.0+)
     */
    void parse_dd_records_deleted_indexes(const uint8_t* data,
                                           std::map<uint64_t, std::vector<std::map<std::string, std::string>>>& indexes_data);

    /**
     * @brief Build schema from DD column data (MySQL 8.0+)
     */
    catalog::TableSchema build_schema_from_dd(const std::string& table_name,
                                               const std::vector<SysColumnEntry>& cols);
};

// ==================== Orphaned Page Scanner ====================

/**
 * @brief Scans disk for orphaned INDEX pages
 *
 * Finds INDEX pages that belong to dropped tables by matching index_id
 */
class OrphanedPageScanner {
public:
    /**
     * @brief Found orphaned page
     */
    struct OrphanedPage {
        uint64_t disk_offset;
        uint64_t page_number;
        uint64_t index_id;
        uint32_t space_id;
        uint32_t page_size;
        uint64_t lsn;
        bool is_leaf;
        uint32_t record_count;
        std::vector<uint8_t> data;
    };

    /**
     * @brief Construct scanner
     * @param engine Raw disk engine
     */
    explicit OrphanedPageScanner(RawDiskIOEngine& engine);

    /**
     * @brief Scan for pages with specific index_id
     * @param index_id Index ID to search for
     * @param page_size Page size
     * @return Found pages
     */
    std::vector<OrphanedPage> scan_by_index_id(uint64_t index_id, uint32_t page_size = 16384);

    /**
     * @brief Scan for pages with any of the specified index_ids
     * @param index_ids Set of index IDs to search for
     * @param page_size Page size
     * @return Found pages grouped by index_id
     */
    std::map<uint64_t, std::vector<OrphanedPage>> scan_by_index_ids(
        const std::vector<uint64_t>& index_ids, uint32_t page_size = 16384);

    /**
     * @brief Scan entire disk for all orphaned INDEX pages
     * @param page_size Page size
     * @param callback Progress callback
     * @return All found INDEX pages
     */
    std::vector<OrphanedPage> scan_all(uint32_t page_size = 16384,
                                        std::function<void(double)> callback = nullptr);

private:
    RawDiskIOEngine& engine_;

    /**
     * @brief Check if page is INDEX page
     */
    static bool is_index_page(const uint8_t* data, uint32_t page_size);

    /**
     * @brief Extract index_id from INDEX page
     */
    static uint64_t extract_index_id(const uint8_t* data, uint32_t page_size);

    /**
     * @brief Check page checksum
     */
    static bool verify_checksum(const uint8_t* data, uint32_t page_size);
};

// ==================== DROP TABLE Recovery ====================

/**
 * @brief DROP TABLE Recovery Engine
 *
 * Complete recovery pipeline for dropped tables.
 */
class DropTableRecovery {
public:
    /**
     * @brief Recovery configuration
     */
    struct Config {
        std::string system_tablespace;  // ibdata1 or mysql.ibd path
        std::string data_directory;     // MySQL data directory
        std::string output_directory;   // Output for recovered data
        std::string device_path;        // Optional: raw device for deep scan
        std::string schema_file;        // Optional: JSON schema file path
        // Only decode these clustered index IDs when no system-tablespace
        // metadata survived; empty means "resolve from metadata" (and a
        // schema-driven scan then fails closed if the source is ambiguous).
        std::vector<uint64_t> target_index_ids;

        uint32_t page_size = 16384;
        bool scan_deleted_only = true;  // Only scan deleted records
        bool deep_scan = false;         // Scan entire disk for orphaned pages
        bool export_csv = true;
        bool export_sql = true;
        bool export_json = false;
        unsigned num_threads = 4;
    };

    /**
     * @brief Recovery result
     */
    struct Result {
        bool success;
        uint32_t tables_found;
        uint32_t tables_recovered;
        uint64_t total_records;
        std::vector<RecoveredTableMetadata> recovered_tables;
        std::map<uint64_t, uint64_t> records_per_table;  // table_id -> record_count
        std::vector<std::string> errors;
        double scan_time_seconds;
        double recovery_time_seconds;
    };

    /**
     * @brief Construct recovery engine
     */
    DropTableRecovery();

    /**
     * @brief Set configuration
     */
    void set_config(const Config& config);

    /**
     * @brief Transfer an already-authorized raw-device capability to this recovery.
     */
    void adopt_authorized_raw_device(safety::AuthorizedRawDevice&& device);

    /**
     * @brief Run recovery
     * @param progress Progress callback
     * @return Recovery result
     */
    Result recover(std::function<void(double, const std::string&)> progress = nullptr);

    /**
     * @brief Recover specific table by name
     * @param schema_name Schema name
     * @param table_name Table name
     * @return Recovery result for single table
     */
    Result recover_table(const std::string& schema_name, const std::string& table_name);

    /**
     * @brief Recover table by table_id
     * @param table_id Table ID
     * @return Recovery result
     */
    Result recover_by_table_id(uint64_t table_id);

    /**
     * @brief Get last error
     */
    const std::string& get_error() const { return error_; }

private:
    Config config_;
    std::string error_;
    std::unique_ptr<SystemTablespaceScanner> sys_scanner_;
    std::optional<safety::AuthorizedRawDevice> authorized_raw_device_;

    /**
     * @brief Extract records from orphaned pages
     */
    std::vector<std::map<std::string, std::string>> extract_records(
        const std::vector<OrphanedPageScanner::OrphanedPage>& pages,
        const catalog::TableSchema& schema);

    /**
     * @brief Export recovered data
     */
    bool export_data(const std::string& table_name,
                     const catalog::TableSchema& schema,
                     const std::vector<std::map<std::string, std::string>>& records);

    /**
     * @brief Scan data directory for .ibd files
     */
    std::vector<std::string> find_ibd_files();

    /**
     * @brief Try recovery from existing .ibd file
     */
    bool try_recover_from_ibd(const std::string& ibd_path,
                               const RecoveredTableMetadata& table,
                               std::vector<std::map<std::string, std::string>>& records);
};

// ==================== Utility Functions ====================

/**
 * @brief Get MySQL type name from type code
 */
std::string mysql_type_name(uint32_t mtype);

/**
 * @brief Get InnoDB type from MySQL type
 */
uint32_t mysql_to_innodb_type(uint32_t mysql_type);

/**
 * @brief Parse prtype to extract charset, nullability, etc.
 */
struct ParsedPrType {
    uint32_t charset_id;
    bool is_nullable;
    bool is_unsigned;
    bool is_binary;
    bool is_virtual;
    uint32_t base_type;
};

inline ParsedPrType parse_prtype(uint32_t prtype) {
    ParsedPrType result;
    result.charset_id = (prtype >> 16) & 0x7FFF;
    result.is_nullable = !(prtype & 256);  // DATA_NOT_NULL
    result.is_unsigned = (prtype & 512) != 0;  // DATA_UNSIGNED
    result.is_binary = (prtype & 1024) != 0;  // DATA_BINARY_TYPE
    result.is_virtual = (prtype & 8192) != 0;  // DATA_VIRTUAL
    result.base_type = prtype & 255;  // DATA_MYSQL_TYPE_MASK
    return result;
}

/**
 * @brief Get row format name from flags
 */
inline std::string row_format_name(uint32_t flags) {
    if (!(flags & 1)) return "REDUNDANT";
    else if (!(flags & 32)) return "COMPACT";
    else if ((flags & 30) >> 1) return "COMPRESSED";
    else return "DYNAMIC";
}

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_DROP_TABLE_RECOVERY_HPP
