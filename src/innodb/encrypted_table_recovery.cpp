/**
 * @file encrypted_table_recovery.cpp
 * @brief Encrypted table recovery implementations
 *
 * Extracted from encrypted_table_recovery.hpp for better compilation.
 */

#include "dbrecovery/innodb/encrypted_table_recovery.hpp"
#include "dbrecovery/innodb/offline_keyring_parser.hpp"

#include <cstring>
#include <chrono>
#include <fstream>

namespace dbrecovery {
namespace innodb {

// ==================== EncryptedTableRecovery ====================

EncryptedTableRecovery::EncryptedTableRecovery(size_t page_size)
    : page_size_(page_size), try_recovery_without_key_(false) {
    std::memset(&stats_, 0, sizeof(stats_));
}

void EncryptedTableRecovery::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

void EncryptedTableRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

void EncryptedTableRecovery::set_primary_key(const std::vector<uint8_t>& key) {
    decryptor_.set_primary_key(key);
}

void EncryptedTableRecovery::add_table_key(uint32_t key_id, const std::vector<uint8_t>& key,
                                            TDEAlgorithm algorithm) {
    decryptor_.add_table_key(key_id, key, algorithm);
}

bool EncryptedTableRecovery::configure_keys(const std::vector<MasterKey>& keys) {
    if (keys.empty()) {
        last_error_ = "No usable key material provided";
        return false;
    }

    std::set<uint32_t> key_ids;
    for (const auto& key : keys) {
        const size_t key_size = key.key_data.size();
        if (!key.is_valid() || key.key_id == 0 ||
            (key_size != 16 && key_size != 24 && key_size != 32)) {
            last_error_ = "Keyring contains invalid key material or key ID";
            return false;
        }
        if (!key_ids.insert(key.key_id).second) {
            last_error_ = "Keyring contains duplicate key IDs";
            return false;
        }
    }

    for (const auto& key : keys) {
        decryptor_.add_table_key(key.key_id, key.key_data, key.algorithm);
    }
    last_error_.clear();
    return true;
}

bool EncryptedTableRecovery::load_keyring(const std::string& keyring_path) {
    OfflineKeyringParser parser;
    if (!parser.parse_keyring_file(keyring_path)) {
        last_error_ = parser.get_last_error();
        return false;
    }

    return configure_keys(parser.get_master_keys());
}

void EncryptedTableRecovery::set_try_recovery_without_key(bool try_recovery) {
    try_recovery_without_key_ = try_recovery;
}

void EncryptedTableRecovery::clear() {
    encrypted_pages_.clear();
    failed_pages_.clear();
    records_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

std::vector<ScannedPage> EncryptedTableRecovery::scan_pages() {
    auto start = std::chrono::high_resolution_clock::now();

    encrypted_pages_.clear();
    std::vector<ScannedPage> pages;

    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return pages;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    size_t total_pages = file_size / page_size_;
    stats_.total_pages = total_pages;

    if (total_pages == 0) {
        return pages;
    }

    // Deterministic fixed-size reads; FIL fields parsed with canonical
    // big-endian offsets from page_defs.hpp.
    std::vector<uint8_t> page_buf(page_size_);
    for (size_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(static_cast<std::streamoff>(page_no * page_size_), std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buf.data()),
                  static_cast<std::streamsize>(page_size_));
        if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
            break;
        }

        const uint8_t* p = page_buf.data();
        ScannedPage page{};
        page.offset = static_cast<uint32_t>(page_no * page_size_);
        page.physical_page_index = static_cast<uint32_t>(page_no);
        page.space_id = read_be<uint32_t>(p, FIL_PAGE_SPACE_ID);
        page.page_no = read_be<uint32_t>(p, FIL_PAGE_OFFSET);
        page.prev_page = read_be<uint32_t>(p, FIL_PAGE_PREV_ABS);
        page.next_page = read_be<uint32_t>(p, FIL_PAGE_NEXT_ABS);
        page.lsn = read_be<uint64_t>(p, FIL_PAGE_LSN_ABS);
        page.type = read_be<uint16_t>(p, FIL_PAGE_TYPE_ABS);
        page.page_type = page.type;
        page.is_valid = PageScanner::is_innodb_page(p, page_size_);

        // Identity validation: the header page_no must match the physical
        // position in the file.  A mismatch indicates page-level corruption,
        // misaligned scan, or cross-file reassembly.  Reject such pages
        // rather than feeding wrong-key decryption or incorrect parsing.
        if (page.is_valid && page.page_no != page.physical_page_index) {
            page.is_valid = false;
        }

        if (is_page_encrypted(p, page_size_)) {
            page.is_encrypted = true;
            encrypted_pages_.push_back(page.page_no);
        }

        pages.push_back(page);
    }

