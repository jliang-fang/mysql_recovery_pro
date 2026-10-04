#ifndef DBRECOVERY_INNODB_COMPRESSED_TABLE_RECOVERY_HPP
#define DBRECOVERY_INNODB_COMPRESSED_TABLE_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/utils/lz4_helper.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief Compressed table recovery outcome classification
 *
 * Fail-closed status for compressed tablespace recovery. A zero-row result
 * must never alone indicate success — the status distinguishes between
 * "no compression", "recognized but unsupported", "recovered",
 * "corrupted metadata", "physical page length mismatch", and
 * "malformed input".
 *
 * Distinguished states (Phase 4a):
 *  - NOT_COMPRESSED:         Valid FSP flags indicate no compression.
 *  - RECOGNIZED_UNSUPPORTED: Valid compression flags, but no fixture-validated
 *                            decoder available.
 *  - CORRUPTED_METADATA:     FSP_SPACE_FLAGS unreadable, truncated, or invalid
 *                            (e.g. zip_ssize >= 5, non-FSP_HDR page 0).
 *  - PHYSICAL_LENGTH_MISMATCH: Declared physical_page_size from FSP flags
 *                              differs from on-disk page stride.
 *  - COMPRESSED_AND_ENCRYPTED: Both compression and encryption flags present;
 *                              unsupported combination.
 *  - RECOVERED:              Successfully decompressed and parsed records.
 *  - MALFORMED:              File cannot be opened.
 */
enum class CompressionStatus : uint8_t {
    NOT_COMPRESSED             = 0,  ///< Tablespace is not compressed
    RECOGNIZED_UNSUPPORTED     = 1,  ///< Compressed layout detected but no validated decoder
    RECOVERED                  = 2,  ///< Successfully decompressed and parsed records
    MALFORMED                  = 3,  ///< File cannot be opened or read
    CORRUPTED_METADATA         = 4,  ///< FSP flags unreadable, truncated, or invalid
    PHYSICAL_LENGTH_MISMATCH   = 5,  ///< Declared physical page size != on-disk stride
    COMPRESSED_AND_ENCRYPTED   = 6   ///< Both compression and encryption present
};

/**
 * @brief Compressed table recovery statistics
 */
struct CompressedTableRecoveryStats {
    size_t total_pages = 0;
    size_t compressed_pages = 0;
    size_t decompressed_pages = 0;
    size_t recovered_records = 0;
    size_t failed_pages = 0;
    double decompress_time_ms = 0.0;
    double recovery_time_ms = 0.0;
    double total_time_ms = 0.0;
    CompressionStatus status = CompressionStatus::NOT_COMPRESSED;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "CompressedTableRecoveryStats{pages=" << total_pages
            << ", compressed=" << compressed_pages
            << ", decompressed=" << decompressed_pages
            << ", records=" << recovered_records
            << ", failed=" << failed_pages
            << ", status=" << status_to_string(status)
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }

    static const char* status_to_string(CompressionStatus s) {
        switch (s) {
            case CompressionStatus::NOT_COMPRESSED:           return "NOT_COMPRESSED";
            case CompressionStatus::RECOGNIZED_UNSUPPORTED:   return "RECOGNIZED_UNSUPPORTED";
            case CompressionStatus::RECOVERED:                return "RECOVERED";
            case CompressionStatus::MALFORMED:                return "MALFORMED";
            case CompressionStatus::CORRUPTED_METADATA:       return "CORRUPTED_METADATA";
            case CompressionStatus::PHYSICAL_LENGTH_MISMATCH: return "PHYSICAL_LENGTH_MISMATCH";
            case CompressionStatus::COMPRESSED_AND_ENCRYPTED: return "COMPRESSED_AND_ENCRYPTED";
        }
        return "UNKNOWN";
    }
};

/**
 * @brief Compressed table recovery engine
 */
class CompressedTableRecovery {
public:
    explicit CompressedTableRecovery(size_t page_size = 16384);

    void set_file_path(const std::filesystem::path& file_path);
    void set_schema(const catalog::TableSchema& schema);
    void set_compressed_page_size(size_t compressed_size);

    /**
     * @brief Auto-detect compressed page size from IBD file page 0
     *
     * Reads FSP_SPACE_FLAGS at absolute offset 54
     * (FIL_PAGE_DATA + FSP_SPACE_FLAGS = 38 + 16) in big-endian format,
     * compressed_page_size = 16384 / (1 << zip_ssize).
     *
     * Canonical FSP classification only. This never inspects the custom
     * little-endian ZSD byte pattern.
     *
     * @param file_path  Path to the .ibd file
     * @return true if compression detected and page size set, false otherwise
     */
    bool auto_detect_compression(const std::string& file_path);

    /**
     * @brief Classify the tablespace compression status (fail-closed)
     *
     * Canonical FSP-flags classification of an .ibd page 0. A recognized
     * but unvalidated compressed layout reports RECOGNIZED_UNSUPPORTED;
     * recovery is not claimed until a fixture-validated decoder exists.
     *
     * @param file_path Path to the .ibd file
     * @param page_size Logical page size used to locate FSP flags
     * @return Classification result; NOT_COMPRESSED when the tablespace is not compressed
     */
    CompressionStatus classify(const std::string& file_path, size_t page_size = 16384) const;

    /**
     * @brief Has a valid compressed decoder been verified?
     *
     * Returns false until a real compressed fixture validates page
     * interpretation plus ground-truth record recovery. While false,
     * recover() for compressed layout reports RECOGNIZED_UNSUPPORTED
     * instead of claiming successful recovery.
     */
    static bool decoder_is_validated();

    /**
     * @brief Prove the physical page stride from the file layout
     *
     * Returns page_size when no shorter stride can be proven. Used to record
     * the on-disk stride for evidence reports even when FSP_SPACE_FLAGS do not
     * describe it correctly.
     */
    static size_t observe_physical_stride(const std::string& file_path, size_t page_size);

    std::vector<std::map<std::string, std::string>> recover();
    const CompressedTableRecoveryStats& get_stats() const { return stats_; }
    const std::vector<uint32_t>& get_compressed_pages() const { return compressed_pages_; }
    void clear();

private:
    size_t page_size_;
    size_t compressed_page_size_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;

    std::vector<uint32_t> compressed_pages_;
    std::vector<std::map<std::string, std::string>> records_;
    CompressedTableRecoveryStats stats_;

    std::vector<ScannedPage> scan_pages();
    bool is_page_compressed(const uint8_t* page_data, size_t page_size);
    std::vector<uint8_t> decompress_page_data(const uint8_t* compressed_data,
                                              size_t compressed_size,
                                              size_t expected_size);
    std::vector<std::map<std::string, std::string>> parse_page_records(
        const std::vector<ScannedPage>& pages);
};

/**
 * @brief Compressed table recovery manager
 */
class CompressedTableRecoveryManager {
public:
    CompressedTableRecoveryManager();

    void add_compressed_table(const std::string& table_name,
                              uint32_t compressed_page_size);
    void set_compressed_page_size(const std::string& table_name, size_t size);
    std::optional<size_t> get_compressed_page_size(const std::string& table_name) const;
    std::optional<size_t> parse_compression_info(const std::string& ibd_path,
                                                      size_t page_size = 16384);
    const std::map<std::string, size_t>& get_tables() const { return tables_; }

private:
    std::map<std::string, size_t> tables_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_COMPRESSED_TABLE_RECOVERY_HPP