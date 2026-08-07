#ifndef DBRECOVERY_ENTERPRISE_SHARED_TABLESPACE_HPP
#define DBRECOVERY_ENTERPRISE_SHARED_TABLESPACE_HPP

#include "dbrecovery/innodb/shared_tablespace_recovery.hpp"
#include "dbrecovery/enterprise/partition_recovery.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <filesystem>

namespace dbrecovery {
namespace enterprise {

/**
 * @brief Enterprise shared tablespace recovery facade
 *
 * Wraps innodb::SharedTablespaceRecovery with a page-size constructor.
 * The enterprise-level SharedTablespaceRecovery free functions live in
 * partition_recovery.hpp; this facade exposes the innodb engine directly.
 */
class SharedTablespaceRecoveryEngine {
public:
    explicit SharedTablespaceRecoveryEngine(size_t page_size = 16384)
        : inner_(page_size) {}

    void set_tablespace_path(const std::string& path) {
        inner_.set_tablespace_path(path);
    }

    void set_output_dir(const std::string& dir) {
        inner_.set_output_dir(dir);
    }

    const innodb::SharedTablespaceStats& get_stats() const {
        return inner_.get_stats();
    }

    std::vector<innodb::TableIdentity> list_tables() {
        return inner_.list_tables();
    }

    bool extract_table(uint32_t target_space_id, const std::string& output_ibd_path) {
        return inner_.extract_table(target_space_id, output_ibd_path);
    }

    std::vector<std::map<std::string, std::string>> recover_records(
        uint32_t target_space_id, const catalog::TableSchema& schema) {
        return inner_.recover_records(target_space_id, schema);
    }

private:
    innodb::SharedTablespaceRecovery inner_;
};

} // namespace enterprise
} // namespace dbrecovery

#endif // DBRECOVERY_ENTERPRISE_SHARED_TABLESPACE_HPP