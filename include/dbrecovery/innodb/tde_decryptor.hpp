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
     * @brief 检查页是否加密
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

inline bool TDEDecryptor::is_encrypted(const uint8_t* data, size_t size) {
    if (size < 44) return false;

    // TDE page header starts at byte 0 of the page body (after FIL header at offset 38)
    // The FIL header's page type field tells us if this is an index page
    // For TDE detection, we look at the encryption algorithm byte at FIL_PAGE_DATA+0
    // which is at offset 38 from page start

    // First validate this looks like an InnoDB page: check FIL_PAGE_TYPE
    uint16_t page_type = read_be<uint16_t>(data, 24);  // FIL_PAGE_TYPE at offset 24
    if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_SDI &&
        page_type != FIL_PAGE_SDI_ZIP && page_type != 0) {
        // Not a recognizable page type, skip encryption check
        return false;
    }

    // Check encryption algorithm byte at TDE header offset
    // TDE header: 1 byte algorithm | 1 byte key_id | 4 bytes key_version | 16 bytes IV | ...
    // The algorithm field values: 1=AES-128, 2=AES-192, 3=AES-256
    uint8_t algo = data[38];  // First byte after FIL header (38 bytes)
    if (algo >= 1 && algo <= 9) {
        // Additional validation: check that the page size is reasonable
        // and that we're not looking at random data
        uint32_t checksum = read_le<uint32_t>(data, 0);
        // For encrypted pages, the FIL checksum may be 0 or stored differently
        // but the page should not be all zeros
        bool all_zero = true;
        for (size_t i = 0; i < std::min(size, size_t(100)); i++) {
            if (data[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) return false;

        return true;
    }

    return false;
}

// ==================== KeyringFile ====================

/**
 * @brief MySQL Keyring file format
 *
 * MySQL Keyring stores encryption keys with XOR obfuscation
 */
inline bool KeyringFile::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    // Read entire file
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 27) {  // Minimum: header + "EOF"
        return false;
    }

    std::vector<uint8_t> data(file_size);
    file.read(reinterpret_cast<char*>(data.data()), file_size);

    // XOR decryption key (from MySQL source)
    const char* xor_key = "*305=Ljt0*!@$Hnm(*-9-w;:";
    const size_t xor_key_len = 24;

    // Start parsing after header (offset 24)
    size_t offset = 24;

    entries.clear();

    while (offset + 3 < file_size) {
        // Check for EOF marker
        if (data[offset] == 'E' && data[offset+1] == 'O' && data[offset+2] == 'F') {
            break;
        }

        // Entry header: 5 x uint64 (total_length, key_id_length, key_type_length, user_id_length, key_length)
        if (offset + 40 > file_size) {
            break;
        }

        uint64_t total_length = read_le<uint64_t>(data.data(), offset);
        uint64_t key_id_length = read_le<uint64_t>(data.data(), offset + 8);
        uint64_t key_type_length = read_le<uint64_t>(data.data(), offset + 16);
        uint64_t user_id_length = read_le<uint64_t>(data.data(), offset + 24);
        uint64_t key_length = read_le<uint64_t>(data.data(), offset + 32);

        offset += 40;

        // Sanity check
        if (key_id_length > 256 || key_type_length > 64 ||
            key_length > 256 || offset + key_id_length + key_type_length + user_id_length + key_length > file_size) {
            break;
        }

        // Read key_id (string)
        std::string key_id(data.data() + offset, data.data() + offset + key_id_length);
        offset += key_id_length;

        // Read key_type (string)
        std::string key_type(data.data() + offset, data.data() + offset + key_type_length);
        offset += key_type_length;

        // Skip user_id
        offset += user_id_length;

        // Read key (XOR encrypted)
        std::vector<uint8_t> encrypted_key(data.data() + offset, data.data() + offset + key_length);
        offset += key_length;

        // XOR decrypt
        std::vector<uint8_t> decrypted_key(encrypted_key.size());
        for (size_t i = 0; i < encrypted_key.size(); i++) {
            decrypted_key[i] = encrypted_key[i] ^ static_cast<uint8_t>(xor_key[i % xor_key_len]);
        }

        // Create entry
        Entry entry;
        entry.key_name = key_id;  // Keep as string (e.g., "INNODBKey-uuid-123")
        entry.type = TDEKeyType::KEYRING_KEY;

        // Determine algorithm from key_type
        if (key_type.find("AES") != std::string::npos) {
            if (key_type.find("256") != std::string::npos) {
                entry.algorithm = TDEAlgorithm::AES_256_CBC;
            } else if (key_type.find("192") != std::string::npos) {
                entry.algorithm = TDEAlgorithm::AES_192_CBC;
            } else {
                entry.algorithm = TDEAlgorithm::AES_128_CBC;
            }
        } else {
            entry.algorithm = TDEAlgorithm::AES_256_CBC;  // Default
        }
        entry.key_data = decrypted_key;

        entries.push_back(entry);

        // Align to 8 bytes
        if (offset % 8 != 0) {
            offset += 8 - (offset % 8);
        }
    }

    return !entries.empty();
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