    stats_.encrypted_pages = encrypted_pages_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.decrypt_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return pages;
}

bool EncryptedTableRecovery::is_page_encrypted(const uint8_t* page_data, size_t page_size) {
    return TDEDecryptor::is_encrypted(page_data, page_size);
}

std::vector<uint8_t> EncryptedTableRecovery::decrypt_page_data(const uint8_t* encrypted_data,
                                                                size_t size, uint32_t page_no,
                                                                uint32_t space_id) {
    auto start = std::chrono::high_resolution_clock::now();

    auto decrypted = decryptor_.decrypt_page(encrypted_data, size, space_id, page_no);

    auto end = std::chrono::high_resolution_clock::now();
    stats_.decrypt_time_ms += std::chrono::duration<double, std::milli>(end - start).count();

    if (decrypted.empty()) {
        failed_pages_.push_back(page_no);
    } else {
        stats_.decrypted_pages++;
    }

    return decrypted;
}

std::vector<std::map<std::string, std::string>> EncryptedTableRecovery::parse_page_records(
    const std::vector<ScannedPage>& pages) {

    auto start = std::chrono::high_resolution_clock::now();

    records_.clear();

    for (const auto& page : pages) {
        if (!page.is_valid) {
            continue;
        }

        std::ifstream file(file_path_, std::ios::binary);
        if (!file) continue;

        size_t offset = static_cast<size_t>(page.physical_page_index) * page_size_;
        file.seekg(offset, std::ios::beg);

        std::vector<uint8_t> page_data(page_size_);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
            continue;
        }

        std::vector<uint8_t> working_data = page_data;
        if (page.is_encrypted) {
            working_data = decrypt_page_data(page_data.data(), page_data.size(), page.page_no, page.space_id);
            if (working_data.empty()) {
                if (!try_recovery_without_key_) {
                    continue;
                }
                // Security: never feed undecrypted ciphertext to the B+Tree parser.
                // Without a verified key, the page content is unknowable — emitting
                // it as structured rows would fabricate data from AES ciphertext.
                std::cerr << "[WARN] Page " << page.page_no
                          << " could not be decrypted (no key); skipping page\n";
                continue;
            }
        }

        BPlusTreeParser parser(working_data);
        if (!parser.parse()) {
            continue;
        }

        auto page_records = parser.parse_records(schema_);
        records_.insert(records_.end(), page_records.begin(), page_records.end());
    }

    stats_.recovered_records = records_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_time_ms = stats_.decrypt_time_ms + stats_.recovery_time_ms;

    return records_;
}

std::vector<std::map<std::string, std::string>> EncryptedTableRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // Without a fixture-validated TDE decryptor, report ENCRYPTED_UNSUPPORTED
    // and emit zero records. This prevents claiming success on an empty result
    // set or, worse, fabricating structured rows from unverified decryption.
    if (!decryptor_is_validated()) {
        stats_.tde_status = TDEStatus::ENCRYPTED_UNSUPPORTED;
        auto total_end = std::chrono::high_resolution_clock::now();
        stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
        return records_;
    }

    auto all_pages = scan_pages();

    records_ = parse_page_records(all_pages);

    stats_.tde_status = records_.empty()
        ? TDEStatus::ENCRYPTED_UNSUPPORTED
        : TDEStatus::RECOVERED;

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

bool EncryptedTableRecovery::decryptor_is_validated() {
    // No fixture-validated TDE decryptor exists yet.  Return false so that
    // recover() for encrypted layout reports ENCRYPTED_UNSUPPORTED instead of
    // claiming successful recovery.  Real MySQL encrypted fixtures (known-answer
    // tests with ground-truth plaintext, key, IV, and page metadata) are required
    // before this gate can be opened.
    return false;
}

TDEStatus EncryptedTableRecovery::classify(const std::string& file_path, size_t page_size) const {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return TDEStatus::MALFORMED;

    std::vector<uint8_t> page0(page_size, 0);
    file.read(reinterpret_cast<char*>(page0.data()), page0.size());
    const size_t bytes_read = static_cast<size_t>(file.gcount());
    if (bytes_read < 83) return TDEStatus::MALFORMED;  // FIL header (38) + TDE header (45) min

    const auto probe = TDEDecryptor::probe_candidate(page0.data(), bytes_read);
    if (probe == TDECandidateProbe::RECOGNIZED_CANDIDATE) {
        return TDEStatus::ENCRYPTED_UNSUPPORTED;
    }

    return TDEStatus::NOT_ENCRYPTED;
}

// ==================== EncryptedTableRecoveryManager ====================

EncryptedTableRecoveryManager::EncryptedTableRecoveryManager() {}

void EncryptedTableRecoveryManager::add_encrypted_table(const std::string& table_name,
                                                         const std::string& tablespace_id,
                                                         uint32_t key_id) {
    EncryptedTableInfo info;
    info.table_name = table_name;
    info.tablespace_id = tablespace_id;
    info.key_id = key_id;
    info.algorithm = TDEAlgorithm::AES_256_CBC;
    info.has_key = false;

    tables_[tablespace_id] = info;
}

void EncryptedTableRecoveryManager::set_table_key(const std::string& tablespace_id,
                                                   uint32_t key_id,
                                                   const std::vector<uint8_t>& key,
                                                   TDEAlgorithm algorithm) {
    auto it = tables_.find(tablespace_id);
    if (it != tables_.end()) {
        it->second.key_id = key_id;
        it->second.algorithm = algorithm;
        it->second.has_key = true;
    }
    table_keys_[tablespace_id] = key;
}

void EncryptedTableRecoveryManager::set_primary_key(const std::vector<uint8_t>& key) {
    primary_key_ = key;
}

std::optional<uint32_t> EncryptedTableRecoveryManager::get_table_key_id(
    const std::string& tablespace_id) const {
    auto it = tables_.find(tablespace_id);
    if (it != tables_.end()) {
        return it->second.key_id;
    }
    return std::nullopt;
}

std::optional<EncryptedTableRecoveryManager::EncryptedTableInfo>
EncryptedTableRecoveryManager::parse_encryption_info(const std::string& ibd_path) {
    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 16384) {
        return std::nullopt;
    }

    std::vector<uint8_t> page_data(16384);
    file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

    // Candidate recognition is only a routing safeguard. Without an authoritative,
    // fixture-validated format parser, bytes at FIL_PAGE_DATA cannot become trusted
    // encryption metadata.
    return std::nullopt;
}

} // namespace innodb
} // namespace dbrecovery