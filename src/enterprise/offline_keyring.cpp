/**
 * @file offline_keyring.cpp
 * @brief OfflineKeyring enterprise implementation
 *
 * Extends innodb::OfflineKeyringParser with multi-keyring inventory,
 * key version management, and key rotation support.
 */

#include "dbrecovery/enterprise/offline_keyring.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/offline_keyring_parser.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <set>

namespace dbrecovery {
namespace enterprise {

// ==================== Multi-Keyring Inventory ====================

size_t OfflineKeyring::scan_keyring_directory(const std::string& dir_path) {
    size_t count = 0;
    last_error_.clear();

    if (!std::filesystem::is_directory(dir_path)) {
        last_error_ = "Not a directory: " + dir_path;
        return 0;
    }

    // Common keyring file name patterns
    static const std::vector<std::string> patterns = {
        "keyring", "keyring_file", "keyring_encrypted", "keyring_data",
        "component_keyring", "mysql_keyring", "*.keyring", "*.krm"
    };

    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;

        std::string path = entry.path().string();
        // Normalize to avoid duplicates
        std::string normalized = std::filesystem::absolute(entry.path()).string();
        if (loaded_keyring_files_.count(normalized)) continue;

        // Check if filename matches a keyring pattern
        bool matches = false;
        std::string fname = entry.path().filename().string();
        for (const auto& pat : patterns) {
            if (fname.find(pat) != std::string::npos) {
                matches = true;
                break;
            }
        }
        if (!matches) continue;

        // Try to parse — try keyring_file first, then encrypted
        bool ok = parser_.parse_keyring_file(path);
        if (!ok) {
            // Might be a different format — we could try encrypted with empty
            // password or other heuristics, but for now skip silently.
            continue;
        }

        loaded_keyring_files_.insert(normalized);
        count++;
    }

    return count;
}

std::optional<MasterKey> OfflineKeyring::find_key_any(const std::string& name_hint) const {
    // Search in the primary parser's keys
    auto key = parser_.find_key(name_hint);
    if (key.has_value()) return key;

    // Fallback: linear scan through all keys
    const auto& all = parser_.get_master_keys();
    for (const auto& mk : all) {
        if (mk.key_name.find(name_hint) != std::string::npos ||
            name_hint.find(mk.key_name) != std::string::npos) {
            return mk;
        }
    }

    return std::nullopt;
}

std::vector<MasterKey> OfflineKeyring::all_keys() const {
    return parser_.get_master_keys();
}

// ==================== Multi-Key Derivation ====================

std::vector<uint8_t> OfflineKeyring::derive_table_key_any(uint32_t space_id,
                                                           uint32_t page_no,
                                                           uint64_t lsn) {
    const auto& keys = parser_.get_master_keys();
    for (const auto& mk : keys) {
        if (!mk.is_valid()) continue;
        auto derived = parser_.derive_table_key(mk, space_id, page_no, lsn);
        if (!derived.empty()) return derived;
    }
    return {};
}

// ==================== Key Version Management ====================

void OfflineKeyring::add_key_version(const KeyVersion& version) {
    // Insert sorted by version_id
    auto it = std::lower_bound(key_versions_.begin(), key_versions_.end(), version,
        [](const KeyVersion& a, const KeyVersion& b) {
            return a.version_id < b.version_id;
        });
    key_versions_.insert(it, version);
}

std::optional<OfflineKeyring::KeyVersion> OfflineKeyring::key_version_at_lsn(uint64_t lsn) const {
    // Find the most recent key version that was active at the given LSN
    // (active_from_lsn <= lsn, and retired_at_lsn > lsn or not set)
    std::optional<KeyVersion> best;
    for (const auto& kv : key_versions_) {
        if (kv.active_from_lsn <= lsn) {
            if (!kv.retired_at_lsn.has_value() || kv.retired_at_lsn.value() > lsn) {
                if (!best.has_value() || kv.version_id > best->version_id) {
                    best = kv;
                }
            }
        }
    }
    return best;
}

// ==================== Key Rotation ====================

std::optional<std::vector<uint8_t>> OfflineKeyring::rotate_table_key(
    uint32_t space_id,
    const MasterKey& new_master_key,
    uint32_t page_no,
    uint64_t lsn) {
    if (!new_master_key.is_valid()) {
        last_error_ = "Cannot rotate: new master key is invalid";
        return std::nullopt;
    }

    // Derive a new table key under the new master key
    auto new_derived = parser_.derive_table_key(new_master_key, space_id, page_no, lsn);
    if (new_derived.empty()) {
        last_error_ = "Key rotation failed: could not derive table key from new master key";
        return std::nullopt;
    }

    // Verify the derived key looks reasonable (non-empty, AES key size)
    if (new_derived.size() < 16) {
        last_error_ = "Key rotation warning: derived key is unusually short (" +
                      std::to_string(new_derived.size()) + " bytes)";
    }

    return new_derived;
}

} // namespace enterprise
} // namespace dbrecovery