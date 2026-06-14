#ifndef DBRECOVERY_INNODB_RECOVERY_LIMITS_HPP
#define DBRECOVERY_INNODB_RECOVERY_LIMITS_HPP

#include <cstdint>
#include <string>
#include <stdexcept>

namespace dbrecovery {
namespace innodb {

/**
 * @brief Recovery limits configuration
 *
 * Inspired by testdisk/PhotoRec's max_filesize mechanism.
 * Prevents memory exhaustion from malicious or corrupted files.
 */
struct RecoveryLimits {
    /// Maximum file size to process (default: 2TB)
    uint64_t max_file_size = 2ULL * 1024 * 1024 * 1024;

    /// Maximum page size to accept (default: 64KB)
    uint32_t max_page_size = 64 * 1024;

    /// Minimum page size to accept (default: 4KB)
    uint32_t min_page_size = 4 * 1024;

    /// Maximum number of records to recover per table (default: 1 billion)
    uint64_t max_records_per_table = 1000ULL * 1000 * 1000;

    /// Maximum BLOB chain length (default: 1000 pages)
    uint32_t max_blob_chain_length = 1000;

    /// Maximum BLOB data size per record (default: 1GB)
    uint64_t max_blob_size_per_record = 1024ULL * 1024 * 1024;

    /// Maximum memory usage for decompression (default: 256MB)
    uint64_t max_decompression_memory = 256ULL * 1024 * 1024;

    /// Maximum SDI JSON size (default: 16MB)
    uint64_t max_sdi_json_size = 16ULL * 1024 * 1024;

    /// Maximum field count per record (default: 1024)
    uint32_t max_fields_per_record = 1024;

    /// Maximum field size (default: 16MB)
    uint64_t max_field_size = 16ULL * 1024 * 1024;

    /// Maximum index chain depth (prevent infinite loops)
    uint32_t max_index_chain_depth = 10000;

    /// Timeout for recovery operations in seconds (default: 1 hour)
    uint32_t recovery_timeout_seconds = 3600;

    /**
     * @brief Validate file size against limit
     * @throws std::runtime_error if file is too large
     */
    void validate_file_size(uint64_t size, const std::string& filename = "") const {
        if (size > max_file_size) {
            throw std::runtime_error(
                "File too large: " + std::to_string(size) + " bytes > " +
                std::to_string(max_file_size) + " bytes limit" +
                (filename.empty() ? "" : " (" + filename + ")")
            );
        }
    }

    /**
     * @brief Validate page size against limits
     * @throws std::runtime_error if page size is invalid
     */
    void validate_page_size(uint32_t size) const {
        if (size < min_page_size || size > max_page_size) {
            throw std::runtime_error(
                "Invalid page size: " + std::to_string(size) +
                " (valid range: " + std::to_string(min_page_size) + "-" +
                std::to_string(max_page_size) + " bytes)"
            );
        }
    }

    /**
     * @brief Check if record count is within limit
     * @return true if within limit, false if exceeded
     */
    bool check_record_count(uint64_t count) const {
        return count < max_records_per_table;
    }

    /**
     * @brief Check if BLOB chain length is within limit
     * @return true if within limit, false if exceeded
     */
    bool check_blob_chain_length(uint32_t length) const {
        return length < max_blob_chain_length;
    }

    /**
     * @brief Create conservative limits (for resource-constrained environments)
     */
    static RecoveryLimits conservative() {
        RecoveryLimits limits;
        limits.max_file_size = 100ULL * 1024 * 1024;  // 100MB
        limits.max_records_per_table = 10 * 1000 * 1000;  // 10M records
        limits.max_blob_size_per_record = 64ULL * 1024 * 1024;  // 64MB
        limits.max_decompression_memory = 64ULL * 1024 * 1024;  // 64MB
        limits.recovery_timeout_seconds = 300;  // 5 minutes
        return limits;
    }

    /**
     * @brief Create unlimited limits (use with caution)
     */
    static RecoveryLimits unlimited() {
        RecoveryLimits limits;
        limits.max_file_size = UINT64_MAX;
        limits.max_records_per_table = UINT64_MAX;
        limits.max_blob_size_per_record = UINT64_MAX;
        limits.max_decompression_memory = UINT64_MAX;
        limits.recovery_timeout_seconds = UINT32_MAX;
        return limits;
    }
};

/**
 * @brief Global recovery limits instance
 */
inline RecoveryLimits& global_limits() {
    static RecoveryLimits limits;
    return limits;
}

/**
 * @brief RAII guard for temporarily changing limits
 */
class LimitsGuard {
public:
    explicit LimitsGuard(const RecoveryLimits& new_limits)
        : old_limits_(global_limits()) {
        global_limits() = new_limits;
    }

    ~LimitsGuard() {
        global_limits() = old_limits_;
    }

private:
    RecoveryLimits old_limits_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_RECOVERY_LIMITS_HPP