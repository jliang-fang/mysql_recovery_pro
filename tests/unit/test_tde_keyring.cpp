/**
 * @file test_tde_keyring.cpp
 * @brief Unit tests for TDE keyring loading functionality
 */

#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <stdexcept>
#include <cstdint>
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include "dbrecovery/innodb/offline_keyring_parser.hpp"
#include "dbrecovery/innodb/encrypted_table_recovery.hpp"
#include "dbrecovery/innodb/compressed_table_recovery.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/recovery/engine.hpp"

using namespace dbrecovery;

// Helper to create a minimal keyring file for testing
class TDEKeyringTest : public ::testing::Test {
protected:
    std::string test_dir_;
    std::string keyring_path_;

    void SetUp() override {
        const char* root = std::getenv("DBREC_TEST_TMPDIR");
        if (!root || *root == '\0') {
            throw std::runtime_error("DBREC_TEST_TMPDIR must be set");
        }
        test_dir_ = std::string(root) + "/tde_keyring_test_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this));
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

    std::string write_json_keyring(const std::string& name, const std::string& json) {
        const std::string path = test_dir_ + "/" + name;
        std::ofstream file(path);
        file << json;
        return path;
    }
};

TEST_F(TDEKeyringTest, LoadKeyringFileRejected) {
    create_test_keyring();

    innodb::KeyringFile keyring;
    // Native binary keyring loading is unsupported — always fails closed
    ASSERT_FALSE(keyring.load(keyring_path_)) << "Native binary keyring must be rejected";
    EXPECT_TRUE(keyring.entries.empty()) << "No entries should be loaded from native binary keyring";
}

TEST_F(TDEKeyringTest, OfflineParserRejectsUnknownAlgorithm) {
    const auto path = write_json_keyring(
        "unknown_algorithm.json",
        R"({"key":{"master_key_id":1,"algorithm":"AES-256-GCM","key":"abababababababababababababababab"}})");

    innodb::OfflineKeyringParser parser;
    EXPECT_FALSE(parser.parse_keyring_file(path));
    EXPECT_TRUE(parser.get_master_keys().empty());
    EXPECT_NE(parser.get_last_error().find("unsupported algorithm"), std::string::npos);
}

TEST_F(TDEKeyringTest, OfflineParserRejectsMalformedBase64) {
    const auto invalid_char = write_json_keyring(
        "invalid_base64_char.json",
        R"({"key":{"master_key_id":1,"key_data":"q6urq6urq6urq6urq6urq!="}})");
    const auto invalid_padding = write_json_keyring(
        "invalid_base64_padding.json",
        R"({"key":{"master_key_id":1,"key_data":"q6urq6urq6urq6urq6urq=="}})");
    const auto invalid_length = write_json_keyring(
        "invalid_base64_length.json",
        R"({"key":{"master_key_id":1,"key_data":"q6urq6urq6urq6urq6urq"}})");

    for (const auto& path : {invalid_char, invalid_padding, invalid_length}) {
        innodb::OfflineKeyringParser parser;
        EXPECT_FALSE(parser.parse_keyring_file(path));
        EXPECT_TRUE(parser.get_master_keys().empty());
        EXPECT_NE(parser.get_last_error().find("invalid Base64"), std::string::npos);
    }
}

TEST_F(TDEKeyringTest, OfflineParserRejectsMalformedHex) {
    const auto odd_length = write_json_keyring(
        "odd_hex.json",
        R"({"key":{"master_key_id":1,"key":"abc"}})");
    const auto invalid_character = write_json_keyring(
        "invalid_hex.json",
        R"({"key":{"master_key_id":1,"key":"gg000000000000000000000000000000"}})");

    for (const auto& path : {odd_length, invalid_character}) {
        innodb::OfflineKeyringParser parser;
        EXPECT_FALSE(parser.parse_keyring_file(path));
        EXPECT_TRUE(parser.get_master_keys().empty());
        EXPECT_NE(parser.get_last_error().find("invalid hexadecimal"), std::string::npos);
    }
}

TEST_F(TDEKeyringTest, OfflineParserAcceptsValidBase64AndHex) {
    const auto base64 = write_json_keyring(
        "valid_base64.json",
        R"({"key":{"master_key_id":1,"key_data":"q6urq6urq6urq6urq6urqw=="}})");
    const auto hex = write_json_keyring(
        "valid_hex.json",
        R"({"key":{"master_key_id":2,"key":"abababababababababababababababab"}})");

    innodb::OfflineKeyringParser parser;
    EXPECT_TRUE(parser.parse_keyring_file(base64));
    ASSERT_EQ(parser.get_master_keys().size(), 1u);
    EXPECT_EQ(parser.get_master_keys()[0].key_id, 1u);
    EXPECT_EQ(parser.get_master_keys()[0].key_data.size(), 16u);

    EXPECT_TRUE(parser.parse_keyring_file(hex));
    ASSERT_EQ(parser.get_master_keys().size(), 1u);
    EXPECT_EQ(parser.get_master_keys()[0].key_id, 2u);
    EXPECT_EQ(parser.get_master_keys()[0].key_data.size(), 16u);
}

TEST_F(TDEKeyringTest, OfflineParserPreservesKeysAfterFailedReplacement) {
    const auto valid = write_json_keyring(
        "valid_before_failure.json",
        R"({"existing":{"master_key_id":7,"key":"abababababababababababababababab"}})");
    const auto invalid = write_json_keyring(
        "invalid_replacement.json",
        R"({"replacement":{"master_key_id":8,"algorithm":"AES-256-GCM","key":"abababababababababababababababab"}})");

    innodb::OfflineKeyringParser parser;
    ASSERT_TRUE(parser.parse_keyring_file(valid));
    ASSERT_EQ(parser.get_master_keys().size(), 1u);
    EXPECT_FALSE(parser.parse_keyring_file(invalid));
    ASSERT_EQ(parser.get_master_keys().size(), 1u);
    EXPECT_EQ(parser.get_master_keys()[0].key_id, 7u);
    EXPECT_EQ(parser.get_master_keys()[0].key_name, "existing");
}


TEST_F(TDEKeyringTest, FindKeyByNameFailsOnLoadedKeyring) {
    create_test_keyring();

    innodb::KeyringFile keyring;
    // load() always fails closed for native binary keyrings
    ASSERT_FALSE(keyring.load(keyring_path_));
    EXPECT_TRUE(keyring.entries.empty());

    // With no entries, find_key must return empty
    auto not_found = keyring.find_key("INNODBKey-test-uuid-1");
    EXPECT_FALSE(not_found.has_value()) << "No key should be found on empty entries";
}

TEST_F(TDEKeyringTest, RecoveryEngineRejectsNativeKeyringWithoutNumericKeyIdentity) {
    create_test_keyring();

    recovery::RecoveryEngine engine;
    EXPECT_FALSE(engine.load_tde_keyring(keyring_path_));
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

TEST_F(TDEKeyringTest, TDECandidateProbeDistinguishesRecognitionFromConfirmation) {
    std::vector<uint8_t> page(100, 0);
    page[24] = 0x45;
    page[25] = 0xBF;

    EXPECT_EQ(innodb::TDEDecryptor::probe_candidate(page.data(), page.size()),
              innodb::TDECandidateProbe::NOT_DETECTED);

    page[38] = 6;
    EXPECT_EQ(innodb::TDEDecryptor::probe_candidate(page.data(), page.size()),
              innodb::TDECandidateProbe::RECOGNIZED_CANDIDATE);
    EXPECT_TRUE(innodb::TDEDecryptor::is_encrypted(page.data(), page.size()));

    page[38] = 0;
    EXPECT_EQ(innodb::TDEDecryptor::probe_candidate(page.data(), page.size()),
              innodb::TDECandidateProbe::NOT_DETECTED);

    page[38] = 99;
    EXPECT_EQ(innodb::TDEDecryptor::probe_candidate(page.data(), page.size()),
              innodb::TDECandidateProbe::NOT_DETECTED);
}

TEST_F(TDEKeyringTest, IsEncryptedDetection) {
    std::vector<uint8_t> page(100, 0);
    page[24] = 0x45;
    page[25] = 0xBF;
    EXPECT_FALSE(innodb::TDEDecryptor::is_encrypted(page.data(), page.size()));

    page[38] = 6;
    EXPECT_TRUE(innodb::TDEDecryptor::is_encrypted(page.data(), page.size()));
    EXPECT_FALSE(innodb::TDEDecryptor::is_encrypted(nullptr, page.size()));
    EXPECT_FALSE(innodb::TDEDecryptor::is_encrypted(page.data(), 82));
    EXPECT_TRUE(innodb::TDEDecryptor::is_encrypted(page.data(), 83));

    page[24] = 0;
    page[25] = 1;
    EXPECT_FALSE(innodb::TDEDecryptor::is_encrypted(page.data(), page.size()));
}

TEST_F(TDEKeyringTest, ConfigureKeysRejectsUnusableMaterial) {
    innodb::EncryptedTableRecovery recovery;

    // Empty keys vector — hits the keys.empty() gate
    EXPECT_FALSE(recovery.configure_keys({}));
    EXPECT_EQ(recovery.get_last_error(), "No usable key material provided");
    EXPECT_EQ(recovery.get_decryptor().get_stats().keys_loaded, 0u);

    // Key with key_id=0 — hits the is_valid() gate before the loop body
    innodb::MasterKey zero_id;
    zero_id.key_name = "zero-id";
    EXPECT_FALSE(recovery.configure_keys({zero_id}));
    EXPECT_EQ(recovery.get_last_error(), "Keyring contains invalid key material or key ID");
    EXPECT_EQ(recovery.get_decryptor().get_stats().keys_loaded, 0u);
}

TEST_F(TDEKeyringTest, ConfigureKeysRejectsMixedInvalidKeyAtomically) {
    innodb::MasterKey invalid;
    invalid.key_name = "invalid";

    innodb::MasterKey valid;
    valid.key_id = 42;
    valid.key_name = "parsed-key";
    valid.key_data.assign(32, 0xAB);
    valid.algorithm = innodb::TDEAlgorithm::AES_256_CBC;

    innodb::EncryptedTableRecovery recovery;
    EXPECT_FALSE(recovery.configure_keys({invalid, valid}));
    EXPECT_FALSE(recovery.get_last_error().empty());
    EXPECT_EQ(recovery.get_decryptor().get_stats().keys_loaded, 0u);
}

TEST_F(TDEKeyringTest, ConfigureKeysRejectsZeroId) {
    innodb::MasterKey key;
    key.key_id = 0;
    key.key_name = "zero-id";
    key.key_data.assign(32, 0xAB);
    key.algorithm = innodb::TDEAlgorithm::AES_256_CBC;

    innodb::EncryptedTableRecovery recovery;
    EXPECT_FALSE(recovery.configure_keys({key}));
    EXPECT_FALSE(recovery.get_last_error().empty());
    EXPECT_EQ(recovery.get_decryptor().get_stats().keys_loaded, 0u);
}

TEST_F(TDEKeyringTest, ConfigureKeysRejectsDuplicateIds) {
    innodb::MasterKey key;
    key.key_id = 7;
    key.key_name = "first";
    key.key_data.assign(32, 0xAB);
    key.algorithm = innodb::TDEAlgorithm::AES_256_CBC;

    innodb::MasterKey dup = key;
    dup.key_name = "second";

    innodb::EncryptedTableRecovery recovery;
    EXPECT_FALSE(recovery.configure_keys({key, dup}));
    EXPECT_FALSE(recovery.get_last_error().empty());
    EXPECT_EQ(recovery.get_decryptor().get_stats().keys_loaded, 0u);
}

TEST_F(TDEKeyringTest, ConfigureKeysRejectsUnsupportedKeyLength) {
    innodb::MasterKey key;
    key.key_id = 9;
    key.key_name = "bad-length";
    key.key_data.assign(20, 0xAB);
    key.algorithm = innodb::TDEAlgorithm::AES_256_CBC;

    innodb::EncryptedTableRecovery recovery;
    EXPECT_FALSE(recovery.configure_keys({key}));
    EXPECT_FALSE(recovery.get_last_error().empty());
    EXPECT_EQ(recovery.get_decryptor().get_stats().keys_loaded, 0u);
}

TEST_F(TDEKeyringTest, ConfigureKeysAcceptsAllSupportedKeyLengths) {
    for (size_t len : {size_t(16), size_t(24), size_t(32)}) {
        innodb::MasterKey key;
        key.key_id = static_cast<uint32_t>(len);
        key.key_name = "len-" + std::to_string(len);
        key.key_data.assign(len, 0xCD);
        key.algorithm = innodb::TDEAlgorithm::AES_256_CBC;

        innodb::EncryptedTableRecovery recovery;
        EXPECT_TRUE(recovery.configure_keys({key})) << "length " << len << " should be accepted";
        EXPECT_TRUE(recovery.get_last_error().empty());
        EXPECT_EQ(recovery.get_decryptor().get_stats().keys_loaded, 1u);
    }
}

// ==================== TDE fail-closed behavior tests ====================

// Helper to write a binary IBD file for testing
void write_ibd_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
}

// Minimal FIL header with page type
void write_fil_header(uint8_t* p, uint16_t page_type) {
    // FIL_PAGE_TYPE at offset 24
    p[24] = static_cast<uint8_t>(page_type >> 8);
    p[25] = static_cast<uint8_t>(page_type);
    // FIL_PAGE_SPACE_ID at offset 34
    p[34] = 0; p[35] = 0; p[36] = 0; p[37] = 1;
    // FIL_PAGE_OFFSET at offset 4
    p[4] = 0; p[5] = 0; p[6] = 0; p[7] = 0;
    // FIL_PAGE_PREV at offset 8
    p[8] = 0xFF; p[9] = 0xFF; p[10] = 0xFF; p[11] = 0xFF;
    // FIL_PAGE_NEXT at offset 12
    p[12] = 0xFF; p[13] = 0xFF; p[14] = 0xFF; p[15] = 0xFF;
    // FIL_PAGE_LSN at offset 16
    p[16] = 0; p[17] = 0; p[18] = 0; p[19] = 0; p[20] = 0; p[21] = 0; p[22] = 0; p[23] = 0;
}

// Write TDE header at offset 38 (FIL_PAGE_DATA)
void write_tde_header(uint8_t* p, uint8_t algo, uint32_t key_id) {
    // Algorithm byte
    p[38] = algo;
    // Key ID (4 bytes, little-endian in TDE header)
    p[39] = static_cast<uint8_t>(key_id & 0xFF);
    p[40] = static_cast<uint8_t>((key_id >> 8) & 0xFF);
    p[41] = static_cast<uint8_t>((key_id >> 16) & 0xFF);
    p[42] = static_cast<uint8_t>((key_id >> 24) & 0xFF);
    // IV (12 bytes)
    for (int i = 0; i < 12; i++) p[43 + i] = static_cast<uint8_t>(i);
    // Encrypted header (28 bytes): non-zero to pass all-zero check
    for (int i = 0; i < 28; i++) p[55 + i] = static_cast<uint8_t>(0xAB);
}

TEST_F(TDEKeyringTest, DecryptorIsNotValidatedWithoutFixture) {
    EXPECT_FALSE(innodb::EncryptedTableRecovery::decryptor_is_validated());
}

TEST_F(TDEKeyringTest, RecoverWithoutValidatedDecryptorEmitsZeroRows) {
    const std::string path = test_dir_ + "/encrypted_recover.ibd";
    const size_t page_size = 16384;
    std::vector<uint8_t> data(page_size * 4, 0);
    // Page 0: FSP_HDR with encrypted TDE header
    write_fil_header(data.data(), 0x0000); // FSP_HDR
    write_tde_header(data.data(), 6, 1);
    // Page 1: INDEX with encrypted header
    write_fil_header(data.data() + page_size, 0x45BF);
    write_tde_header(data.data() + page_size, 6, 1);
    write_ibd_file(path, data);

    innodb::EncryptedTableRecovery enc_rec(page_size);
    enc_rec.set_file_path(path);

    // Without validated decryptor, recover() must fail closed
    auto records = enc_rec.recover();
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(enc_rec.get_stats().tde_status, innodb::TDEStatus::ENCRYPTED_UNSUPPORTED);
}

TEST_F(TDEKeyringTest, ClassifyEncryptedFileReturnsUnsupported) {
    const std::string path = test_dir_ + "/encrypted_classify.ibd";
    const size_t page_size = 16384;
    std::vector<uint8_t> data(page_size, 0);
    write_fil_header(data.data(), 0x45BF);
    write_tde_header(data.data(), 6, 1);
    write_ibd_file(path, data);

    innodb::EncryptedTableRecovery enc_rec(page_size);
    EXPECT_EQ(enc_rec.classify(path, page_size), innodb::TDEStatus::ENCRYPTED_UNSUPPORTED);
}

TEST_F(TDEKeyringTest, ClassifyNotEncryptedReturnsNotEncrypted) {
    const std::string path = test_dir_ + "/not_encrypted.ibd";
    const size_t page_size = 16384;
    std::vector<uint8_t> data(page_size, 0);
    write_fil_header(data.data(), 0x45BF);
    // No TDE header — page is not encrypted
    write_ibd_file(path, data);

    innodb::EncryptedTableRecovery enc_rec(page_size);
    EXPECT_EQ(enc_rec.classify(path, page_size), innodb::TDEStatus::NOT_ENCRYPTED);
}

TEST_F(TDEKeyringTest, ClassifyMalformedFileReturnsMalformed) {
    const std::string path = test_dir_ + "/malformed.ibd";
    const size_t page_size = 16384;
    std::vector<uint8_t> data(50, 0); // Too small for FIL header + TDE header
    write_ibd_file(path, data);

    innodb::EncryptedTableRecovery enc_rec(page_size);
    EXPECT_EQ(enc_rec.classify(path, page_size), innodb::TDEStatus::MALFORMED);
}

TEST_F(TDEKeyringTest, ClassifyUnknownAlgorithmReturnsNotEncrypted) {
    const std::string path = test_dir_ + "/unknown_algo.ibd";
    const size_t page_size = 16384;
    std::vector<uint8_t> data(page_size, 0);
    write_fil_header(data.data(), 0x45BF);
    // Algorithm byte 99 is outside the detectable range [1,9];
    // is_encrypted() returns false → classify() returns NOT_ENCRYPTED
    write_tde_header(data.data(), 99, 1);
    write_ibd_file(path, data);

    innodb::EncryptedTableRecovery enc_rec(page_size);
    EXPECT_EQ(enc_rec.classify(path, page_size), innodb::TDEStatus::NOT_ENCRYPTED);
}

TEST_F(TDEKeyringTest, TDEStatusToStringRoundTrip) {
    EXPECT_STREQ(innodb::EncryptedTableRecoveryStats::status_to_string(
                     innodb::TDEStatus::NOT_ENCRYPTED), "NOT_ENCRYPTED");
    EXPECT_STREQ(innodb::EncryptedTableRecoveryStats::status_to_string(
                     innodb::TDEStatus::ENCRYPTED_UNSUPPORTED), "ENCRYPTED_UNSUPPORTED");
    EXPECT_STREQ(innodb::EncryptedTableRecoveryStats::status_to_string(
                     innodb::TDEStatus::RECOVERED), "RECOVERED");
    EXPECT_STREQ(innodb::EncryptedTableRecoveryStats::status_to_string(
                     innodb::TDEStatus::MALFORMED), "MALFORMED");
    EXPECT_STREQ(innodb::EncryptedTableRecoveryStats::status_to_string(
                     innodb::TDEStatus::CORRUPTED_METADATA), "CORRUPTED_METADATA");
    EXPECT_STREQ(innodb::EncryptedTableRecoveryStats::status_to_string(
                     innodb::TDEStatus::KEY_MISSING), "KEY_MISSING");
}

TEST_F(TDEKeyringTest, ClassifyKeyMissingStatusRoundTrip) {
    // Verify KEY_MISSING is a valid enum value and round-trips through to_string
    EXPECT_EQ(innodb::TDEStatus::KEY_MISSING, static_cast<innodb::TDEStatus>(5));
    EXPECT_STREQ(innodb::EncryptedTableRecoveryStats::status_to_string(
                     innodb::TDEStatus::KEY_MISSING), "KEY_MISSING");
}

TEST_F(TDEKeyringTest, ClassifyFileNotFoundReturnsMalformed) {
    innodb::EncryptedTableRecovery enc_rec(16384);
    EXPECT_EQ(enc_rec.classify("/nonexistent/path.ibd", 16384), innodb::TDEStatus::MALFORMED);
}

TEST_F(TDEKeyringTest, ClassifyTruncatedPageReturnsMalformed) {
    const std::string path = test_dir_ + "/truncated.ibd";
    const size_t page_size = 16384;
    std::vector<uint8_t> data(80, 0); // 80 bytes < 83 minimum for TDE header
    write_fil_header(data.data(), 0x45BF);
    write_ibd_file(path, data);

    innodb::EncryptedTableRecovery enc_rec(page_size);
    EXPECT_EQ(enc_rec.classify(path, page_size), innodb::TDEStatus::MALFORMED);
}