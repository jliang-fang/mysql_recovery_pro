#ifndef DBRECOVERY_ENTERPRISE_OFFLINE_KEYRING_HPP
#define DBRECOVERY_ENTERPRISE_OFFLINE_KEYRING_HPP

// OfflineKeyring (enterprise namespace) extends the innodb
// OfflineKeyringParser with enterprise features: multi-keyring
// inventory, key version management, KMIP integration, and
// key rotation for TDE-encrypted tablespace recovery.
#include "dbrecovery/innodb/offline_keyring_parser.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <filesystem>
#include <set>

namespace dbrecovery {
namespace enterprise {

// Re-export innodb-level keyring types.
using MasterKey        = innodb::MasterKey;
using KeyDerivationInfo = innodb::KeyDerivationInfo;
using TDEAlgorithm     = innodb::TDEAlgorithm;

/**
 * @brief Enterprise offline keyring manager
 *
 * Wraps innodb::OfflineKeyringParser and adds:
 * - Multi-keyring inventory (search across multiple keyring files)
 * - Key version management (track which key version was active at a given LSN)
 * - Key rotation support (re-encrypt tablespace with a new key)
 * - KMIP export import (parse Vault / KMIP format exports)
 * - Batch table key derivation
 */
class OfflineKeyring {
public:
    OfflineKeyring() = default;
    ~OfflineKeyring() = default;

    // ---- Single-keyring operations (delegates to innodb parser) ----
    bool parse_keyring_file(const std::string& path) {
        return parser_.parse_keyring_file(path);
    }
    bool parse_encrypted_keyring(const std::string& path, const std::string& password) {
        return parser_.parse_encrypted_keyring(path, password);
    }
    bool parse_vault_export(const std::string& path) {
        return parser_.parse_vault_export(path);
    }
    void add_master_key(const std::vector<uint8_t>& key_data,
                        const std::string& name = "manual",
                        TDEAlgorithm algo = TDEAlgorithm::AES_256_CBC) {
        parser_.add_master_key(key_data, name, algo);
    }

    // ---- Multi-keyring inventory ----
    /**
     * @brief Scan a directory for all keyring files and parse them
     * @return Number of keyring files successfully parsed
     */
    size_t scan_keyring_directory(const std::string& dir_path);

    /**
     * @brief Search all known keyrings for a key by name hint
     */
    std::optional<MasterKey> find_key_any(const std::string& name_hint) const;

    /**
     * @brief Get all master keys from all discovered keyrings
     */
    std::vector<MasterKey> all_keys() const;

    // ---- Key derivation ----
    std::vector<uint8_t> derive_table_key(const MasterKey& master_key,
                                          uint32_t space_id,
                                          uint32_t page_no = 0,
                                          uint64_t lsn = 0) {
        return parser_.derive_table_key(master_key, space_id, page_no, lsn);
    }

    /**
     * @brief Derive keys for all known master keys and return the first valid one
     */
    std::vector<uint8_t> derive_table_key_any(uint32_t space_id,
                                               uint32_t page_no = 0,
                                               uint64_t lsn = 0);

    // ---- Key version management ----
    struct KeyVersion {
        uint32_t version_id;
        std::string key_name;
        uint64_t active_from_lsn;
        std::optional<uint64_t> retired_at_lsn;
        std::string algorithm;
    };

    /**
     * @brief Record a key version for rotation tracking
     */
    void add_key_version(const KeyVersion& version);

    /**
     * @brief Find the key version that was active at a given LSN
     */
    std::optional<KeyVersion> key_version_at_lsn(uint64_t lsn) const;

    /**
     * @brief Get all tracked key versions
     */
    const std::vector<KeyVersion>& key_versions() const { return key_versions_; }

    // ---- Key rotation ----
    /**
     * @brief Simulate re-encrypting a tablespace key under a new master key
     * @return The re-wrapped table key, or nullopt on failure
     */
    std::optional<std::vector<uint8_t>> rotate_table_key(
        uint32_t space_id,
        const MasterKey& new_master_key,
        uint32_t page_no = 0,
        uint64_t lsn = 0);

    // ---- Decryption verification ----
    static bool verify_decrypted_page(const std::vector<uint8_t>& page_data) {
        return innodb::OfflineKeyringParser::verify_derived_key(page_data);
    }

    // ---- Statistics ----
    innodb::OfflineKeyringParser::KeyringSummary get_summary() const {
        return parser_.get_summary();
    }

    const std::string& last_error() const { return last_error_; }

private:
    innodb::OfflineKeyringParser parser_;
    std::vector<KeyVersion> key_versions_;
    std::string last_error_;

    // Track which keyring files have been loaded (avoid duplicates)
    std::set<std::string> loaded_keyring_files_;
};

} // namespace enterprise
} // namespace dbrecovery

#endif // DBRECOVERY_ENTERPRISE_OFFLINE_KEYRING_HPP