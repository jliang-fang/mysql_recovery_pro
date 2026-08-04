#ifndef DBRECOVERY_INNODB_ENCRYPTED_TABLE_RECOVERY_HPP
#define DBRECOVERY_INNODB_ENCRYPTED_TABLE_RECOVERY_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief Encrypted table recovery statistics
 */
struct EncryptedTableRecoveryStats {
    size_t total_pages = 0;
    size_t encrypted_pages = 0;
    size_t decrypted_pages = 0;
    size_t recovered_records = 0;
    size_t failed_pages = 0;
    double decrypt_time_ms = 0.0;
    double recovery_time_ms = 0.0;
    double total_time_ms = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "EncryptedTableRecoveryStats{pages=" << total_pages
            << ", encrypted=" << encrypted_pages
            << ", decrypted=" << decrypted_pages
            << ", records=" << recovered_records
            << ", failed=" << failed_pages
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }
};

/**
 * @brief Encrypted table recovery engine
 */
class EncryptedTableRecovery {
public:
    explicit EncryptedTableRecovery(size_t page_size = 16384);

    void set_file_path(const std::filesystem::path& file_path);
    void set_schema(const catalog::TableSchema& schema);
    void set_primary_key(const std::vector<uint8_t>& key);
    void add_table_key(uint32_t key_id, const std::vector<uint8_t>& key,
                       TDEAlgorithm algorithm);
    bool load_keyring(const std::string& keyring_path);
    void set_try_recovery_without_key(bool try_recovery);

    std::vector<std::map<std::string, std::string>> recover();
    const EncryptedTableRecoveryStats& get_stats() const { return stats_; }
    TDEDecryptor& get_decryptor() { return decryptor_; }
    const std::vector<uint32_t>& get_encrypted_pages() const { return encrypted_pages_; }
    const std::vector<uint32_t>& get_failed_pages() const { return failed_pages_; }
    void clear();

private:
    size_t page_size_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;
    TDEDecryptor decryptor_;
    bool try_recovery_without_key_ = true;

    std::vector<uint32_t> encrypted_pages_;
    std::vector<uint32_t> failed_pages_;
    std::vector<std::map<std::string, std::string>> records_;
    EncryptedTableRecoveryStats stats_;

    std::vector<ScannedPage> scan_pages();
    bool is_page_encrypted(const uint8_t* page_data, size_t page_size);
    std::vector<uint8_t> decrypt_page_data(const uint8_t* encrypted_data,
                                           size_t size, uint32_t page_no,
                                           uint32_t space_id);
    std::vector<std::map<std::string, std::string>> parse_page_records(
        const std::vector<ScannedPage>& pages);
};

/**
 * @brief Encrypted table recovery manager
 */
class EncryptedTableRecoveryManager {
public:
    struct EncryptedTableInfo {
        std::string table_name;
        std::string tablespace_id;
        uint32_t key_id;
        TDEAlgorithm algorithm;
        bool has_key;
    };

    EncryptedTableRecoveryManager();

    void add_encrypted_table(const std::string& table_name,
                             const std::string& tablespace_id,
                             uint32_t key_id);
    void set_table_key(const std::string& tablespace_id, uint32_t key_id,
                       const std::vector<uint8_t>& key, TDEAlgorithm algorithm);
    void set_primary_key(const std::vector<uint8_t>& key);
    std::optional<uint32_t> get_table_key_id(const std::string& tablespace_id) const;
    const std::map<std::string, EncryptedTableInfo>& get_tables() const { return tables_; }
    std::optional<EncryptedTableInfo> parse_encryption_info(const std::string& ibd_path);

private:
    std::map<std::string, EncryptedTableInfo> tables_;
    std::vector<uint8_t> primary_key_;
    std::unordered_map<std::string, std::vector<uint8_t>> table_keys_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_ENCRYPTED_TABLE_RECOVERY_HPP