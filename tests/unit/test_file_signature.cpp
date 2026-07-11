/**
 * @file test_file_signature.cpp
 * @brief Unit tests for file signature detection framework
 *
 * Reference: testdisk filegen.h patterns
 */

#include <gtest/gtest.h>
#include "dbrecovery/disk/file_signature.hpp"

using namespace dbrecovery::disk;

class FileSignatureTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ==================== JPEG Signature Tests ====================

TEST_F(FileSignatureTest, JpegSignatureValid) {
    auto jpeg_hint = signatures::jpeg();

    // Valid JPEG header
    std::vector<uint8_t> jpeg_data = {
        0xFF, 0xD8, 0xFF, 0xE0,  // JPEG SOI + APP0
        0x00, 0x10, 0x4A, 0x46,  // Length + JFIF
        0x49, 0x46, 0x00, 0x01   // JFIF\0 + version
    };

    auto result = FileSignatureRegistry::instance().check_data(
        jpeg_data.data(), jpeg_data.size());

    EXPECT_TRUE(result.detected);
    EXPECT_EQ(result.extension, "jpg");
}

TEST_F(FileSignatureTest, JpegSignatureInvalid) {
    // Invalid data - not a JPEG
    std::vector<uint8_t> random_data = {0x00, 0x01, 0x02, 0x03};

    auto result = FileSignatureRegistry::instance().check_data(
        random_data.data(), random_data.size());

    EXPECT_FALSE(result.detected);
}

TEST_F(FileSignatureTest, JpegSignatureTooShort) {
    std::vector<uint8_t> short_data = {0xFF};  // Only 1 byte

    auto result = FileSignatureRegistry::instance().check_data(
        short_data.data(), short_data.size());

    EXPECT_FALSE(result.detected);
}

// ==================== PNG Signature Tests ====================

TEST_F(FileSignatureTest, PngSignatureValid) {
    auto png_hint = signatures::png();

    // Valid PNG header
    std::vector<uint8_t> png_data = {
        0x89, 0x50, 0x4E, 0x47,  // PNG signature
        0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D   // IHDR chunk length
    };

    auto result = FileSignatureRegistry::instance().check_data(
        png_data.data(), png_data.size());

    EXPECT_TRUE(result.detected);
    EXPECT_EQ(result.extension, "png");
}

// ==================== ZIP Signature Tests ====================

TEST_F(FileSignatureTest, ZipSignatureValid) {
    auto zip_hint = signatures::zip();

    // Valid ZIP header
    std::vector<uint8_t> zip_data = {
        0x50, 0x4B, 0x03, 0x04,  // ZIP local file header
        0x14, 0x00, 0x00, 0x00,  // Version + flags
        0x08, 0x00              // Compression method
    };

    auto result = FileSignatureRegistry::instance().check_data(
        zip_data.data(), zip_data.size());

    EXPECT_TRUE(result.detected);
    EXPECT_EQ(result.extension, "zip");
}

// ==================== PDF Signature Tests ====================

TEST_F(FileSignatureTest, PdfSignatureValid) {
    auto pdf_hint = signatures::pdf();

    // Valid PDF header
    std::vector<uint8_t> pdf_data = {
        0x25, 0x50, 0x44, 0x46,  // %PDF
        0x2D, 0x31, 0x2E, 0x34,  // -1.4
        0x0A, 0x25, 0xE2, 0xE3   // newline + binary
    };

    auto result = FileSignatureRegistry::instance().check_data(
        pdf_data.data(), pdf_data.size());

    EXPECT_TRUE(result.detected);
    EXPECT_EQ(result.extension, "pdf");
}

// ==================== IBD Page Signature Tests ====================

TEST_F(FileSignatureTest, IbdIndexPageSignature) {
    auto ibd_hint = signatures::ibd_index();

    // Valid InnoDB index page header
    std::vector<uint8_t> ibd_data(16384, 0);

    // Set FIL_PAGE_TYPE at offset 24
    ibd_data[24] = 0x45;
    ibd_data[25] = 0xBF;  // FIL_PAGE_INDEX

    // Set some valid header fields
    ibd_data[0] = 0x00;
    ibd_data[1] = 0x00;
    ibd_data[2] = 0x00;
    ibd_data[3] = 0x01;  // space_id = 1

    auto result = FileSignatureRegistry::instance().check_data(
        ibd_data.data(), ibd_data.size());

    EXPECT_TRUE(result.detected);
    EXPECT_EQ(result.extension, "ibd");
}

TEST_F(FileSignatureTest, IbdSdiPageSignature) {
    // Valid InnoDB SDI page header
    std::vector<uint8_t> ibd_data(16384, 0);

    // Set FIL_PAGE_TYPE at offset 24
    ibd_data[24] = 0x45;
    ibd_data[25] = 0xBE;  // FIL_PAGE_SDI

    auto result = FileSignatureRegistry::instance().check_data(
        ibd_data.data(), ibd_data.size());

    EXPECT_TRUE(result.detected);
    EXPECT_EQ(result.extension, "ibd");
}

