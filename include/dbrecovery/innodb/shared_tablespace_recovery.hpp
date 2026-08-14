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

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/sdi_extractor.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

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

class SharedTablespaceRecovery {
public:
    explicit SharedTablespaceRecovery(size_t page_size = 16384);

    void set_tablespace_path(const std::string& path);
    void set_output_dir(const std::string& dir);
    const SharedTablespaceStats& get_stats() const;

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

    /// Populate page_cache_ from the tablespace file (idempotent)
    void ensure_cache_populated();

    size_t page_size_;
    std::string ts_path_;
    std::string output_dir_;
    SharedTablespaceStats stats_;

    /// Page cache: populated once on first scan, reused across all methods
    std::map<uint32_t, std::vector<uint8_t>> page_cache_;
    bool cache_populated_ = false;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SHARED_TABLESPACE_RECOVERY_HPP