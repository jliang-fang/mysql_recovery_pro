#ifndef DBRECOVERY_INNODB_SHARED_TABLESPACE_RECOVERY_HPP
#define DBRECOVERY_INNODB_SHARED_TABLESPACE_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <algorithm>
#include <set>
#include <sstream>
#include <iomanip>
#include <memory>
#include <fstream>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/sdi_extractor.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Page Bundle Types (Phase 5e) ====================

/// @brief Per-page entry in the scan manifest
struct PageBundleEntry {
    uint32_t page_no = 0;
    uint32_t space_id = 0;
    uint16_t page_type = 0;
    std::string page_type_name;
    bool checksum_valid = false;
    ChecksumAlgo checksum_algorithm = ChecksumAlgo::NONE;
    bool identity_matched = false;  // space_id matches expected filter
};

/// @brief Importability verdict for a page bundle
enum class ImportabilityVerdict : uint8_t {
    UNKNOWN = 0,
    IMPORTABLE,          ///< All pages valid, no holes, no mismatches
    HAS_HOLES,           ///< Missing page numbers in sequence
    IDENTITY_MISMATCH,   ///< Space_id mismatches detected
    CHECKSUM_FAILURES,   ///< Checksum validation failures
    MULTIPLE_SPACES,     ///< Pages from multiple space_ids found
    UNVERIFIABLE         ///< Cannot verify (no checksum info)
};

/// @brief Page bundle manifest (Phase 5e)
struct PageBundleManifest {
    std::vector<PageBundleEntry> entries;
    uint64_t total_file_pages = 0;
    uint64_t pages_scanned = 0;
    uint64_t pages_with_valid_checksum = 0;
    uint64_t pages_with_invalid_checksum = 0;
    uint64_t pages_identity_matched = 0;
    uint64_t pages_identity_mismatched = 0;
    uint64_t holes_found = 0;
    uint64_t mismatches_found = 0;
    uint64_t file_size_bytes = 0;
    uint32_t page_size = 0;
    ImportabilityVerdict verdict = ImportabilityVerdict::UNKNOWN;
    std::map<uint32_t, uint64_t> pages_per_space;

    std::string verdict_string() const;
    std::string to_json() const;
};

// ==================== Scan Configuration ====================

struct SharedTablespaceScanConfig {
    uint64_t max_memory_bytes = 64ULL * 1024 * 1024;  // 64 MB default
    bool verify_checksums = true;
    uint32_t filter_space_id = 0;  // 0 = all spaces
};

// ==================== Legacy Stats (backward compatible) ====================

struct SharedTablespaceStats {
    uint64_t total_pages_scanned = 0;
    uint64_t data_pages_found = 0;
    uint64_t sdi_pages_found = 0;
    uint64_t tables_found = 0;
    uint64_t records_recovered = 0;
    double recovery_time_ms = 0.0;
};

struct TableIdentity {
    std::string schema_name;
    std::string table_name;
    uint32_t space_id = 0;
    uint64_t table_id = 0;
    uint64_t estimated_rows = 0;
};

// ==================== SharedTablespaceRecovery ====================

class SharedTablespaceRecovery {
public:
    explicit SharedTablespaceRecovery(size_t page_size = 16384);

    void set_tablespace_path(const std::string& path);
    void set_output_dir(const std::string& dir);
    void set_scan_config(const SharedTablespaceScanConfig& config);
    const SharedTablespaceStats& get_stats() const;

    // Phase 5e: Streaming scan returns manifest (no full page cache)
    PageBundleManifest scan_pages(uint32_t filter_space_id = 0);

    // Legacy API methods (backward compatible, use streaming internally)
    std::vector<TableIdentity> list_tables();
    bool extract_table(uint32_t target_space_id, const std::string& output_ibd_path);
    std::vector<std::map<std::string, std::string>> recover_records(
        uint32_t target_space_id, const catalog::TableSchema& schema);

private:
    struct SDIEntry {
        std::string schema_name;
        std::string table_name;
        uint32_t space_id;
        uint64_t table_id;
    };

    std::vector<SDIEntry> extract_sdi_entries();

    // Phase 5e: Read a single page from file (streaming, no cache)
    bool read_page(std::ifstream& file, uint32_t page_no, std::vector<uint8_t>& page_buf) const;

    size_t page_size_;
    std::string ts_path_;
    std::string output_dir_;
    SharedTablespaceStats stats_;
    SharedTablespaceScanConfig scan_config_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SHARED_TABLESPACE_RECOVERY_HPP