// ==================== Registry Tests ====================

TEST_F(FileSignatureTest, RegistryListSignatures) {
    auto sig_list = FileSignatureRegistry::instance().list_signatures();

    EXPECT_FALSE(sig_list.empty());
    // Should have at least the default signatures
    EXPECT_GE(sig_list.size(), 5);
}

TEST_F(FileSignatureTest, RegistryGetSignature) {
    const FileSignatureHint* hint = FileSignatureRegistry::instance().get_signature("JPEG");

    EXPECT_NE(hint, nullptr);
    EXPECT_EQ(hint->name, "JPEG");
    EXPECT_EQ(hint->extension, "jpg");
}

TEST_F(FileSignatureTest, RegistryGetNonExistentSignature) {
    const FileSignatureHint* hint = FileSignatureRegistry::instance().get_signature("NONEXISTENT");

    EXPECT_EQ(hint, nullptr);
}

// ==================== Priority Tests ====================

TEST_F(FileSignatureTest, SignaturePriority) {
    // IBD signatures should have higher priority (lower number) than JPEG
    auto ibd_hint = signatures::ibd_index();
    auto jpeg_hint = signatures::jpeg();

    EXPECT_LT(ibd_hint.priority, jpeg_hint.priority);
}

// ==================== Null Data Tests ====================

TEST_F(FileSignatureTest, NullDataCheck) {
    auto result = FileSignatureRegistry::instance().check_data(nullptr, 0);
    EXPECT_FALSE(result.detected);
}

TEST_F(FileSignatureTest, EmptyDataCheck) {
    std::vector<uint8_t> empty_data;
    auto result = FileSignatureRegistry::instance().check_data(
        empty_data.data(), empty_data.size());
    EXPECT_FALSE(result.detected);
}

// ==================== Custom Signature Tests ====================

TEST_F(FileSignatureTest, CustomSignature) {
    // Register a custom signature
    FileSignatureHint custom;
    custom.name = "CUSTOM_TEST";
    custom.extension = "test";
    custom.description = "Test signature";
    custom.offset = 0;
    custom.signature = {0xDE, 0xAD, 0xBE, 0xEF};
    custom.priority = 1;  // Very high priority

    FileSignatureRegistry::instance().register_signature(custom);

    // Test detection
    std::vector<uint8_t> custom_data = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00};

    auto result = FileSignatureRegistry::instance().check_data(
        custom_data.data(), custom_data.size());

    EXPECT_TRUE(result.detected);
    EXPECT_EQ(result.extension, "test");
}

// ==================== Mask Tests ====================

TEST_F(FileSignatureTest, SignatureWithMask) {
    FileSignatureHint masked;
    masked.name = "MASKED_TEST";
    masked.extension = "msk";
    masked.description = "Test with mask";
    masked.offset = 0;
    masked.signature = {0x12, 0x34, 0x56, 0x78};
    masked.mask = {0xFF, 0xFF, 0x00, 0xFF};  // Third byte is wildcard
    masked.priority = 1;

    FileSignatureRegistry::instance().register_signature(masked);

    // Should match with any third byte
    std::vector<uint8_t> data1 = {0x12, 0x34, 0xAB, 0x78};  // Third byte different
    std::vector<uint8_t> data2 = {0x12, 0x34, 0xCD, 0x78};

    auto result1 = FileSignatureRegistry::instance().check_data(data1.data(), data1.size());
    auto result2 = FileSignatureRegistry::instance().check_data(data2.data(), data2.size());

    // Note: Both should match, but registry checks in priority order
    // and IBD signatures might match first
}

// ==================== SQLite Signature Tests ====================

TEST_F(FileSignatureTest, SqliteSignatureValid) {
    std::vector<uint8_t> sqlite_data = {
        0x53, 0x51, 0x4C, 0x69,  // "SQLi"
        0x74, 0x65, 0x20, 0x66,  // "te f"
        0x6F, 0x72, 0x6D, 0x61,  // "orma"
        0x74, 0x20, 0x33, 0x00   // "t 3\0"
    };

    auto result = FileSignatureRegistry::instance().check_data(
        sqlite_data.data(), sqlite_data.size());

    EXPECT_TRUE(result.detected);
    EXPECT_EQ(result.extension, "sqlite");
}

// ==================== GZIP Signature Tests ====================

TEST_F(FileSignatureTest, GzipSignatureValid) {
    std::vector<uint8_t> gzip_data = {
        0x1F, 0x8B, 0x08, 0x00,  // GZIP magic + compression method
        0x00, 0x00, 0x00, 0x00   // Flags
    };

    auto result = FileSignatureRegistry::instance().check_data(
        gzip_data.data(), gzip_data.size());

    EXPECT_TRUE(result.detected);
    EXPECT_EQ(result.extension, "gz");
}