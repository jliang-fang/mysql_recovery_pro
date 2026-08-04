/**
 * @file offline_keyring_parser.cpp
 * @brief Multi-backend keyring parser with AES-256-CBC decryption
 *
 * Implements real AES-256-CBC decryption for encrypted keyring files,
 * replacing the XOR placeholder. Maintains all other functionality
 * from the header (keyring_file JSON, vault export, manual keys).
 */

#include "dbrecovery/innodb/offline_keyring_parser.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace dbrecovery {
namespace innodb {

// ==================== OfflineKeyringParser ====================

bool OfflineKeyringParser::parse_keyring_file(const std::string& path) {
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

bool OfflineKeyringParser::parse_encrypted_keyring(const std::string& path, const std::string& password) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        last_error_ = "Cannot open encrypted keyring: " + path;
        return false;
    }

    std::vector<uint8_t> encrypted_data((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());

    if (encrypted_data.empty()) {
        last_error_ = "Empty keyring file";
        return false;
    }

    // Derive key from password using SHA256-based KDF
    std::vector<uint8_t> derived_key(32);
    derive_key_from_password(password, encrypted_data, derived_key);

    // First 16 bytes are the IV for AES-256-CBC
    if (encrypted_data.size() < 32) {
        last_error_ = "Encrypted keyring too small";
        return false;
    }

    std::vector<uint8_t> iv(encrypted_data.begin(), encrypted_data.begin() + 16);
    std::vector<uint8_t> ciphertext(encrypted_data.begin() + 16, encrypted_data.end());

    // AES-256-CBC decryption using OpenSSL EVP
    std::vector<uint8_t> plaintext = aes_256_cbc_decrypt(derived_key, iv, ciphertext);

    if (plaintext.empty()) {
        last_error_ = "AES-256-CBC decryption failed -- wrong password or corrupted keyring";
        return false;
    }

    // Parse decrypted JSON
    std::string json_str(plaintext.begin(), plaintext.end());
    size_t null_pos = json_str.find('\0');
    if (null_pos != std::string::npos) {
        json_str = json_str.substr(0, null_pos);
    }

    try {
        auto j = nlohmann::json::parse(json_str);
        return parse_keyring_json(j);
    } catch (const std::exception& e) {
        last_error_ = std::string("JSON parse failed after decryption: ") + e.what();
        return false;
    }
}

bool OfflineKeyringParser::parse_vault_export(const std::string& path) {
    std::ifstream file(path);
    if (!file) return false;

    try {
        auto j = nlohmann::json::parse(file);

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

void OfflineKeyringParser::add_master_key(const std::vector<uint8_t>& key_data,
                                            const std::string& name,
                                            TDEAlgorithm algo) {
    MasterKey mk;
    mk.key_name = name;
    mk.key_data = key_data;
    mk.algorithm = algo;
    mk.key_id = static_cast<uint32_t>(master_keys_.size() + 1);
    master_keys_.push_back(mk);
}

std::vector<uint8_t> OfflineKeyringParser::derive_table_key(
    const MasterKey& master_key,
    uint32_t space_id,
    uint32_t page_no,
    uint64_t lsn) {
    (void)page_no; (void)lsn;

    std::vector<uint8_t> plaintext(16, 0);
    plaintext[0] = space_id & 0xFF;
    plaintext[1] = (space_id >> 8) & 0xFF;
    plaintext[2] = (space_id >> 16) & 0xFF;
    plaintext[3] = (space_id >> 24) & 0xFF;

    std::vector<uint8_t> derived(32, 0);
    std::vector<uint8_t> mk = master_key.key_data;

    if (mk.size() < 16) mk.resize(16, 0);
    else if (mk.size() < 24) mk.resize(24, 0);
    else if (mk.size() < 32) mk.resize(32, 0);
    else if (mk.size() > 32) mk.resize(32);

    AES_KEY aes_key;
    int bits = (mk.size() == 32) ? 256 : (mk.size() == 24) ? 192 : 128;
    if (AES_set_encrypt_key(mk.data(), bits, &aes_key) != 0) {
        return {};
    }

    for (size_t i = 0; i < derived.size(); i += 16) {
        std::vector<uint8_t> block = plaintext;
        block[0] ^= static_cast<uint8_t>(i / 16);
        uint8_t output[16];
        AES_encrypt(block.data(), output, &aes_key);
        std::memcpy(derived.data() + i, output, std::min(size_t(16), derived.size() - i));
    }

    return derived;
}

bool OfflineKeyringParser::verify_derived_key(const std::vector<uint8_t>& decrypted_page) {
    if (decrypted_page.size() < 38) return false;
    uint16_t page_type = (decrypted_page[24] << 8) | decrypted_page[25];
    return page_type == 0x45BF || page_type == 0x45BE ||
           page_type == 0x45B2 || page_type == 0x0000;
}

std::optional<MasterKey> OfflineKeyringParser::find_key(const std::string& name_hint) const {
    for (const auto& mk : master_keys_) {
        if (mk.key_name.find(name_hint) != std::string::npos) {
            return mk;
        }
    }
    return std::nullopt;
}

OfflineKeyringParser::KeyringSummary OfflineKeyringParser::get_summary() const {
    KeyringSummary s;
    s.total_keys = master_keys_.size();
    for (const auto& mk : master_keys_) {
        s.key_names.push_back(mk.key_name);
        if (mk.key_type == "AES") s.aes_keys++;
        else s.other_keys++;
    }
    return s;
}

// ==================== Private Methods ====================

bool OfflineKeyringParser::parse_keyring_json(const nlohmann::json& j) {
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_object()) continue;

        MasterKey mk;
        mk.key_name = it.key();
        mk.algorithm_str = it.value().value("algorithm", "AES-256-CBC");
        mk.algorithm = parse_algorithm(mk.algorithm_str);
        mk.key_type = it.value().value("key_type", "AES");

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

TDEAlgorithm OfflineKeyringParser::parse_algorithm(const std::string& algo) {
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

void OfflineKeyringParser::derive_key_from_password(const std::string& password,
                                                     const std::vector<uint8_t>& salt,
                                                     std::vector<uint8_t>& out_key) {
    // Use SHA256-based PBKDF2-like derivation
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), password.begin(), password.end());
    combined.insert(combined.end(), salt.begin(),
                    salt.begin() + std::min(size_t(16), salt.size()));

    // Multiple rounds of SHA256 hashing
    std::vector<uint8_t> hash(32);
    for (size_t round = 0; round < 10000; round++) {
        // Feed combined data + round number into SHA256
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, combined.data(), combined.size());

        // Mix in round number as 4 bytes LE
        uint8_t round_bytes[4];
        round_bytes[0] = round & 0xFF;
        round_bytes[1] = (round >> 8) & 0xFF;
        round_bytes[2] = (round >> 16) & 0xFF;
        round_bytes[3] = (round >> 24) & 0xFF;
        SHA256_Update(&ctx, round_bytes, 4);

        // Mix in previous hash (except first round)
        if (round > 0) {
            SHA256_Update(&ctx, hash.data(), hash.size());
        }

        SHA256_Final(hash.data(), &ctx);
    }

    // Copy result to output key (truncate or pad)
    size_t copy_size = std::min(hash.size(), out_key.size());
    std::memcpy(out_key.data(), hash.data(), copy_size);
    if (out_key.size() > copy_size) {
        std::memset(out_key.data() + copy_size, 0, out_key.size() - copy_size);
    }
}

std::vector<uint8_t> OfflineKeyringParser::aes_256_cbc_decrypt(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv,
    const std::vector<uint8_t>& ciphertext) {

    // Use OpenSSL EVP for AES-256-CBC decryption
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    std::vector<uint8_t> plaintext(ciphertext.size() + 16, 0);
    int len = 0;
    int plaintext_len = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Disable padding for exact-size output
    EVP_CIPHER_CTX_set_padding(ctx, 1);

    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), static_cast<int>(ciphertext.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    plaintext_len = len;

    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    plaintext_len += len;

    EVP_CIPHER_CTX_free(ctx);
    plaintext.resize(static_cast<size_t>(plaintext_len));

    return plaintext;
}

std::vector<uint8_t> OfflineKeyringParser::decode_base64(const std::string& encoded) {
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

std::vector<uint8_t> OfflineKeyringParser::decode_hex(const std::string& hex) {
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

} // namespace innodb
} // namespace dbrecovery