/**
 * @file test_data_validation.cpp
 * @brief Unit tests for data validation functions
 */

#include <gtest/gtest.h>
#include "dbrecovery/innodb/data_validation.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

using namespace dbrecovery::innodb::validation;

class DataValidationTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ==================== DateTime Validation Tests ====================

TEST_F(DataValidationTest, DateTimeOldValid) {
    // Valid datetime: 2024-06-15 10:30:45
    // MySQL old datetime format: YYMMDDHHMMSS (each 2 digits, stored as integer)
    // 24 06 15 10 30 45 -> 240615103045 (but parsed as separate 2-digit pairs)
    // The packed value is read as YYMMDDHHMMSS where each is a separate 2-digit field
    // Actual format: YYYYMMDDHHMMSS as a 14-digit number
    // We need to pack it properly: 20240615103045
    // But the function parses digit by digit: year = value % 10000
    // Let's check the correct format
    uint64_t packed = 20240615103045ULL;  // Full format with 4-digit year
    EXPECT_TRUE(validate_datetime_old(packed));
}

TEST_F(DataValidationTest, DateTimeOldNull) {
    // Zero datetime represents NULL
    EXPECT_TRUE(validate_datetime_old(0));
}

TEST_F(DataValidationTest, DateTimeOldInvalidSecond) {
    // Invalid second (60)
    uint64_t packed = 20240615103060ULL;
    EXPECT_FALSE(validate_datetime_old(packed));
}

TEST_F(DataValidationTest, DateTimeOldInvalidHour) {
    // Invalid hour (25)
    uint64_t packed = 20240615250000ULL;
    EXPECT_FALSE(validate_datetime_old(packed));
}

TEST_F(DataValidationTest, DateTimeOldInvalidYear) {
    // Year too old (1900)
    uint64_t packed = 19000101000000ULL;
    EXPECT_FALSE(validate_datetime_old(packed));
}

TEST_F(DataValidationTest, DateTime2Valid) {
    EXPECT_TRUE(validate_datetime2(2024, 6, 15, 10, 30, 45));
    EXPECT_TRUE(validate_datetime2(1950, 1, 1, 0, 0, 0));
    EXPECT_TRUE(validate_datetime2(2050, 12, 31, 23, 59, 59));
}

TEST_F(DataValidationTest, DateTime2InvalidYear) {
    EXPECT_FALSE(validate_datetime2(1949, 6, 15, 10, 30, 45));  // Too old
    EXPECT_FALSE(validate_datetime2(2051, 6, 15, 10, 30, 45));  // Too new
}

TEST_F(DataValidationTest, DateTime2InvalidMonth) {
    // Month 0 is valid in MySQL (indicates zero date)
    // Month 13 is invalid
    EXPECT_FALSE(validate_datetime2(2024, 13, 15, 10, 30, 45));
}

TEST_F(DataValidationTest, DateTime2InvalidDay) {
    // Day 0 is valid in MySQL (indicates zero date)
    // Day 32 is invalid
    EXPECT_FALSE(validate_datetime2(2024, 6, 32, 10, 30, 45));
}

TEST_F(DataValidationTest, DateTime2InvalidHour) {
    EXPECT_FALSE(validate_datetime2(2024, 6, 15, 24, 30, 45));
    EXPECT_FALSE(validate_datetime2(2024, 6, 15, -1, 30, 45));
}

TEST_F(DataValidationTest, Time2Valid) {
    EXPECT_TRUE(validate_time2(10, 30, 45));
    EXPECT_TRUE(validate_time2(0, 0, 0));
    EXPECT_TRUE(validate_time2(838, 59, 59));
    EXPECT_TRUE(validate_time2(-838, 59, 59));
}

TEST_F(DataValidationTest, Time2InvalidHour) {
    EXPECT_FALSE(validate_time2(839, 30, 45));  // Too large
    EXPECT_FALSE(validate_time2(-839, 30, 45)); // Too negative
}

TEST_F(DataValidationTest, Time2InvalidMinute) {
    EXPECT_FALSE(validate_time2(10, 60, 45));
}

