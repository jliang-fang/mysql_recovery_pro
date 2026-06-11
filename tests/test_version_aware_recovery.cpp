/**
 * @file test_version_aware_recovery.cpp
 * @brief Unit tests for version-aware recovery engine
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <cstring>

#include "dbrecovery/recovery/version_aware_recovery.hpp"
#include "dbrecovery/common/version_compat.hpp"

using namespace dbrecovery;
using namespace dbrecovery::recovery;

// ==================== VersionConfig Tests ====================

class VersionConfigTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(VersionConfigTest, GetMySQL56Config) {
    auto config = get_version_config(MySQLVersion::MYSQL_56);

    EXPECT_EQ(config.version, MySQLVersion::MYSQL_56);
    EXPECT_EQ(config.name, "MySQL 5.6");
    EXPECT_EQ(config.default_page_size, 16384);
    EXPECT_FALSE(config.supports_sdi);
    EXPECT_FALSE(config.supports_instant_ddl);
    EXPECT_FALSE(config.supports_encryption);
    EXPECT_TRUE(config.supports_compact);
    EXPECT_FALSE(config.supports_dynamic);
}

TEST_F(VersionConfigTest, GetMySQL57Config) {
    auto config = get_version_config(MySQLVersion::MYSQL_57);

    EXPECT_EQ(config.version, MySQLVersion::MYSQL_57);
    EXPECT_EQ(config.name, "MySQL 5.7");
    EXPECT_TRUE(config.supports_instant_ddl);
    EXPECT_TRUE(config.supports_json_type);
    EXPECT_TRUE(config.supports_dynamic);
}

TEST_F(VersionConfigTest, GetMySQL80Config) {
    auto config = get_version_config(MySQLVersion::MYSQL_80);

    EXPECT_EQ(config.version, MySQLVersion::MYSQL_80);
    EXPECT_EQ(config.name, "MySQL 8.0");
    EXPECT_TRUE(config.supports_sdi);
    EXPECT_TRUE(config.supports_instant_ddl);
    EXPECT_TRUE(config.supports_encryption);
    EXPECT_TRUE(config.supports_invisible_columns);
}

TEST_F(VersionConfigTest, GetMySQL84Config) {
    auto config = get_version_config(MySQLVersion::MYSQL_84);

    EXPECT_EQ(config.version, MySQLVersion::MYSQL_84);
    EXPECT_EQ(config.name, "MySQL 8.4");
    EXPECT_TRUE(config.supports_sdi);
    EXPECT_TRUE(config.supports_temporary_pages);
}

TEST_F(VersionConfigTest, GetMySQL90Config) {
    auto config = get_version_config(MySQLVersion::MYSQL_90);

    EXPECT_EQ(config.version, MySQLVersion::MYSQL_90);
    EXPECT_EQ(config.name, "MySQL 9.0");
    EXPECT_TRUE(config.supports_sdi);
    EXPECT_TRUE(config.supports_encryption);
}

// ==================== VersionDetector Tests ====================

class VersionDetectorTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(VersionDetectorTest, VersionToString) {
    EXPECT_EQ(VersionDetector::version_to_string(MySQLVersion::MYSQL_56), "MySQL 5.6");
    EXPECT_EQ(VersionDetector::version_to_string(MySQLVersion::MYSQL_57), "MySQL 5.7");
    EXPECT_EQ(VersionDetector::version_to_string(MySQLVersion::MYSQL_80), "MySQL 8.0");
    EXPECT_EQ(VersionDetector::version_to_string(MySQLVersion::MYSQL_84), "MySQL 8.4");
    EXPECT_EQ(VersionDetector::version_to_string(MySQLVersion::MYSQL_90), "MySQL 9.0");
    EXPECT_EQ(VersionDetector::version_to_string(MySQLVersion::UNKNOWN), "Unknown");
}

TEST_F(VersionDetectorTest, SupportsSDI) {
    EXPECT_FALSE(VersionDetector::supports_sdi(MySQLVersion::MYSQL_56));
    EXPECT_FALSE(VersionDetector::supports_sdi(MySQLVersion::MYSQL_57));
    EXPECT_TRUE(VersionDetector::supports_sdi(MySQLVersion::MYSQL_80));
    EXPECT_TRUE(VersionDetector::supports_sdi(MySQLVersion::MYSQL_84));
    EXPECT_TRUE(VersionDetector::supports_sdi(MySQLVersion::MYSQL_90));
}

TEST_F(VersionDetectorTest, SupportsEncryption) {
    EXPECT_FALSE(VersionDetector::supports_encryption(MySQLVersion::MYSQL_56));
    EXPECT_FALSE(VersionDetector::supports_encryption(MySQLVersion::MYSQL_57));
    EXPECT_TRUE(VersionDetector::supports_encryption(MySQLVersion::MYSQL_80));
    EXPECT_TRUE(VersionDetector::supports_encryption(MySQLVersion::MYSQL_84));
    EXPECT_TRUE(VersionDetector::supports_encryption(MySQLVersion::MYSQL_90));
}

TEST_F(VersionDetectorTest, SupportsInstantDDL) {
    EXPECT_FALSE(VersionDetector::supports_instant_ddl(MySQLVersion::MYSQL_56));
    EXPECT_TRUE(VersionDetector::supports_instant_ddl(MySQLVersion::MYSQL_57));
    EXPECT_TRUE(VersionDetector::supports_instant_ddl(MySQLVersion::MYSQL_80));
    EXPECT_TRUE(VersionDetector::supports_instant_ddl(MySQLVersion::MYSQL_84));
    EXPECT_TRUE(VersionDetector::supports_instant_ddl(MySQLVersion::MYSQL_90));
}

TEST_F(VersionDetectorTest, GetDefaultRowFormat) {
    EXPECT_EQ(VersionDetector::get_default_row_format(MySQLVersion::MYSQL_56), RowFormat::COMPACT);
    EXPECT_EQ(VersionDetector::get_default_row_format(MySQLVersion::MYSQL_57), RowFormat::DYNAMIC);
    EXPECT_EQ(VersionDetector::get_default_row_format(MySQLVersion::MYSQL_80), RowFormat::DYNAMIC);
    EXPECT_EQ(VersionDetector::get_default_row_format(MySQLVersion::MYSQL_84), RowFormat::DYNAMIC);
    EXPECT_EQ(VersionDetector::get_default_row_format(MySQLVersion::MYSQL_90), RowFormat::DYNAMIC);
}

// ==================== VersionAwareRecoveryEngine Tests ====================

class VersionAwareRecoveryEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = std::make_unique<VersionAwareRecoveryEngine>();
    }

    std::unique_ptr<VersionAwareRecoveryEngine> engine_;
};

TEST_F(VersionAwareRecoveryEngineTest, SetForceVersion) {
    engine_->set_force_version(MySQLVersion::MYSQL_80);
    // Version should be forced, not detected
}

TEST_F(VersionAwareRecoveryEngineTest, SetPageSize) {
    engine_->set_page_size(32768);
    // Page size should be set
}

TEST_F(VersionAwareRecoveryEngineTest, SetRecoveryMode) {
    engine_->set_recovery_mode(VersionAwareRecoveryEngine::RecoveryMode::TRUNCATE);
    engine_->set_recovery_mode(VersionAwareRecoveryEngine::RecoveryMode::DROP);
    engine_->set_recovery_mode(VersionAwareRecoveryEngine::RecoveryMode::RMRF);
}

// ==================== MySQL Version-Specific Parser Tests ====================

class MySQL56ParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a simple test page
        test_page_.resize(16384, 0);
    }

    std::vector<uint8_t> test_page_;
};

TEST_F(MySQL56ParserTest, ParseEmptyPage) {
    MySQL56Parser parser(test_page_.data(), test_page_.size());
    auto records = parser.parse_compact();
    EXPECT_TRUE(records.empty());
}

TEST_F(MySQL56ParserTest, ParseRedundantFormat) {
    MySQL56Parser parser(test_page_.data(), test_page_.size());
    auto records = parser.parse_redundant();
    EXPECT_TRUE(records.empty());
}

class MySQL57ParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_page_.resize(16384, 0);
    }

    std::vector<uint8_t> test_page_;
};

TEST_F(MySQL57ParserTest, ParseDynamicFormat) {
    MySQL57Parser parser(test_page_.data(), test_page_.size());
    auto records = parser.parse_dynamic();
    EXPECT_TRUE(records.empty());
}

TEST_F(MySQL57ParserTest, InstantColumnDetection) {
    MySQL57Parser parser(test_page_.data(), test_page_.size());
    // By default, no instant columns
    EXPECT_FALSE(parser.has_instant_columns());
}

class MySQL80ParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_page_.resize(16384, 0);
    }

    std::vector<uint8_t> test_page_;
};

TEST_F(MySQL80ParserTest, ParseDynamicFormat) {
    MySQL80Parser parser(test_page_.data(), test_page_.size());
    auto records = parser.parse_dynamic();
    EXPECT_TRUE(records.empty());
}

TEST_F(MySQL80ParserTest, EncryptionDetection) {
    MySQL80Parser parser(test_page_.data(), test_page_.size());
    // By default, not encrypted
    EXPECT_FALSE(parser.is_encrypted());
}

TEST_F(MySQL80ParserTest, ParseSDI) {
    MySQL80Parser parser(test_page_.data(), test_page_.size());
    auto schema = parser.parse_sdi();
    // Empty page should not have valid SDI
    EXPECT_FALSE(schema.has_value());
}

class MySQL84ParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_page_.resize(16384, 0);
    }

    std::vector<uint8_t> test_page_;
};

TEST_F(MySQL84ParserTest, TemporaryPageDetection) {
    MySQL84Parser parser(test_page_.data(), test_page_.size());
    // By default, not temporary
    EXPECT_FALSE(parser.is_temporary());
}

class MySQL90ParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_page_.resize(16384, 0);
    }

    std::vector<uint8_t> test_page_;
};

TEST_F(MySQL90ParserTest, ParseWithMetadata) {
    MySQL90Parser parser(test_page_.data(), test_page_.size());
    auto records = parser.parse_with_metadata();
    EXPECT_TRUE(records.empty());
}

// ==================== Version Detection Tests ====================

class VersionDetectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_page_.resize(16384, 0);
    }

    std::vector<uint8_t> test_page_;
};

TEST_F(VersionDetectionTest, DetectMySQL56FromLSN) {
    // Set LSN to a value typical for MySQL 5.6 (< 1B)
    uint64_t lsn = 500000000ULL;  // 500M
    std::memcpy(test_page_.data() + 16, &lsn, sizeof(lsn));

    VersionAwareRecoveryEngine engine;
    auto version = engine.detect_version(test_page_.data(), test_page_.size());

    // Should detect as MySQL 5.6 or similar
    EXPECT_NE(version, MySQLVersion::UNKNOWN);
}

TEST_F(VersionDetectionTest, DetectMySQL80FromLSN) {
    // Set LSN to a value typical for MySQL 8.0 (> 5B)
    uint64_t lsn = 10000000000ULL;  // 10B
    std::memcpy(test_page_.data() + 16, &lsn, sizeof(lsn));

    VersionAwareRecoveryEngine engine;
    auto version = engine.detect_version(test_page_.data(), test_page_.size());

    // Should detect as MySQL 8.0+
    EXPECT_TRUE(version == MySQLVersion::MYSQL_80 ||
                version == MySQLVersion::MYSQL_84 ||
                version == MySQLVersion::MYSQL_90);
}

// ==================== Integration Tests ====================

class MultiVersionIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = std::make_unique<VersionAwareRecoveryEngine>();
    }

    std::unique_ptr<VersionAwareRecoveryEngine> engine_;
};

TEST_F(MultiVersionIntegrationTest, AllVersionsSupported) {
    std::vector<MySQLVersion> versions = {
        MySQLVersion::MYSQL_56,
        MySQLVersion::MYSQL_57,
        MySQLVersion::MYSQL_80,
        MySQLVersion::MYSQL_84,
        MySQLVersion::MYSQL_90
    };

    for (auto version : versions) {
        auto config = get_version_config(version);
        EXPECT_NE(config.name, "Unknown");
        EXPECT_GT(config.default_page_size, 0);
    }
}

TEST_F(MultiVersionIntegrationTest, VersionProgression) {
    // Each version should have more features than the previous
    auto config56 = get_version_config(MySQLVersion::MYSQL_56);
    auto config57 = get_version_config(MySQLVersion::MYSQL_57);
    auto config80 = get_version_config(MySQLVersion::MYSQL_80);
    auto config84 = get_version_config(MySQLVersion::MYSQL_84);
    auto config90 = get_version_config(MySQLVersion::MYSQL_90);

    // MySQL 5.7 should have features that 5.6 doesn't
    EXPECT_TRUE(config57.supports_instant_ddl && !config56.supports_instant_ddl);
    EXPECT_TRUE(config57.supports_json_type && !config56.supports_json_type);

    // MySQL 8.0 should have features that 5.7 doesn't
    EXPECT_TRUE(config80.supports_sdi && !config57.supports_sdi);
    EXPECT_TRUE(config80.supports_encryption && !config57.supports_encryption);

    // MySQL 8.4 should have features that 8.0 doesn't
    EXPECT_TRUE(config84.supports_temporary_pages && !config80.supports_temporary_pages);
}

// ==================== Main ====================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
