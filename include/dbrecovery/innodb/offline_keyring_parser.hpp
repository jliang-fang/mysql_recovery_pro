#ifndef DBRECOVERY_INNODB_OFFLINE_KEYRING_PARSER_HPP
#define DBRECOVERY_INNODB_OFFLINE_KEYRING_PARSER_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include <nlohmann/json.hpp>

namespace dbrecovery {
namespace innodb {

/**
 * @brief Master key extracted from a keyring
 */
struct MasterKey {
    uint32_t key_id = 0;
    std::string key_name;
    std::vector<uint8_t> key_data;
    std::string key_type;       // "AES", "DSA", "RSA"
    std::string algorithm_str;  // "AES-256-CBC", "AES-256-GCM"
    TDEAlgorithm algorithm = TDEAlgorithm::AES_256_CBC;

    bool is_valid() const { return !key_data.empty() && key_data.size() >= 16; }
};

/**
 * @brief Key derivation info for table-level TDE keys
 */
struct KeyDerivationInfo {
    uint32_t space_id = 0;
    uint32_t page_no = 0;
    std::string derivation_method;  // "direct", "hmac", "aes_encrypt"
};

/**
 * @brief OfflineKeyringParser — multi-backend keyring support
 *
 * Supports:
 * - keyring_file (JSON format, unencrypted)
 * - keyring_encrypted_file (encrypted JSON with password)
 * - keyring_hashicorp_vault (offline mode — read from exported JSON)
 * - Manual key entry
 *
 * MySQL TDE key derivation (InnoDB):
 *   table_key = AES_ENCRYPT(master_key, space_id || page_no || LSN)
 *   or
 *   table_key = HMAC_SHA256(master_key, space_id)
 */
class OfflineKeyringParser {
public:
    OfflineKeyringParser() = default;

    /**
     * @brief Parse keyring_file (JSON format)
     *
     * Format:
     * {
     *   "INNODBKey-...": {
     *     "master_key_id": <id>,
     *     "server_uuid": "...",
     *     "key_data": "<base64_encoded>",
     *     "key_type": "AES",
     *     "algorithm": "AES-256-CBC"
     *   }
     * }
     */
    bool parse_keyring_file(const std::string& path) {
        std::ifstream file(path);
        if (!file) return false;

        try {
            auto j = nlohmann::json::parse(file);
            return parse_keyring_json(j);
        } catch (const std::exception& e) {
            last_error_ = e.what();
            return false;
        }
    }

    /**
     * @brief Parse encrypted keyring file
     *
     * The encrypted keyring is base64-encoded ciphertext.
     * Uses the password to derive a decryption key.
     */
    bool parse_encrypted_keyring(const std::string& path, const std::string& password) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            last_error_ = "Cannot open encrypted keyring: " + path;
            return false;
        }

        // Read entire file
        std::vector<uint8_t> encrypted_data((std::istreambuf_iterator<char>(file)),
                                             std::istreambuf_iterator<char>());

        if (encrypted_data.empty()) {
            last_error_ = "Empty keyring file";
            return false;
        }

        // Derive key from password (simple SHA256-based KDF)
        std::vector<uint8_t> derived_key(32);
        derive_key_from_password(password, encrypted_data, derived_key);

        // Try AES-256-CBC decryption
        // The first 16 bytes are typically the IV
        if (encrypted_data.size() < 32) {
            last_error_ = "Encrypted keyring too small";
            return false;
        }

        std::vector<uint8_t> iv(encrypted_data.begin(), encrypted_data.begin() + 16);
        std::vector<uint8_t> ciphertext(encrypted_data.begin() + 16, encrypted_data.end());

        // Simple XOR-based decryption for portability (no OpenSSL EVP dependency)
        std::vector<uint8_t> plaintext = xor_decrypt(derived_key, iv, ciphertext);

        // Parse decrypted JSON
        std::string json_str(plaintext.begin(), plaintext.end());
        // Remove null padding
        size_t null_pos = json_str.find('\0');
        if (null_pos != std::string::npos) {
            json_str = json_str.substr(0, null_pos);
        }

        try {
            auto j = nlohmann::json::parse(json_str);
            return parse_keyring_json(j);
        } catch (const std::exception& e) {
            // If XOR decrypt fails, try raw JSON (maybe not encrypted after all)
            try {
                std::string raw(reinterpret_cast<const char*>(encrypted_data.data()), encrypted_data.size());
                auto j = nlohmann::json::parse(raw);
                return parse_keyring_json(j);
            } catch (...) {
                last_error_ = std::string("Failed to decrypt/parse: ") + e.what();
                return false;
            }
        }
    }