TEST_F(DataValidationTest, Time2InvalidSecond) {
    EXPECT_FALSE(validate_time2(10, 30, 60));
}

TEST_F(DataValidationTest, DateValid) {
    EXPECT_TRUE(validate_date(2024, 6, 15));
    EXPECT_TRUE(validate_date(1000, 1, 1));
    EXPECT_TRUE(validate_date(9999, 12, 31));
}

TEST_F(DataValidationTest, DateInvalidYear) {
    EXPECT_FALSE(validate_date(999, 6, 15));  // Too old
    EXPECT_FALSE(validate_date(10000, 6, 15)); // Too new
}

TEST_F(DataValidationTest, DateInvalidMonth) {
    EXPECT_FALSE(validate_date(2024, 0, 15));
    EXPECT_FALSE(validate_date(2024, 13, 15));
}

// ==================== String Validation Tests ====================

TEST_F(DataValidationTest, AsciiPrintableValid) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
    EXPECT_TRUE(validate_ascii_printable(data));
}

TEST_F(DataValidationTest, AsciiPrintableWithNewline) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o', '\n', 'W', 'o', 'r', 'l', 'd'};
    EXPECT_TRUE(validate_ascii_printable(data));
}

TEST_F(DataValidationTest, AsciiPrintableWithTab) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o', '\t', 'W', 'o', 'r', 'l', 'd'};
    EXPECT_TRUE(validate_ascii_printable(data));
}

TEST_F(DataValidationTest, AsciiPrintableInvalid) {
    std::vector<uint8_t> data = {'H', 'e', 0x01, 'l', 'o'};  // Control character
    EXPECT_FALSE(validate_ascii_printable(data));
}

TEST_F(DataValidationTest, AsciiPrintableEmpty) {
    std::vector<uint8_t> data;
    EXPECT_TRUE(validate_ascii_printable(data));
}

TEST_F(DataValidationTest, DigitsOnlyValid) {
    std::vector<uint8_t> data = {'1', '2', '3', '4', '5'};
    EXPECT_TRUE(validate_digits_only(data));
}

TEST_F(DataValidationTest, DigitsOnlyInvalid) {
    std::vector<uint8_t> data = {'1', '2', 'a', '4', '5'};
    EXPECT_FALSE(validate_digits_only(data));
}

TEST_F(DataValidationTest, Utf8AsciiValid) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    EXPECT_TRUE(validate_utf8(data));
}

TEST_F(DataValidationTest, Utf8TwoByteValid) {
    // 'é' = 0xC3 0xA9
    std::vector<uint8_t> data = {0xC3, 0xA9};
    EXPECT_TRUE(validate_utf8(data));
}

TEST_F(DataValidationTest, Utf8ThreeByteValid) {
    // '中' = 0xE4 0xB8 0xAD
    std::vector<uint8_t> data = {0xE4, 0xB8, 0xAD};
    EXPECT_TRUE(validate_utf8(data));
}

TEST_F(DataValidationTest, Utf8FourByteValid) {
    // Emoji '😀' = 0xF0 0x9F 0x98 0x80
    std::vector<uint8_t> data = {0xF0, 0x9F, 0x98, 0x80};
    EXPECT_TRUE(validate_utf8(data));
}

TEST_F(DataValidationTest, Utf8InvalidSequence) {
    // Invalid: 0xC2 without continuation byte
    std::vector<uint8_t> data = {0xC2, 'a'};
    EXPECT_FALSE(validate_utf8(data));
}

// ==================== Integer Validation Tests ====================

TEST_F(DataValidationTest, IntRangeValid) {
    EXPECT_TRUE(validate_int_range(50, 0, 100));
    EXPECT_TRUE(validate_int_range(0, 0, 100));
    EXPECT_TRUE(validate_int_range(100, 0, 100));
}

TEST_F(DataValidationTest, IntRangeInvalid) {
    EXPECT_FALSE(validate_int_range(-1, 0, 100));
    EXPECT_FALSE(validate_int_range(101, 0, 100));
}

