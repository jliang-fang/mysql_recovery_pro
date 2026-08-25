#ifndef DBRECOVERY_INFERENCE_DDL_CHANGE_DETECTOR_HPP
#define DBRECOVERY_INFERENCE_DDL_CHANGE_DETECTOR_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <memory>
#include <chrono>

#include "dbrecovery/engine/schema_template.hpp"

namespace dbrecovery {
namespace inference {

/// Column change metadata for a schema version transition
struct ColumnDelta {
    uint32_t ordinal_position = 0;
    std::string column_name;
    std::string type_name;
    double confidence = 0.0;
};

/// Schema version detected by DDLChangeDetector
struct SchemaVersion {
    uint32_t version_id = 0;
    uint32_t column_count = 0;
    uint32_t page_count = 0;
    std::vector<uint32_t> page_numbers;
    double confidence = 0.0;
    std::vector<ColumnDelta> columns_added;
    std::vector<ColumnDelta> columns_dropped;
    std::string description;
};

/// Result of DDL change detection
struct DDLChangeChainResult {
    bool has_ddl_change = false;
    bool blocked_auto_decode = false;
    std::vector<SchemaVersion> versions;
    std::string summary;
};

/**
 * @brief Detects DDL changes (column count variations) across InnoDB pages
 *
 * Clusters pages by column count, identifies schema version boundaries,
 * and provides confidence scores for each detected version.
 */
class DDLChangeDetector {
public:
    DDLChangeDetector() = default;
    ~DDLChangeDetector() = default;

    /**
     * @brief Detect DDL changes from raw page data
     * @param pages Vector of (data_ptr, size) pairs from INDEX pages
     * @return Detection result with version information
     */
    DDLChangeChainResult detect_versions_from_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);

    /**
     * @brief Detect DDL changes from page buffers (convenience overload)
     * @param page_buffers Vector of page data buffers
     * @return Detection result with version information
     */
    DDLChangeChainResult detect_versions_from_buffers(
        const std::vector<std::vector<uint8_t>>& page_buffers);

    /**
     * @brief Apply detected versions to a SchemaTemplate
     * @param result Detection result from detect_versions
     * @param tmpl Schema template to annotate
     * @return true if applied successfully
     */
    bool apply_to_schema_template(
        const DDLChangeChainResult& result,
        engine::SchemaTemplate& tmpl) const;
};

} // namespace inference
} // namespace dbrecovery

#endif // DBRECOVERY_INFERENCE_DDL_CHANGE_DETECTOR_HPP