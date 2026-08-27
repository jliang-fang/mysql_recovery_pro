#include <gtest/gtest.h>
#include "dbrecovery/innodb/roll_ptr_decoder.hpp"
#include <cstring>
#include <vector>

namespace dbrecovery {
namespace innodb {

// ==================== Internal Format Tests ====================

TEST(RollPtrDecoderTest, DecodeInternalFormat) {
    // Internal: bits [31:0]=page_no, [49:32]=offset (18 bits), [55:50]=flags (6 bits)
    uint64_t rp = (0x2BULL << 50) | (0x1234ULL << 32) | 0x000FEDCBULL;
    auto decoded = RollPtrDecoder::decode(rp);

    EXPECT_EQ(decoded.page_no, 0x000FEDCBu);
    EXPECT_EQ(decoded.offset, 0x1234u);
    EXPECT_EQ(decoded.flags, 0x2Bu);
    EXPECT_TRUE(decoded.is_valid());
    EXPECT_FALSE(decoded.is_null());
}

TEST(RollPtrDecoderTest, DecodeInternalFormatZero) {
    auto decoded = RollPtrDecoder::decode(0);
    EXPECT_EQ(decoded.page_no, 0u);
    EXPECT_EQ(decoded.offset, 0u);
    EXPECT_EQ(decoded.flags, 0u);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

TEST(RollPtrDecoderTest, DecodeInternalFormatFIL_NULL) {
    uint64_t rp = (0x00ULL << 50) | (0x0000ULL << 32) | FIL_NULL;
    auto decoded = RollPtrDecoder::decode(rp);
    EXPECT_EQ(decoded.page_no, FIL_NULL);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

TEST(RollPtrDecoderTest, EncodeInternalFormat) {
    uint64_t rp = RollPtrDecoder::encode(0x000FEDCB, 0x1234, 0x2B);
    EXPECT_EQ(rp, (0x2BULL << 50) | (0x1234ULL << 32) | 0x000FEDCBULL);
}

TEST(RollPtrDecoderTest, EncodeDecodeRoundTrip) {
    uint64_t rp = RollPtrDecoder::encode(42, 128, 0);
    auto decoded = RollPtrDecoder::decode(rp);
    EXPECT_EQ(decoded.page_no, 42u);
    EXPECT_EQ(decoded.offset, 128u);
    EXPECT_EQ(decoded.flags, 0u);
}

TEST(RollPtrDecoderTest, EncodeWithFlagsRoundTrip) {
    uint64_t rp = RollPtrDecoder::encode(100, 200, 5);
    auto decoded = RollPtrDecoder::decode(rp);
    EXPECT_EQ(decoded.page_no, 100u);
    EXPECT_EQ(decoded.offset, 200u);
    EXPECT_EQ(decoded.flags, 5u);
}

// ==================== Storage Format (BE) Tests ====================

TEST(RollPtrDecoderTest, DecodeStorage7BytesMySQL80) {
    // 7 bytes BE: page_no >> 20, offset & 0x3FFFF, flags at top
    // page_no=0x1000, offset=0x200, flags=0x1
    // val = (1 << 52) | (0x1000 << 20) | 0x200
    uint64_t val = (1ULL << 52) | (0x1000ULL << 20) | 0x200ULL;
    uint8_t data[7];
    for (int i = 6; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    auto decoded = RollPtrDecoder::decode_storage(data, 7);
    EXPECT_EQ(decoded.page_no, 0x1000u);
    EXPECT_EQ(decoded.offset, 0x200u);
    EXPECT_EQ(decoded.flags, 0x1u);
    EXPECT_TRUE(decoded.is_valid());
}

TEST(RollPtrDecoderTest, DecodeStorage6BytesMySQL56) {
    // 6 bytes BE: no flags byte
    // page_no=0x1000, offset=0x200
    uint64_t val = (0x1000ULL << 20) | 0x200ULL;
    uint8_t data[6];
    for (int i = 5; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    auto decoded = RollPtrDecoder::decode_storage(data, 6);
    EXPECT_EQ(decoded.page_no, 0x1000u);
    EXPECT_EQ(decoded.offset, 0x200u);
    EXPECT_EQ(decoded.flags, 0u);  // No flags in 6-byte format
    EXPECT_TRUE(decoded.is_valid());
}

TEST(RollPtrDecoderTest, DecodeStorageNull) {
    // 7 bytes BE with page_no=0
    uint8_t data[7] = {0};
    auto decoded = RollPtrDecoder::decode_storage(data, 7);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

TEST(RollPtrDecoderTest, DecodeStorageNullPtr) {
    auto decoded = RollPtrDecoder::decode_storage(nullptr, 7);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

TEST(RollPtrDecoderTest, DecodeStorageTooSmall) {
    uint8_t data[5] = {0};
    auto decoded = RollPtrDecoder::decode_storage(data, 5);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

TEST(RollPtrDecoderTest, DecodeStorageToInternal) {
    // page_no=0x1000, offset=0x200, flags=0x1
    uint64_t val = (1ULL << 52) | (0x1000ULL << 20) | 0x200ULL;
    uint8_t data[7];
    for (int i = 6; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    uint64_t internal = RollPtrDecoder::decode_storage_to_internal(data, 7);
    auto decoded = RollPtrDecoder::decode(internal);
    EXPECT_EQ(decoded.page_no, 0x1000u);
    EXPECT_EQ(decoded.offset, 0x200u);
    EXPECT_EQ(decoded.flags, 0x1u);
}

TEST(RollPtrDecoderTest, DecodeStorageToInternalNull) {
    uint8_t data[7] = {0};
    uint64_t internal = RollPtrDecoder::decode_storage_to_internal(data, 7);
    EXPECT_EQ(internal, 0u);
}

TEST(RollPtrDecoderTest, EncodeStorageRoundTrip) {
    RollPtrDecoder::DecodedRollPtr rp{0x1000, 0x200, 0x1};
    uint8_t data[7] = {0};
    RollPtrDecoder::encode_storage(data, rp);

    auto decoded = RollPtrDecoder::decode_storage(data, 7);
    EXPECT_EQ(decoded.page_no, 0x1000u);
    EXPECT_EQ(decoded.offset, 0x200u);
    EXPECT_EQ(decoded.flags, 0x1u);
}

TEST(RollPtrDecoderTest, EncodeStorageNullOutput) {
    RollPtrDecoder::DecodedRollPtr rp{0x1000, 0x200, 0x1};
    RollPtrDecoder::encode_storage(nullptr, rp);  // Should not crash
}

// ==================== Field Data Format (LE) Tests ====================

TEST(RollPtrDecoderTest, DecodeFieldData) {
    // 6 bytes LE: [4B page_no, 2B offset]
    uint8_t data[6];
    write_le<uint32_t>(data, 0, 0x000FEDCB);
    write_le<uint16_t>(data, 4, 0x1234);

    auto decoded = RollPtrDecoder::decode_field_data(data, 6);
    EXPECT_EQ(decoded.page_no, 0x000FEDCBu);
    EXPECT_EQ(decoded.offset, 0x1234u);
    EXPECT_EQ(decoded.flags, 0u);
    EXPECT_TRUE(decoded.is_valid());
}

TEST(RollPtrDecoderTest, DecodeFieldDataNull) {
    uint8_t data[6] = {0};
    auto decoded = RollPtrDecoder::decode_field_data(data, 6);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

TEST(RollPtrDecoderTest, DecodeFieldDataNullPtr) {
    auto decoded = RollPtrDecoder::decode_field_data(nullptr, 6);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

TEST(RollPtrDecoderTest, DecodeFieldDataTooSmall) {
    uint8_t data[5] = {0};
    auto decoded = RollPtrDecoder::decode_field_data(data, 5);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

TEST(RollPtrDecoderTest, DecodeFieldDataToInternal) {
    uint8_t data[6];
    write_le<uint32_t>(data, 0, 0x000FEDCB);
    write_le<uint16_t>(data, 4, 0x1234);

    uint64_t internal = RollPtrDecoder::decode_field_data_to_internal(data, 6);
    auto decoded = RollPtrDecoder::decode(internal);
    EXPECT_EQ(decoded.page_no, 0x000FEDCBu);
    EXPECT_EQ(decoded.offset, 0x1234u);
}

TEST(RollPtrDecoderTest, DecodeFieldDataToInternalNull) {
    uint8_t data[6] = {0};
    uint64_t internal = RollPtrDecoder::decode_field_data_to_internal(data, 6);
    EXPECT_EQ(internal, 0u);
}

// ==================== Legacy Wrapper Tests ====================

TEST(RollPtrDecoderTest, ExtractPrevPtrFromField) {
    std::vector<uint8_t> field_data(6);
    write_le<uint32_t>(field_data.data(), 0, 0x000FEDCB);
    write_le<uint16_t>(field_data.data(), 4, 0x1234);

    uint64_t prev_ptr = RollPtrDecoder::extract_prev_ptr_from_field(field_data);
    auto decoded = RollPtrDecoder::decode(prev_ptr);
    EXPECT_EQ(decoded.page_no, 0x000FEDCBu);
    EXPECT_EQ(decoded.offset, 0x1234u);
}

TEST(RollPtrDecoderTest, ExtractPrevPtrFromFieldEmpty) {
    std::vector<uint8_t> field_data;
    EXPECT_EQ(RollPtrDecoder::extract_prev_ptr_from_field(field_data), 0u);
}

// ==================== Utility Tests ====================

TEST(RollPtrDecoderTest, ToStringDecoded) {
    RollPtrDecoder::DecodedRollPtr rp{42, 128, 0};
    std::string s = RollPtrDecoder::to_string(rp);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("page=42"), std::string::npos);
    EXPECT_NE(s.find("offset=128"), std::string::npos);
}

TEST(RollPtrDecoderTest, ToStringInternal) {
    uint64_t rp = RollPtrDecoder::encode(42, 128, 0);
    std::string s = RollPtrDecoder::to_string(rp);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("page=42"), std::string::npos);
    EXPECT_NE(s.find("offset=128"), std::string::npos);
}

// ==================== Edge Cases ====================

TEST(RollPtrDecoderTest, MaxValues) {
    // Use page_no that doesn't overflow into offset bits via OR
    uint64_t rp = RollPtrDecoder::encode(0x00FFFFFF, 0x3FFFF, 0x3F);
    auto decoded = RollPtrDecoder::decode(rp);
    EXPECT_EQ(decoded.page_no, 0x00FFFFFFu);
    EXPECT_EQ(decoded.offset, 0x3FFFFu);
    EXPECT_EQ(decoded.flags, 0x3Fu);
}

TEST(RollPtrDecoderTest, StorageMaxValues) {
    // 7 bytes: max page_no, max offset, max flags
    uint64_t val = (0xFULL << 52) | (0xFFFFFFFFFULL << 20) | 0x3FFFFULL;
    uint8_t data[7];
    for (int i = 6; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    auto decoded = RollPtrDecoder::decode_storage(data, 7);
    EXPECT_EQ(decoded.page_no, 0xFFFFFFFFu);
    EXPECT_EQ(decoded.offset, 0x3FFFFu);
    EXPECT_EQ(decoded.flags, 0xFu);
}

TEST(RollPtrDecoderTest, EncodeMaxValues) {
    uint64_t rp = RollPtrDecoder::encode(0xFFFFFFFF, 0x3FFFF, 0x3F);
    auto decoded = RollPtrDecoder::decode(rp);
    EXPECT_EQ(decoded.page_no, 0xFFFFFFFFu);
    EXPECT_EQ(decoded.offset, 0x3FFFFu);
    EXPECT_EQ(decoded.flags, 0x3Fu);
}

TEST(RollPtrDecoderTest, EncodeRejectsOutOfRangeInternalValues) {
    EXPECT_EQ(RollPtrDecoder::encode(1, 0x40000, 0), 0u);
    EXPECT_EQ(RollPtrDecoder::encode(1, 0, 0x40), 0u);
}

TEST(RollPtrDecoderTest, EncodeStorageRejectsOutOfRangeValues) {
    uint8_t data[7] = {0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5};
    EXPECT_FALSE(RollPtrDecoder::encode_storage(data, {1, 0x40000, 0}));
    EXPECT_FALSE(RollPtrDecoder::encode_storage(data, {1, 1, 0x10}));
    for (const auto byte : data) EXPECT_EQ(byte, 0xA5);
}

TEST(RollPtrDecoderTest, EncodeStorage6ByteRejectsOutOfRangeValues) {
    uint8_t data[6] = {0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5};
    EXPECT_FALSE(RollPtrDecoder::encode_storage_6byte(data, {1, 1, 1}));
    EXPECT_FALSE(RollPtrDecoder::encode_storage_6byte(data, {1, 0x40000, 0}));
    for (const auto byte : data) EXPECT_EQ(byte, 0xA5);
}

// ==================== Profile-based decode_storage Tests ====================

TEST(RollPtrDecoderTest, DecodeStorageWithProfile7Bytes) {
    // 7 bytes BE with MYSQL8_7BYTE profile
    uint64_t val = (1ULL << 52) | (0x1000ULL << 20) | 0x200ULL;
    uint8_t data[7];
    for (int i = 6; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    auto decoded = RollPtrDecoder::decode_storage(data, 7, UndoFormatProfile::MYSQL8_7BYTE);
    EXPECT_EQ(decoded.page_no, 0x1000u);
    EXPECT_EQ(decoded.offset, 0x200u);
    EXPECT_EQ(decoded.flags, 0x1u);
    EXPECT_TRUE(decoded.is_valid());
}

TEST(RollPtrDecoderTest, DecodeStorageWithProfile6Bytes) {
    // 6 bytes BE with LEGACY_6BYTE profile
    uint64_t val = (0x1000ULL << 20) | 0x200ULL;
    uint8_t data[6];
    for (int i = 5; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    auto decoded = RollPtrDecoder::decode_storage(data, 6, UndoFormatProfile::LEGACY_6BYTE);
    EXPECT_EQ(decoded.page_no, 0x1000u);
    EXPECT_EQ(decoded.offset, 0x200u);
    EXPECT_EQ(decoded.flags, 0u);  // No flags in 6-byte format
    EXPECT_TRUE(decoded.is_valid());
}

TEST(RollPtrDecoderTest, DecodeStorageWithProfileUnresolved) {
    uint64_t val = (1ULL << 52) | (0x1000ULL << 20) | 0x200ULL;
    uint8_t data[7];
    for (int i = 6; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    // UNRESOLVED must return invalid regardless of data
    auto decoded = RollPtrDecoder::decode_storage(data, 7, UndoFormatProfile::UNRESOLVED);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

TEST(RollPtrDecoderTest, DecodeStorageWithProfileWrongWidth) {
    // 6 bytes data with MYSQL8_7BYTE profile — expects 7 bytes, should fail
    uint64_t val = (0x1000ULL << 20) | 0x200ULL;
    uint8_t data[6];
    for (int i = 5; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    auto decoded = RollPtrDecoder::decode_storage(data, 6, UndoFormatProfile::MYSQL8_7BYTE);
    EXPECT_FALSE(decoded.is_valid());
    EXPECT_TRUE(decoded.is_null());
}

TEST(RollPtrDecoderTest, DecodeStorageToInternalWithProfileUnresolved) {
    uint64_t val = (1ULL << 52) | (0x1000ULL << 20) | 0x200ULL;
    uint8_t data[7];
    for (int i = 6; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    uint64_t internal = RollPtrDecoder::decode_storage_to_internal(data, 7, UndoFormatProfile::UNRESOLVED);
    EXPECT_EQ(internal, 0u);
}

TEST(RollPtrDecoderTest, DecodeStorageToInternalWithProfile7Bytes) {
    uint64_t val = (1ULL << 52) | (0x1000ULL << 20) | 0x200ULL;
    uint8_t data[7];
    for (int i = 6; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    uint64_t internal = RollPtrDecoder::decode_storage_to_internal(data, 7, UndoFormatProfile::MYSQL8_7BYTE);
    auto decoded = RollPtrDecoder::decode(internal);
    EXPECT_EQ(decoded.page_no, 0x1000u);
    EXPECT_EQ(decoded.offset, 0x200u);
    EXPECT_EQ(decoded.flags, 0x1u);
}

TEST(RollPtrDecoderTest, DecodeStorageToInternalWithProfile6Bytes) {
    uint64_t val = (0x1000ULL << 20) | 0x200ULL;
    uint8_t data[6];
    for (int i = 5; i >= 0; i--) {
        data[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }

    uint64_t internal = RollPtrDecoder::decode_storage_to_internal(data, 6, UndoFormatProfile::LEGACY_6BYTE);
    auto decoded = RollPtrDecoder::decode(internal);
    EXPECT_EQ(decoded.page_no, 0x1000u);
    EXPECT_EQ(decoded.offset, 0x200u);
    EXPECT_EQ(decoded.flags, 0u);
}

} // namespace innodb
} // namespace dbrecovery

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}