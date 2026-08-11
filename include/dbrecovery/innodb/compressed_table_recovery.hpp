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
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/utils/lz4_helper.hpp"

namespace dbrecovery {
namespace innodb {

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

    std::string to_string() const {
        std::ostringstream oss;
        oss << "CompressedTableRecoveryStats{pages=" << total_pages
            << ", compressed=" << compressed_pages
            << ", decompressed=" << decompressed_pages
            << ", records=" << recovered_records
            << ", failed=" << failed_pages
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
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
     * Reads FSP_SPACE_FLAGS at absolute offset 42 (FIL_HEADER_SIZE + 4)
     * in big-endian format, extracts ZIP_SSIZE at bits 1-4, and calculates
     * compressed_page_size = 16384 / (1 << zip_ssize).
     *
     * @param file_path  Path to the .ibd file
     * @return true if compression detected and page size set, false otherwise
     */
    bool auto_detect_compression(const std::string& file_path);

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