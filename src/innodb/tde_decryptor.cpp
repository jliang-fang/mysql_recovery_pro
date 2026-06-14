#include "dbrecovery/innodb/tde_decryptor.hpp"
#include <sstream>
#include <iomanip>
#include <fstream>
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// ==================== TDEDecryptor ====================

std::vector<uint8_t> TDEDecryptor::decrypt_page(const uint8_t* encrypted_data, size_t size,
                                                 uint32_t space_id, uint32_t page_no) {
    if (!encrypted_data || size < 44) {
        stats_.pages_failed++;
        return {};
    }

    // Check if page is encrypted
    if (!is_encrypted(encrypted_data, size)) {
        // Page is not encrypted, return as-is
        return std::vector<uint8_t>(encrypted_data, encrypted_data + size);
    }

    // Parse TDE header
    TDEPageHeader header;
    if (!header.parse(encrypted_data, size)) {
        stats_.pages_failed++;
        return {};
    }

    // Get table key
    uint32_t key_id = header.key_id[0] | (header.key_id[1] << 8) |
                      (header.key_id[2] << 16) | (header.key_id[3] << 24);

    auto it = table_keys_.find(key_id);
    if (it == table_keys_.end()) {
        // Try to derive from primary key
        if (primary_key_.empty()) {
            stats_.pages_failed++;
            return {};
        }

        // Create a TDEKey from primary_key_
        TDEKey primary_key;
        primary_key.key_data = primary_key_;
        primary_key.algorithm = TDEAlgorithm::AES_256_CBC;  // Default

        // Derive table key from primary key
        std::vector<uint8_t> derived_key_data = derive_table_key(primary_key, space_id, page_no);
        
        TDEKey derived_key;
        derived_key.key_data = derived_key_data;
        derived_key.algorithm = TDEAlgorithm::AES_256_CBC;
        derived_key.type = TDEKeyType::TABLE_KEY;
        derived_key.key_id = key_id;
        
        it = table_keys_.insert({key_id, derived_key}).first;
    }

    const TDEKey& key = it->second;

    // Extract ciphertext (skip TDE header)
    std::vector<uint8_t> ciphertext(encrypted_data + 44, encrypted_data + size);

    // Extract IV from header
    std::vector<uint8_t> iv(header.iv, header.iv + 12);

    // Decrypt
    std::vector<uint8_t> plaintext = aes_decrypt(key.key_data, iv, ciphertext);

    if (plaintext.empty()) {
        stats_.pages_failed++;
        return {};
    }

    stats_.pages_decrypted++;
    return plaintext;
}

std::optional<TDEPageHeader> TDEDecryptor::decrypt_page_header(const uint8_t* data, size_t size) {
    if (!data || size < 44) {
        return std::nullopt;
    }

    TDEPageHeader header;
    if (!header.parse(data, size)) {
        return std::nullopt;
    }

    return header;
}

bool TDEDecryptor::verify_checksum(const uint8_t* data, size_t size) {
    if (!data || size < 52) {
        return false;
    }

    // Calculate checksum (simplified - would use CRC32C in production)
    uint32_t checksum = calculate_checksum(data, size);

    // Read stored checksum
    uint32_t stored_checksum = dbrecovery::innodb::read_le<uint32_t>(data, 24);

    return checksum == stored_checksum;
}

std::vector<uint8_t> TDEDecryptor::derive_table_key(const TDEKey& primary_key,
                                                     uint32_t space_id, uint32_t page_no) {
    // Simplified key derivation
    // In production, would use proper key derivation function (KDF)

    std::vector<uint8_t> derived_key(primary_key.key_data);

    // Mix in space_id and page_no
    uint8_t space_bytes[4];
    uint8_t page_bytes[4];

    for (int i = 0; i < 4; ++i) {
        space_bytes[i] = (space_id >> (i * 8)) & 0xFF;
        page_bytes[i] = (page_no >> (i * 8)) & 0xFF;
    }

    // XOR with space_id and page_no (simplified)
    for (int i = 0; i < 4 && i < static_cast<int>(derived_key.size()); ++i) {
        derived_key[i] ^= space_bytes[i];
        derived_key[i + 4] ^= page_bytes[i];
    }

    return derived_key;
}

std::vector<uint8_t> TDEDecryptor::aes_decrypt(const std::vector<uint8_t>& key,
                                                const std::vector<uint8_t>& iv,
                                                const std::vector<uint8_t>& ciphertext) {
    if (ciphertext.empty() || key.empty()) {
        return {};
    }

    // Ensure IV is 16 bytes (AES block size)
    std::vector<uint8_t> padded_iv(16, 0);
    size_t iv_copy_size = std::min(iv.size(), static_cast<size_t>(16));
    std::copy(iv.begin(), iv.begin() + iv_copy_size, padded_iv.begin());

    // Determine key size and cipher type
    const EVP_CIPHER* cipher = nullptr;
    switch (key.size()) {
        case 16:
            cipher = EVP_aes_128_cbc();
            break;
        case 24:
            cipher = EVP_aes_192_cbc();
            break;
        case 32:
            cipher = EVP_aes_256_cbc();
            break;
        default:
            // Invalid key size
            return {};
    }

    // Create decryption context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return {};
    }

    std::vector<uint8_t> plaintext(ciphertext.size() + AES_BLOCK_SIZE);
    int len = 0;
    int plaintext_len = 0;

    // Initialize decryption
    if (EVP_DecryptInit_ex(ctx, cipher, nullptr, key.data(), padded_iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Disable padding check for partial blocks
    EVP_CIPHER_CTX_set_padding(ctx, 0);

    // Decrypt
    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ciphertext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    plaintext_len = len;

    // Finalize
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    plaintext_len += len;

    EVP_CIPHER_CTX_free(ctx);

    plaintext.resize(plaintext_len);
    return plaintext;
}

uint32_t TDEDecryptor::calculate_checksum(const uint8_t* data, size_t size) {
    // Simplified checksum calculation
    // In production, would use CRC32C

    uint32_t checksum = 0;
    for (size_t i = 0; i < size; i += 4) {
        if (i + 4 <= size) {
            checksum ^= dbrecovery::innodb::read_le<uint32_t>(data, i);
        }
    }

    return checksum;
}

} // namespace innodb
} // namespace dbrecovery
