#ifndef DBRECOVERY_INNODB_TDE_DECRYPTOR_HPP
#define DBRECOVERY_INNODB_TDE_DECRYPTOR_HPP

#include <vector>
#include <cstdint>
#include <optional>
#include <map>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <fstream>

#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief TDE 密钥类型
 */
enum class TDEKeyType : uint8_t {
    PRIMARY_KEY = 0x01,      // 主密钥
    TABLE_KEY = 0x02,        // 表密钥
    KEYRING_KEY = 0x03,      // Keyring 密钥
};

/**
 * @brief TDE 加密算法
 */
enum class TDEAlgorithm : uint8_t {
    AES_128_ECB = 0x01,
    AES_192_ECB = 0x02,
    AES_256_ECB = 0x03,
    AES_128_CBC = 0x04,
    AES_192_CBC = 0x05,
    AES_256_CBC = 0x06,
    AES_128_CTR = 0x07,
    AES_192_CTR = 0x08,
    AES_256_CTR = 0x09,
};

/**
 * @brief TDE 密钥结构
 */
struct TDEKey {
    TDEKeyType type;
    TDEAlgorithm algorithm;
    std::vector<uint8_t> key_data;
    std::vector<uint8_t> iv;      // 初始化向量 (CBC/CTR 模式)
    std::vector<uint8_t> salt;    // 盐值

    // 密钥元数据
    uint32_t key_id;
    uint32_t rotation_count;
    uint64_t created_at;
    uint64_t rotated_at;

    bool is_valid() const;
    size_t key_size() const;
    size_t iv_size() const;
};

/**
 * @brief TDE 页头 (加密页)
 */
struct TDEPageHeader {
    // 加密元数据
    uint8_t algorithm;      // 加密算法
    uint8_t key_id[4];      // 密钥ID
    uint8_t iv[12];         // 初始化向量

    // 原始页头 (加密后)
    uint8_t encrypted_header[28];  // 加密的页头数据

    bool parse(const uint8_t* data, size_t size);
    std::string to_string() const;
};

/**
 * Candidate-routing outcome for the current TDE heuristic. This is not
 * authoritative encryption identity or metadata validation.
 */
enum class TDECandidateProbe : uint8_t {
    NOT_DETECTED,
    RECOGNIZED_CANDIDATE,
};

/**
 * @brief TDE 解密器
 */
class TDEDecryptor {
public:
    TDEDecryptor();

    /**
     * @brief 设置主密钥
     */
    void set_primary_key(const std::vector<uint8_t>& key);

    /**
     * @brief 添加表密钥
     */
    void add_table_key(uint32_t key_id, const std::vector<uint8_t>& key,
                       TDEAlgorithm algorithm);

    /**
     * @brief 从 Keyring 文件加载密钥
     */
    bool load_keyring(const std::string& keyring_path);

    /**
     * @brief 解密页数据
     */
    std::vector<uint8_t> decrypt_page(const uint8_t* encrypted_data, size_t size,
                                      uint32_t space_id, uint32_t page_no);

    /**
     * @brief 解密页头
     */
    std::optional<TDEPageHeader> decrypt_page_header(const uint8_t* data, size_t size);

    /**
     * @brief 验证解密后的页校验和
     */
    bool verify_checksum(const uint8_t* data, size_t size);

    /**
     * @brief Probe whether a page matches the unvalidated TDE candidate heuristic
     */
    static TDECandidateProbe probe_candidate(const uint8_t* data, size_t size);

    /**
     * @brief Check whether a page must be routed as a TDE candidate
     *
     * A true result does not confirm encryption; it prevents candidate bytes
     * from being treated as plaintext by generic recovery.
     */
    static bool is_encrypted(const uint8_t* data, size_t size);

    /**
     * @brief 获取解密统计信息
     */
    struct Stats {
        size_t pages_decrypted;
        size_t pages_failed;
        size_t keys_loaded;
    };

    Stats get_stats() const;

private:
    std::vector<uint8_t> primary_key_;
    std::map<uint32_t, TDEKey> table_keys_;
    Stats stats_;

    std::vector<uint8_t> derive_table_key(const TDEKey& primary_key,
                                          uint32_t space_id, uint32_t page_no);
    std::vector<uint8_t> aes_decrypt(const std::vector<uint8_t>& key,
                                     const std::vector<uint8_t>& iv,
                                     const std::vector<uint8_t>& ciphertext);
    uint32_t calculate_checksum(const uint8_t* data, size_t size);
};

/**
 * @brief Keyring 文件格式
 */
struct KeyringFile {
    struct Entry {
        std::string key_name;    // e.g., "INNODBKey-uuid-123"
        TDEKeyType type;
        TDEAlgorithm algorithm;
        std::vector<uint8_t> key_data;
        std::vector<uint8_t> metadata;
    };

    std::vector<Entry> entries;