TEST_F(DataValidationTest, UintRangeValid) {
    EXPECT_TRUE(validate_uint_range(50, 0, 100));
    EXPECT_TRUE(validate_uint_range(0, 0, 100));
    EXPECT_TRUE(validate_uint_range(100, 0, 100));
}

TEST_F(DataValidationTest, UintRangeInvalid) {
    EXPECT_FALSE(validate_uint_range(101, 0, 100));
}

// ==================== ENUM/SET Validation Tests ====================

TEST_F(DataValidationTest, EnumIndexValid) {
    EXPECT_TRUE(validate_enum_index(1, 5));  // First enum value
    EXPECT_TRUE(validate_enum_index(3, 5));  // Middle value
    EXPECT_TRUE(validate_enum_index(5, 5));  // Last value
}

TEST_F(DataValidationTest, EnumIndexInvalid) {
    EXPECT_FALSE(validate_enum_index(0, 5));  // 0 is invalid
    EXPECT_FALSE(validate_enum_index(6, 5));  // Out of range
}

TEST_F(DataValidationTest, SetBitsValid) {
    EXPECT_TRUE(validate_set_bits(0, 5));        // No values set
    EXPECT_TRUE(validate_set_bits(1, 5));        // First value
    EXPECT_TRUE(validate_set_bits(3, 5));        // First two values
    EXPECT_TRUE(validate_set_bits(31, 5));       // All 5 values (bits 0-4)
}

TEST_F(DataValidationTest, SetBitsInvalid) {
    EXPECT_FALSE(validate_set_bits(32, 5));      // Bit 5 is invalid (only 0-4 allowed)
    EXPECT_FALSE(validate_set_bits(64, 5));      // Bit 6 is invalid
}

// ==================== Field Limits Tests ====================

TEST_F(DataValidationTest, FieldLimitsCharAscii) {
    FieldLimits limits;
    limits.char_ascii_only = true;
    limits.can_be_null = true;

    std::vector<uint8_t> valid_data = {'H', 'e', 'l', 'l', 'o'};
    EXPECT_TRUE(limits.validate(valid_data, false));

    std::vector<uint8_t> invalid_data = {'H', 'e', 0x01, 'l', 'o'};
    EXPECT_FALSE(limits.validate(invalid_data, false));
}

TEST_F(DataValidationTest, FieldLimitsCharDigits) {
    FieldLimits limits;
    limits.char_digits_only = true;

    std::vector<uint8_t> valid_data = {'1', '2', '3'};
    EXPECT_TRUE(limits.validate(valid_data, false));

    std::vector<uint8_t> invalid_data = {'1', 'a', '3'};
    EXPECT_FALSE(limits.validate(invalid_data, false));
}

TEST_F(DataValidationTest, FieldLimitsNull) {
    FieldLimits limits;
    limits.can_be_null = true;

    std::vector<uint8_t> empty_data;
    EXPECT_TRUE(limits.validate(empty_data, true));

    limits.can_be_null = false;
    EXPECT_FALSE(limits.validate(empty_data, true));
}

TEST_F(DataValidationTest, FieldLimitsLength) {
    FieldLimits limits;
    limits.char_min_len = 2;
    limits.char_max_len = 10;

    std::vector<uint8_t> valid_data(5, 'a');
    EXPECT_TRUE(limits.validate(valid_data, false));

    std::vector<uint8_t> too_short(1, 'a');
    EXPECT_FALSE(limits.validate(too_short, false));

    std::vector<uint8_t> too_long(15, 'a');
    EXPECT_FALSE(limits.validate(too_long, false));
}

// ==================== Infimum/Supremum Validation Tests ====================

TEST_F(DataValidationTest, InfimumValid) {
    // Create a page with valid infimum marker at offset 99
    std::vector<uint8_t> page(16384, 0);
    page[99] = 0x02;  // REC_STATUS_INFIMUM
    const char* infimum = "infimum";
    for (int i = 0; infimum[i]; i++) {
        page[99 + 5 + i] = static_cast<uint8_t>(infimum[i]);
    }

    EXPECT_TRUE(validate_infimum(page, 99));
}

