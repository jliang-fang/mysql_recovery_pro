#ifndef DBRECOVERY_INNODB_OFFLINE_KEYRING_PARSER_HPP
#define DBRECOVERY_INNODB_OFFLINE_KEYRING_PARSER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include <nlohmann/json.hpp>
#include <openssl/aes.h>

namespace dbrecovery {
namespace innodb {

/**
 * @brief Master key extracted from a keyring
 */
struct MasterKey {
    uint32_t key_id = 0;
    std::string key_name;
    std::vector<uint8_t> key_data;
    std::string key_type;
    std::string algorithm_str;
    TDEAlgorithm algorithm = TDEAlgorithm::AES_256_CBC;

    bool is_valid() const { return !key_data.empty() && key_data.size() >= 16; }
};

/**
 * @brief Key derivation info for table-level TDE keys
 */
struct KeyDerivationInfo {
    uint32_t space_id = 0;
    uint32_t page_no = 0;
    std::string derivation_method;
};

/**
 * @brief OfflineKeyringParser — multi-backend keyring support
 *
 * Supports:
 * - keyring_file (JSON format, unencrypted)
 * - keyring_encrypted_file (encrypted JSON with AES-256-CBC password)
 * - keyring_hashicorp_vault (offline mode)
 * - Manual key entry
 *
 * Note: Implementations moved to src/innodb/offline_keyring_parser.cpp
 */
class OfflineKeyringParser {
public:
    OfflineKeyringParser() = default;

    bool parse_keyring_file(const std::string& path);
    bool parse_encrypted_keyring(const std::string& path, const std::string& password);
    bool parse_vault_export(const std::string& path);

    void add_master_key(const std::vector<uint8_t>& key_data,
                        const std::string& name = "manual",
                        TDEAlgorithm algo = TDEAlgorithm::AES_256_CBC);

    std::vector<uint8_t> derive_table_key(
        const MasterKey& master_key,
        uint32_t space_id,
        uint32_t page_no = 0,
        uint64_t lsn = 0);

    static bool verify_derived_key(const std::vector<uint8_t>& decrypted_page);

    const std::vector<MasterKey>& get_master_keys() const { return master_keys_; }
    std::optional<MasterKey> find_key(const std::string& name_hint) const;
    const std::string& get_last_error() const { return last_error_; }

    struct KeyringSummary {
        size_t total_keys = 0;
        size_t aes_keys = 0;
        size_t other_keys = 0;
        std::vector<std::string> key_names;
    };

    KeyringSummary get_summary() const;

private:
    std::vector<MasterKey> master_keys_;
    std::string last_error_;

    bool parse_keyring_json(const nlohmann::json& j);
    TDEAlgorithm parse_algorithm(const std::string& algo);
    void derive_key_from_password(const std::string& password,
                                   const std::vector<uint8_t>& salt,
                                   std::vector<uint8_t>& out_key);
    std::vector<uint8_t> aes_256_cbc_decrypt(
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& iv,
        const std::vector<uint8_t>& ciphertext);
    std::vector<uint8_t> decode_base64(const std::string& encoded);
    std::vector<uint8_t> decode_hex(const std::string& hex);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_OFFLINE_KEYRING_PARSER_HPP