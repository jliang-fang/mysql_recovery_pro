#ifndef DBRECOVERY_ENTERPRISE_PARTITION_RECOVERY_HPP
#define DBRECOVERY_ENTERPRISE_PARTITION_RECOVERY_HPP

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <filesystem>
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace enterprise {

struct PartitionInfo {
    std::string partition_name;
    std::string partition_method;   // RANGE/LIST/HASH/KEY
    std::string partition_expression;
    std::vector<std::string> partition_values;
    uint32_t space_id = 0;
    std::string ibd_file_path;
};

struct PartitionRecoveryResult {
    std::string partition_name;
    uint64_t records_recovered = 0;
    uint64_t records_failed = 0;
    double recovery_rate = 0.0;
    bool success = false;
    std::string error;
};

class PartitionRecovery {
public:
    // Detect partitions from data directory + table name pattern
    std::vector<PartitionInfo> detect_partitions(
        const std::string& data_dir,
        const std::string& table_name);

    // Recover a single partition
    PartitionRecoveryResult recover_partition(
        const PartitionInfo& partition,
        const catalog::TableSchema& schema);

    // Aggregate all partitions into one output
    std::vector<PartitionRecoveryResult> recover_all_partitions(
        const std::vector<PartitionInfo>& partitions,
        const catalog::TableSchema& schema);

private:
    // Match file naming pattern: table#P#p0.ibd, table#P#p1.ibd, etc.
    std::vector<std::filesystem::path> find_partition_files(
        const std::string& data_dir,
        const std::string& table_name);
};

struct SharedTableInfo {
    uint32_t space_id;
    std::string table_name;
    std::string table_schema;
};

class SharedTablespaceRecovery {
public:
    // List all tables in a shared tablespace (ibdata1)
    std::vector<SharedTableInfo> list_tables(
        const std::string& shared_tablespace_path);

    // Extract a specific table's data from shared tablespace
    PartitionRecoveryResult extract_table(
        const std::string& shared_tablespace_path,
        uint32_t target_space_id,
        const catalog::TableSchema& schema);

private:
    // Scan SDI pages in ibdata1 to find table metadata
    std::vector<SharedTableInfo> scan_sdi_for_tables(
        const uint8_t* data, size_t size);
};

} // namespace enterprise
} // namespace dbrecovery

#endif