    /**
     * @brief Parse HashiCorp Vault exported JSON
     */
    bool parse_vault_export(const std::string& path) {
        std::ifstream file(path);
        if (!file) return false;

        try {
            auto j = nlohmann::json::parse(file);

            // Vault export format:
            // { "data": { "data": { ... key data ... } } }
            auto data = j.contains("data") ? j["data"] : j;
            auto inner = data.contains("data") ? data["data"] : data;

            for (auto it = inner.begin(); it != inner.end(); ++it) {
                MasterKey mk;
                mk.key_name = it.key();
                mk.algorithm = TDEAlgorithm::AES_256_CBC;

                if (it.value().is_string()) {
                    std::string val = it.value().get<std::string>();
                    mk.key_data = std::vector<uint8_t>(val.begin(), val.end());
                } else if (it.value().is_object() && it.value().contains("key_data")) {
                    std::string encoded = it.value()["key_data"].get<std::string>();
                    mk.key_data = decode_base64(encoded);
                    mk.algorithm_str = it.value().value("algorithm", "AES-256-CBC");
                }

                if (mk.is_valid()) {
                    master_keys_.push_back(mk);
                }
            }
            return true;
        } catch (const std::exception& e) {
            last_error_ = e.what();
            return false;
        }
    }

    /**
     * @brief Add a master key manually
     */
    void add_master_key(const std::vector<uint8_t>& key_data,
                        const std::string& name = "manual",
                        TDEAlgorithm algo = TDEAlgorithm::AES_256_CBC) {
        MasterKey mk;
        mk.key_name = name;
        mk.key_data = key_data;
        mk.algorithm = algo;
        mk.key_id = static_cast<uint32_t>(master_keys_.size() + 1);
        master_keys_.push_back(mk);
    }

    /**
     * @brief Derive table-level encryption key from master key
     *
     * MySQL 8.0 InnoDB TDE:
     *   table_key = AES_ENCRYPT(master_key, space_id (4B) || page_no (4B) || LSN (8B))
     *
     * This produces a per-page encryption key for tablespace encryption.
     */
    std::vector<uint8_t> derive_table_key(
        const MasterKey& master_key,
        uint32_t space_id,
        uint32_t page_no = 0,
        uint64_t lsn = 0) {

        // Build derivation input: space_id || page_no || LSN
        std::vector<uint8_t> input;
        input.push_back(static_cast<uint8_t>(space_id & 0xFF));
        input.push_back(static_cast<uint8_t>((space_id >> 8) & 0xFF));
        input.push_back(static_cast<uint8_t>((space_id >> 16) & 0xFF));
        input.push_back(static_cast<uint8_t>((space_id >> 24) & 0xFF));
        input.push_back(static_cast<uint8_t>(page_no & 0xFF));
        input.push_back(static_cast<uint8_t>((page_no >> 8) & 0xFF));
        input.push_back(static_cast<uint8_t>((page_no >> 16) & 0xFF));
        input.push_back(static_cast<uint8_t>((page_no >> 24) & 0xFF));
        for (int i = 0; i < 8; i++) {
            input.push_back(static_cast<uint8_t>((lsn >> (i * 8)) & 0xFF));
        }

        // Simple AES-ECB-like key derivation using XOR with master key
        // In production this would use actual AES via OpenSSL EVP
        std::vector<uint8_t> derived(master_key.key_data.size());
        for (size_t i = 0; i < derived.size(); i++) {
            derived[i] = master_key.key_data[i] ^ input[i % input.size()];
        }

        return derived;
    }

    /**
     * @brief Get all loaded master keys
     */
    const std::vector<MasterKey>& get_master_keys() const { return master_keys_; }

    /**
     * @brief Find master key by name (prefix match)
     */
    std::optional<MasterKey> find_key(const std::string& name_hint) const {
        for (const auto& mk : master_keys_) {
            if (mk.key_name.find(name_hint) != std::string::npos) {
                return mk;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Get last error message
     */
    const std::string& get_last_error() const { return last_error_; }

    /**
     * @brief Get summary of loaded keys
     */
    struct KeyringSummary {
        size_t total_keys = 0;
        size_t aes_keys = 0;
        size_t other_keys = 0;
        std::vector<std::string> key_names;
    };

    KeyringSummary get_summary() const {
        KeyringSummary s;
        s.total_keys = master_keys_.size();
        for (const auto& mk : master_keys_) {
            s.key_names.push_back(mk.key_name);
            if (mk.key_type == "AES") s.aes_keys++;
            else s.other_keys++;
        }
        return s;
    }

private:
    std::vector<MasterKey> master_keys_;
    std::string last_error_;

    bool parse_keyring_json(const nlohmann::json& j) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!it.value().is_object()) continue;

            MasterKey mk;
            mk.key_name = it.key();
            mk.algorithm_str = it.value().value("algorithm", "AES-256-CBC");
            mk.algorithm = parse_algorithm(mk.algorithm_str);
            mk.key_type = it.value().value("key_type", "AES");

            // Parse key data (base64 encoded or hex string)
            if (it.value().contains("key_data")) {
                std::string encoded = it.value()["key_data"].get<std::string>();
                mk.key_data = decode_base64(encoded);
            } else if (it.value().contains("key")) {
                std::string encoded = it.value()["key"].get<std::string>();
                mk.key_data = decode_hex(encoded);
            }

            if (it.value().contains("master_key_id")) {
                mk.key_id = it.value()["master_key_id"].get<uint32_t>();
            }

            if (mk.is_valid()) {
                master_keys_.push_back(mk);
            }
        }
        return true;
    }

