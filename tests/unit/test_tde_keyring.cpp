/**
 * @file test_tde_keyring.cpp
 * @brief Unit tests for TDE keyring loading functionality
 */

#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include "dbrecovery/recovery/engine.hpp"

using namespace dbrecovery;

// Helper to create a minimal keyring file for testing
class TDEKeyringTest : public ::testing::Test {
protected:
    std::string test_dir_;
    std::string keyring_path_;

    void SetUp() override {
        test_dir_ = "/tmp/tde_keyring_test_" + std::to_string(getpid());
        std::filesystem::create_directories(test_dir_);
        keyring_path_ = test_dir_ + "/keyring";
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }

    // Create a test keyring file with XOR obfuscation
    void create_test_keyring() {
        // Keyring format: header (24 bytes) + entries + "EOF"
        std::vector<uint8_t> data;

        // Header (24 bytes of zeros)
        data.resize(24, 0);

        // XOR key (from MySQL source)
        const char* xor_key = "*305=Ljt0*!@$Hnm(*-9-w;:";
        const size_t xor_key_len = 24;

        // Entry 1: INNODBKey-test-uuid-1
        {
            std::string key_id = "INNODBKey-test-uuid-1";
            std::string key_type = "AES256";
            std::string user_id = "test_user";
            std::vector<uint8_t> key_bytes(32, 0xAB);  // 32-byte AES-256 key

            // XOR encrypt the key
            std::vector<uint8_t> encrypted_key(key_bytes.size());
            for (size_t i = 0; i < key_bytes.size(); i++) {
                encrypted_key[i] = key_bytes[i] ^ static_cast<uint8_t>(xor_key[i % xor_key_len]);
            }

            // Entry header
            uint64_t total_length = 40 + key_id.size() + key_type.size() + user_id.size() + key_bytes.size();
            uint64_t key_id_length = key_id.size();
            uint64_t key_type_length = key_type.size();
            uint64_t user_id_length = user_id.size();
            uint64_t key_length = key_bytes.size();

            // Append entry header (little-endian)
            for (int i = 0; i < 8; i++) {
                data.push_back(static_cast<uint8_t>((total_length >> (i * 8)) & 0xFF));
            }
            for (int i = 0; i < 8; i++) {
                data.push_back(static_cast<uint8_t>((key_id_length >> (i * 8)) & 0xFF));
            }
            for (int i = 0; i < 8; i++) {
                data.push_back(static_cast<uint8_t>((key_type_length >> (i * 8)) & 0xFF));
            }
            for (int i = 0; i < 8; i++) {
                data.push_back(static_cast<uint8_t>((user_id_length >> (i * 8)) & 0xFF));
            }
            for (int i = 0; i < 8; i++) {
                data.push_back(static_cast<uint8_t>((key_length >> (i * 8)) & 0xFF));
            }

            // Append key_id
            for (char c : key_id) {
                data.push_back(static_cast<uint8_t>(c));
            }

            // Append key_type
            for (char c : key_type) {
                data.push_back(static_cast<uint8_t>(c));
            }

            // Append user_id
            for (char c : user_id) {
                data.push_back(static_cast<uint8_t>(c));
            }

            // Append encrypted key
            for (uint8_t b : encrypted_key) {
                data.push_back(b);
            }

            // Align to 8 bytes
            while (data.size() % 8 != 0) {
                data.push_back(0);
            }
        }

        // EOF marker
        data.push_back('E');
        data.push_back('O');
        data.push_back('F');

        // Write to file
        std::ofstream file(keyring_path_, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        file.close();
    }
};

TEST_F(TDEKeyringTest, LoadKeyringFile) {
    create_test_keyring();

    innodb::KeyringFile keyring;
    ASSERT_TRUE(keyring.load(keyring_path_)) << "Failed to load keyring file";

    EXPECT_EQ(keyring.entries.size(), 1) << "Should have 1 key entry";

    if (!keyring.entries.empty()) {
        const auto& entry = keyring.entries[0];
        EXPECT_EQ(entry.key_name, "INNODBKey-test-uuid-1");
        EXPECT_EQ(entry.key_data.size(), 32) << "Key should be 32 bytes";
        EXPECT_EQ(entry.algorithm, innodb::TDEAlgorithm::AES_256_CBC);
    }
}

TEST_F(TDEKeyringTest, FindKeyByName) {
    create_test_keyring();

    innodb::KeyringFile keyring;
    ASSERT_TRUE(keyring.load(keyring_path_));

    auto entry = keyring.find_key("INNODBKey-test-uuid-1");
    EXPECT_TRUE(entry.has_value()) << "Should find key by name";

    auto not_found = keyring.find_key("nonexistent-key");
    EXPECT_FALSE(not_found.has_value()) << "Should not find nonexistent key";
}

TEST_F(TDEKeyringTest, RecoveryEngineLoadKeyring) {
    create_test_keyring();

    recovery::RecoveryEngine engine;
    EXPECT_TRUE(engine.load_tde_keyring(keyring_path_)) << "Engine should load keyring";
}

TEST_F(TDEKeyringTest, SetTDEPrimaryKey) {
    recovery::RecoveryEngine engine;

    // Valid hex key
    EXPECT_TRUE(engine.set_tde_primary_key("0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"));

    // With 0x prefix
    EXPECT_TRUE(engine.set_tde_primary_key("0x0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"));

    // With spaces
    EXPECT_TRUE(engine.set_tde_primary_key("01 23 45 67 89 AB CD EF 01 23 45 67 89 AB CD EF 01 23 45 67 89 AB CD EF 01 23 45 67 89 AB CD EF"));
}

TEST_F(TDEKeyringTest, SetTDEPrimaryKeyInvalid) {
    recovery::RecoveryEngine engine;

    // Odd length hex string (invalid)
    EXPECT_FALSE(engine.set_tde_primary_key("0123456789ABCDE"));

    // Too short
    EXPECT_FALSE(engine.set_tde_primary_key("0123456789ABCDEF"));
}

TEST_F(TDEKeyringTest, AddTDETableKey) {
    recovery::RecoveryEngine engine;

    EXPECT_TRUE(engine.add_tde_table_key(1, "0123456789ABCDEF0123456789ABCDEF"));
    EXPECT_TRUE(engine.add_tde_table_key(2, "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"));
}

TEST_F(TDEKeyringTest, TDEDecryptorBasicOperations) {
    innodb::TDEDecryptor decryptor;

    // Set primary key
    std::vector<uint8_t> primary_key(32, 0xAA);
    decryptor.set_primary_key(primary_key);

    // Add table key
    std::vector<uint8_t> table_key(32, 0xBB);
    decryptor.add_table_key(1, table_key, innodb::TDEAlgorithm::AES_256_CBC);

    auto stats = decryptor.get_stats();
    EXPECT_EQ(stats.keys_loaded, 1);
}

TEST_F(TDEKeyringTest, TDEKeyValidation) {
    innodb::TDEKey key;
    key.algorithm = innodb::TDEAlgorithm::AES_256_CBC;

    // Empty key is invalid
    EXPECT_FALSE(key.is_valid());

    // Valid 16-byte key
    key.key_data.resize(16, 0xAB);
    EXPECT_TRUE(key.is_valid());

    // Valid 32-byte key
    key.key_data.resize(32, 0xAB);
    EXPECT_TRUE(key.is_valid());
}

TEST_F(TDEKeyringTest, TDEPageHeaderParsing) {
    // Create minimal TDE page header (45 bytes to be safe)
    std::vector<uint8_t> header_data(45, 0);

    // Algorithm (AES-256-CBC = 6)
    header_data[0] = 6;

    // Key ID (4 bytes)
    header_data[1] = 0x01;
    header_data[2] = 0x00;
    header_data[3] = 0x00;
    header_data[4] = 0x00;

    // IV (12 bytes)
    for (int i = 0; i < 12; i++) {
        header_data[5 + i] = static_cast<uint8_t>(i);
    }

    // Encrypted header (28 bytes)
    for (int i = 0; i < 28; i++) {
        header_data[17 + i] = static_cast<uint8_t>(i);
    }

    innodb::TDEPageHeader header;
    EXPECT_TRUE(header.parse(header_data.data(), header_data.size()));
    EXPECT_EQ(header.algorithm, 6);
}

TEST_F(TDEKeyringTest, IsEncryptedDetection) {
    // The TDE header starts at offset 38 (right after the FIL header).
    // FIL header at offset 24 has page_type (big-endian), and TDE algorithm byte is at data[38].
    // Non-encrypted page (algorithm byte = 0 at TDE header offset)
    std::vector<uint8_t> normal_page(100, 0);
    // Set a valid page type so is_encrypted proceeds to check the TDE byte
    normal_page[24] = 0x45;  // FIL_PAGE_INDEX (big-endian)
    normal_page[25] = 0xBF;
    EXPECT_FALSE(innodb::TDEDecryptor::is_encrypted(normal_page.data(), normal_page.size()));

    // Encrypted page (algorithm byte = 1-9 at TDE header offset 38)
    normal_page[38] = 6;  // AES-256-CBC
    EXPECT_TRUE(innodb::TDEDecryptor::is_encrypted(normal_page.data(), normal_page.size()));
}