TEST_F(DataValidationTest, InfimumInvalid) {
    std::vector<uint8_t> page(16384, 0);
    page[99] = 0x02;
    // No "infimum" string

    EXPECT_FALSE(validate_infimum(page, 99));
}

TEST_F(DataValidationTest, SupremumValid) {
    // Create a page with valid supremum marker at offset 112
    std::vector<uint8_t> page(16384, 0);
    page[112] = 0x03;  // REC_STATUS_SUPREMUM
    const char* supremum = "supremum";
    for (int i = 0; supremum[i]; i++) {
        page[112 + 5 + i] = static_cast<uint8_t>(supremum[i]);
    }

    EXPECT_TRUE(validate_supremum(page, 112));
}

TEST_F(DataValidationTest, SupremumInvalid) {
    std::vector<uint8_t> page(16384, 0);
    page[112] = 0x03;
    // No "supremum" string

    EXPECT_FALSE(validate_supremum(page, 112));
}

TEST_F(DataValidationTest, PageSystemRecordsValid) {
    // Create a valid COMPACT page with infimum/supremum
    // validate_infimum checks: page_data[offset + 5] onwards for "infimum" string
    std::vector<uint8_t> page(16384, 0);

    // FIL header
    page[24] = 0x45; page[25] = 0xBF;  // FIL_PAGE_INDEX

    // Page header - COMPACT format
    // n_heap is BIG-ENDIAN: byte 42 is high byte, byte 43 is low byte
    // For COMPACT format: n_heap has bit 15 set (0x8000)
    // So we need n_heap = 0x8002 (2 records with COMPACT flag)
    page[42] = 0x80; page[43] = 0x02;  // n_heap = 0x8002 (BIG-ENDIAN)

    // Infimum at offset 99 - validate_infimum checks offset+5 for "infimum" string
    // So we need "infimum" at offset 99+5 = 104
    const char* infimum = "infimum";
    for (int i = 0; infimum[i]; i++) {
        page[99 + 5 + i] = static_cast<uint8_t>(infimum[i]);
    }

    // Supremum at offset 112 - validate_supremum checks offset+5 for "supremum" string
    // So we need "supremum" at offset 112+5 = 117
    const char* supremum = "supremum";
    for (int i = 0; supremum[i]; i++) {
        page[112 + 5 + i] = static_cast<uint8_t>(supremum[i]);
    }

    EXPECT_TRUE(validate_page_system_records(page, 16384));
}

TEST_F(DataValidationTest, PageSystemRecordsInvalidInfimum) {
    std::vector<uint8_t> page(16384, 0);

    // FIL header
    page[24] = 0x45; page[25] = 0xBF;

    // Page header - COMPACT format
    page[42] = 0; page[43] = 0x80 | 2;

    // Invalid infimum (no marker string)
    page[99] = 0x02;

    // Valid supremum
    page[112] = 0x03;
    const char* supremum = "supremum";
    for (int i = 0; supremum[i]; i++) {
        page[112 + 5 + i] = static_cast<uint8_t>(supremum[i]);
    }

    EXPECT_FALSE(validate_page_system_records(page, 16384));
}

// ==================== Combined Validation Tests ====================

TEST_F(DataValidationTest, FullRecordValidation) {
    // Test validation of a complete record with multiple fields
    FieldLimits string_limits;
    string_limits.char_ascii_only = true;
    string_limits.char_max_len = 100;

    // Valid string field
    std::vector<uint8_t> name_data = {'J', 'o', 'h', 'n', ' ', 'D', 'o', 'e'};
    EXPECT_TRUE(string_limits.validate(name_data, false));

    // Valid datetime
    EXPECT_TRUE(validate_datetime2(2024, 6, 15, 14, 30, 0));

    // Valid enum
    EXPECT_TRUE(validate_enum_index(2, 5));

    // All fields valid - record is valid
}