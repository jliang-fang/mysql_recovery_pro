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
#include "dbrecovery/innodb/offline_keyring_parser.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief TDE encrypted table recovery outcome classification
 *
 * Fail-closed status for encrypted tablespace recovery. A zero-row result
 * must never alone indicate success — the status distinguishes between
 * "not encrypted", "encrypted but unsupported", "recovered",
 * "corrupted metadata", "key missing", and "malformed input".
 *
 * Distinguished states (Phase 4c):
 *  - NOT_ENCRYPTED:         No TDE candidate recognized; this is not proof that
 *                            encryption metadata is absent.
 *  - ENCRYPTED_UNSUPPORTED: A TDE candidate was recognized, but no
 *                            fixture-validated decryptor is available.
 *  - RECOVERED:             Successfully decrypted and parsed records.
 *  - MALFORMED:             File cannot be opened or read.
 *  - CORRUPTED_METADATA:    Reserved for future authoritative, version-specific
 *                            TDE metadata validation.
 *  - KEY_MISSING:           Encryption detected but no matching key available.
 */
enum class TDEStatus : uint8_t {
    NOT_ENCRYPTED           = 0,
    ENCRYPTED_UNSUPPORTED   = 1,
    RECOVERED               = 2,
    MALFORMED               = 3,
    CORRUPTED_METADATA      = 4,
    KEY_MISSING             = 5
};

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
    TDEStatus tde_status = TDEStatus::NOT_ENCRYPTED;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "EncryptedTableRecoveryStats{pages=" << total_pages
            << ", encrypted=" << encrypted_pages
            << ", decrypted=" << decrypted_pages
            << ", records=" << recovered_records
            << ", failed=" << failed_pages
            << ", status=" << status_to_string(tde_status)
            << ", time=" << std::fixed << std::setprecision(2) << total_time_ms << "ms"
            << "}";
        return oss.str();
    }

    static const char* status_to_string(TDEStatus s) {
        switch (s) {
            case TDEStatus::NOT_ENCRYPTED:           return "NOT_ENCRYPTED";
            case TDEStatus::ENCRYPTED_UNSUPPORTED:   return "ENCRYPTED_UNSUPPORTED";
            case TDEStatus::RECOVERED:               return "RECOVERED";
            case TDEStatus::MALFORMED:               return "MALFORMED";
            case TDEStatus::CORRUPTED_METADATA:      return "CORRUPTED_METADATA";
            case TDEStatus::KEY_MISSING:             return "KEY_MISSING";
        }
        return "UNKNOWN";
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
    bool configure_keys(const std::vector<MasterKey>& keys);
    bool load_keyring(const std::string& keyring_path);
    void set_try_recovery_without_key(bool try_recovery);
    const std::string& get_last_error() const { return last_error_; }

    std::vector<std::map<std::string, std::string>> recover();
    const EncryptedTableRecoveryStats& get_stats() const { return stats_; }
    TDEDecryptor& get_decryptor() { return decryptor_; }
    const std::vector<uint32_t>& get_encrypted_pages() const { return encrypted_pages_; }
    const std::vector<uint32_t>& get_failed_pages() const { return failed_pages_; }
    void clear();

    /**
     * @brief Has a fixture-validated TDE decryptor been verified?
     *
     * Returns false until a real encrypted fixture validates page
     * interpretation plus ground-truth record recovery. While false,
     * recover() for encrypted layout reports ENCRYPTED_UNSUPPORTED
     * instead of claiming successful recovery.
     */
    static bool decryptor_is_validated();

    /**
     * @brief Classify the tablespace encryption status (fail-closed)
     *
     * Candidate TDE routing classification of an .ibd page 0. A recognized
     * candidate reports ENCRYPTED_UNSUPPORTED; a non-candidate result does not
     * prove encryption is absent. Metadata corruption is reserved for a future
     * authoritative, fixture-validated format parser.
     *
     * @param file_path Path to the .ibd file
     * @param page_size Logical page size used to locate TDE header
     * @return Classification result; NOT_ENCRYPTED when the tablespace is not encrypted
     */
    TDEStatus classify(const std::string& file_path, size_t page_size = 16384) const;

private:
    size_t page_size_;
    std::filesystem::path file_path_;
    catalog::TableSchema schema_;
    TDEDecryptor decryptor_;
    bool try_recovery_without_key_ = false;

    std::vector<uint32_t> encrypted_pages_;
    std::vector<uint32_t> failed_pages_;
    std::vector<std::map<std::string, std::string>> records_;
    EncryptedTableRecoveryStats stats_;
    std::string last_error_;

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