    bool load(const std::string& path);
    std::optional<Entry> find_key(const std::string& key_name) const;
};

// ==================== 实现 ====================

inline TDEDecryptor::TDEDecryptor() {
    stats_ = {0, 0, 0};
}

inline TDEDecryptor::Stats TDEDecryptor::get_stats() const {
    return stats_;
}

inline bool TDEKey::is_valid() const {
    return !key_data.empty() && key_data.size() >= 16;
}

inline size_t TDEKey::key_size() const {
    return key_data.size();
}

inline size_t TDEKey::iv_size() const {
    switch (algorithm) {
        case TDEAlgorithm::AES_128_CBC:
        case TDEAlgorithm::AES_192_CBC:
        case TDEAlgorithm::AES_256_CBC:
        case TDEAlgorithm::AES_128_CTR:
        case TDEAlgorithm::AES_192_CTR:
        case TDEAlgorithm::AES_256_CTR:
            return 16;  // AES block size
        default:
            return 0;
    }
}

// ==================== TDEPageHeader ====================

inline bool TDEPageHeader::parse(const uint8_t* data, size_t size) {
    if (size < 45) {  // 1 + 4 + 12 + 28 = 45
        return false;
    }

    algorithm = data[0];
    std::memcpy(key_id, data + 1, 4);
    std::memcpy(iv, data + 5, 12);
    std::memcpy(encrypted_header, data + 17, 28);

    return true;
}

inline std::string TDEPageHeader::to_string() const {
    std::ostringstream oss;
    oss << "TDEPageHeader{"
        << "algo=" << static_cast<int>(algorithm)
        << ", key_id=" << (key_id[0] | (key_id[1] << 8) | (key_id[2] << 16) | (key_id[3] << 24))
        << ", iv=" << std::hex;
    for (int i = 0; i < 12; i++) {
        oss << std::setw(2) << std::setfill('0') << static_cast<int>(iv[i]);
    }
    oss << std::dec << "}";
    return oss.str();
}

// ==================== TDEDecryptor ====================

inline void TDEDecryptor::set_primary_key(const std::vector<uint8_t>& key) {
    primary_key_ = key;
}

inline void TDEDecryptor::add_table_key(uint32_t key_id, const std::vector<uint8_t>& key,
                                        TDEAlgorithm algorithm) {
    TDEKey tde_key;
    tde_key.type = TDEKeyType::TABLE_KEY;
    tde_key.algorithm = algorithm;
    tde_key.key_data = key;
    tde_key.key_id = key_id;

    table_keys_[key_id] = tde_key;
    stats_.keys_loaded++;
}

inline TDECandidateProbe TDEDecryptor::probe_candidate(const uint8_t* data, size_t size) {
    constexpr size_t kTDEHeaderSize = 45;
    if (data == nullptr || size < FIL_HEADER_SIZE + kTDEHeaderSize) {
        return TDECandidateProbe::NOT_DETECTED;
    }

    const uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI &&
        page_type != FIL_PAGE_SDI_ZIP && page_type != 0) {
        return TDECandidateProbe::NOT_DETECTED;
    }

    const uint8_t algorithm = data[FIL_HEADER_SIZE];
    if (algorithm < 1 || algorithm > 9) {
        return TDECandidateProbe::NOT_DETECTED;
    }

    for (size_t i = 0; i < std::min(size, size_t(100)); ++i) {
        if (data[i] != 0) {
            return TDECandidateProbe::RECOGNIZED_CANDIDATE;
        }
    }
    return TDECandidateProbe::NOT_DETECTED;
}

inline bool TDEDecryptor::is_encrypted(const uint8_t* data, size_t size) {
    return probe_candidate(data, size) == TDECandidateProbe::RECOGNIZED_CANDIDATE;
}

// ==================== KeyringFile ====================

/**
 * @brief Native binary MySQL keyring loader — UNSUPPORTED, fails closed
 *
 * MySQL's native binary keyring file obfuscates key material with a fixed
 * XOR scheme. This project no longer accepts that XOR scheme for key
 * material, and native binary keyrings do not expose the numeric key IDs
 * that encrypted InnoDB pages require in their TDE header. Consequently the
 * loader NEVER decodes key bytes: it clears any prior entries and returns
 * false regardless of input.
 *
 * Supported keyring paths are OfflineKeyringParser (JSON, encrypted
 * JSON AES-256-CBC) and Vault exports that carry a numeric master_key_id.
 */
inline bool KeyringFile::load(const std::string& /*path*/) {
    entries.clear();
    return false;
}

inline std::optional<KeyringFile::Entry> KeyringFile::find_key(const std::string& key_name) const {
    for (const auto& entry : entries) {
        if (entry.key_name == key_name) {
            return entry;
        }
    }
    return std::nullopt;
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_TDE_DECRYPTOR_HPP