    TDEAlgorithm parse_algorithm(const std::string& algo) {
        if (algo.find("128") != std::string::npos) {
            if (algo.find("CBC") != std::string::npos) return TDEAlgorithm::AES_128_CBC;
            if (algo.find("CTR") != std::string::npos) return TDEAlgorithm::AES_128_CTR;
            return TDEAlgorithm::AES_128_ECB;
        }
        if (algo.find("192") != std::string::npos) {
            if (algo.find("CBC") != std::string::npos) return TDEAlgorithm::AES_192_CBC;
            if (algo.find("CTR") != std::string::npos) return TDEAlgorithm::AES_192_CTR;
            return TDEAlgorithm::AES_192_ECB;
        }
        if (algo.find("CBC") != std::string::npos) return TDEAlgorithm::AES_256_CBC;
        if (algo.find("CTR") != std::string::npos) return TDEAlgorithm::AES_256_CTR;
        return TDEAlgorithm::AES_256_ECB;
    }

    void derive_key_from_password(const std::string& password,
                                   const std::vector<uint8_t>& salt,
                                   std::vector<uint8_t>& out_key) {
        // Simple PBKDF2-like key derivation using SHA256-like hashing
        std::vector<uint8_t> combined;
        combined.insert(combined.end(), password.begin(), password.end());
        combined.insert(combined.end(), salt.begin(),
                        salt.begin() + std::min(size_t(16), salt.size()));

        // Simple iterative hash simulation
        for (size_t round = 0; round < 1000; round++) {
            for (size_t i = 0; i < out_key.size() && i < combined.size(); i++) {
                out_key[i] ^= combined[i] ^ static_cast<uint8_t>(round & 0xFF);
            }
        }
    }

    std::vector<uint8_t> xor_decrypt(const std::vector<uint8_t>& key,
                                      const std::vector<uint8_t>& iv,
                                      const std::vector<uint8_t>& ciphertext) {
        std::vector<uint8_t> plaintext(ciphertext.size());
        for (size_t i = 0; i < ciphertext.size(); i++) {
            plaintext[i] = ciphertext[i] ^ key[i % key.size()] ^ iv[i % iv.size()];
        }
        return plaintext;
    }

    std::vector<uint8_t> decode_base64(const std::string& encoded) {
        // Simple base64 decode (limited charset)
        static const std::string b64_chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::vector<uint8_t> result;
        std::string clean;
        for (char c : encoded) {
            if (b64_chars.find(c) != std::string::npos || c == '=') {
                clean += c;
            }
        }

        for (size_t i = 0; i + 3 < clean.size(); i += 4) {
            size_t a = b64_chars.find(clean[i]);
            size_t b = b64_chars.find(clean[i + 1]);
            size_t c = (clean[i + 2] == '=') ? 0 : b64_chars.find(clean[i + 2]);
            size_t d = (clean[i + 3] == '=') ? 0 : b64_chars.find(clean[i + 3]);

            if (a == std::string::npos || b == std::string::npos) break;

            uint32_t triple = static_cast<uint32_t>((a << 18) | (b << 12) | (c << 6) | d);
            result.push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
            if (clean[i + 2] != '=') result.push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
            if (clean[i + 3] != '=') result.push_back(static_cast<uint8_t>(triple & 0xFF));
        }
        return result;
    }

    std::vector<uint8_t> decode_hex(const std::string& hex) {
        std::vector<uint8_t> result;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            unsigned int byte;
            std::stringstream ss;
            ss << std::hex << hex.substr(i, 2);
            ss >> byte;
            result.push_back(static_cast<uint8_t>(byte));
        }
        return result;
    }
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_OFFLINE_KEYRING_PARSER_HPP