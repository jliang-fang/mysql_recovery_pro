/**
 * @file test_multi_version_compat.cpp
 * @brief 多版本MySQL兼容性测试
 *
 * 测试不同MySQL版本的页解析功能
 */

#include "dbrecovery/common/version_compat.hpp"
#include "dbrecovery/common/endian_compat.hpp"
#include "page/page_parser.h"
#include "page/page_parser_factory.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cassert>

using namespace dbrecovery;
using namespace dbrecovery::endian;
using namespace mysql_recovery;

// ==================== 测试工具函数 ====================

/**
 * @brief 创建测试页数据
 */
std::vector<uint8_t> create_test_page(mysql_recovery::MySQLVersion version, mysql_recovery::PageType page_type,
                                      size_t page_size = 16384) {
    std::vector<uint8_t> page(page_size, 0);

    // FIL Header (offset 0-37)
    // 0-3: FIL_PAGE_PREV (default 0)
    endian::write_le32(page.data(), 0, 0);
    // 4-7: FIL_PAGE_NEXT (default 0)
    endian::write_le32(page.data(), 0, 4);
    // 8-15: FIL_PAGE_LSN (8 bytes)
    endian::write_le64(page.data(), 1000, 8);
    // 16-17: FIL_PAGE_TYPE (2 bytes)
    endian::write_le16(page.data(), static_cast<uint16_t>(page_type), 16);
    // 18-21: FIL_PAGE_SPACE_ID (4 bytes)
    endian::write_le32(page.data(), 1, 18);
    // 22-23: FIL_PAGE_SPACE_VERSION (2 bytes)
    endian::write_le16(page.data(), 1, 22);
    
    // 24-31: FIL_PAGE_FILE_FLUSH_LSN (8 bytes) - MySQL 8.0+
    if (version >= MySQLVersion::MYSQL_80) {
        endian::write_le64(page.data(), 2000, 24);
    } else {
        endian::write_le64(page.data(), 0, 24);
    }
    
    // 32-33: FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID (2 bytes)
    endian::write_le16(page.data(), 1, 32);
    
    // 34-35: FIL_PAGE_FILE_FLUSH_LSN_HIGH (4 bytes) - MySQL 8.4+
    if (version >= MySQLVersion::MYSQL_84) {
        endian::write_le32(page.data(), 0, 34);
    }
    
    // 36-37: FIL_PAGE_PAGE_VERSION (2 bytes) - MySQL 8.0+
    if (version >= MySQLVersion::MYSQL_80) {
        endian::write_le16(page.data(), version >= MySQLVersion::MYSQL_84 ? 2 : 1, 36);
    }
    
    // DATA START (offset 38-39)
    endian::write_le16(page.data(), 96, 38);
    
    // PAGE HEADER - Version specific
    if (version >= MySQLVersion::MYSQL_56) {
        // PAGE_N_DIR_SLOTS (48-49)
        endian::write_le16(page.data(), 2, 48);
        // PAGE_HEAP_TOP (50-51)
        endian::write_le16(page.data(), 112, 50);
        // PAGE_HEAP_FREE (52-53)
        endian::write_le16(page.data(), 0, 52);
        // PAGE_N_FREE (54-55)
        endian::write_le16(page.data(), 0, 54);
        // PAGE_N_RECS (56-57)
        endian::write_le16(page.data(), 0, 56);
        // PAGE_MAX_TRX_ID (58-61) - MySQL 5.6+
        if (version >= MySQLVersion::MYSQL_56 && version < MySQLVersion::MYSQL_80) {
            endian::write_le32(page.data(), 100, 58);
        }
        // PAGE_LEVEL (62-63)
        endian::write_le16(page.data(), 0, 62);
        // PAGE_INDEX_ID (64-71)
        endian::write_le64(page.data(), 1, 64);
        // PAGE_BTR_SEG_LEAF (72-83)
        endian::write_le32(page.data(), 1, 72);
        endian::write_le16(page.data(), 0, 76);
        // PAGE_BTR_SEG_TOP (84-95)
        endian::write_le32(page.data(), 1, 84);
        endian::write_le16(page.data(), 0, 88);
    } else {
        // Old format (5.5/5.1)
        endian::write_le16(page.data(), 2, 48);
        endian::write_le16(page.data(), 112, 50);
        endian::write_le16(page.data(), 0, 52);
        endian::write_le16(page.data(), 0, 54);
        endian::write_le16(page.data(), 0, 56);
        endian::write_le16(page.data(), 0, 62);
    }
    
    // SDI Signature (offset 64-71) - MySQL 8.0+
    if (version >= MySQLVersion::MYSQL_80 && page_type == PageType::SDI) {
        constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
        std::memcpy(page.data() + 64, SDI_SIG, 8);
    }
    
    // Checksum (offset 0-3, overwritten at the end)
    uint32_t checksum = endian::crc32(page.data() + 4, page_size - 4);
    endian::write_le32(page.data(), checksum, 0);
    
    return page;
}

/**
 * @brief 测试版本检测
 */
bool test_version_detection() {
    std::cout << "Testing version detection..." << std::endl;
    
    // Test MySQL 5.6
    {
        auto page = create_test_page(MySQLVersion::MYSQL_56, PageType::INDEX);
        auto detected = PageFactory::detect_version(page.data(), page.size());
        if (detected != MySQLVersion::MYSQL_56) {
            std::cerr << "Failed: Expected MySQL 5.6, got " 
                      << static_cast<int>(detected) << std::endl;
            return false;
        }
        std::cout << "  MySQL 5.6 detection: PASS" << std::endl;
    }
    
    // Test MySQL 5.7
    {
        auto page = create_test_page(MySQLVersion::MYSQL_57, PageType::INDEX);
        auto detected = PageFactory::detect_version(page.data(), page.size());
        if (detected != MySQLVersion::MYSQL_57) {
            std::cerr << "Failed: Expected MySQL 5.7, got " 
                      << static_cast<int>(detected) << std::endl;
            return false;
        }
        std::cout << "  MySQL 5.7 detection: PASS" << std::endl;
    }
    
    // Test MySQL 8.0
    {
        auto page = create_test_page(MySQLVersion::MYSQL_80, PageType::INDEX);
        auto detected = PageFactory::detect_version(page.data(), page.size());
        if (detected != MySQLVersion::MYSQL_80) {
            std::cerr << "Failed: Expected MySQL 8.0, got " 
                      << static_cast<int>(detected) << std::endl;
            return false;
        }
        std::cout << "  MySQL 8.0 detection: PASS" << std::endl;
    }
    
    // Test MySQL 8.4
    {
        auto page = create_test_page(MySQLVersion::MYSQL_84, PageType::INDEX);
        auto detected = PageFactory::detect_version(page.data(), page.size());
        if (detected != MySQLVersion::MYSQL_84) {
            std::cerr << "Failed: Expected MySQL 8.4, got " 
                      << static_cast<int>(detected) << std::endl;
            return false;
        }
        std::cout << "  MySQL 8.4 detection: PASS" << std::endl;
    }
    
    // Test MySQL 9.0
    {
        auto page = create_test_page(MySQLVersion::MYSQL_90, PageType::INDEX);
        auto detected = PageFactory::detect_version(page.data(), page.size());
        if (detected != MySQLVersion::MYSQL_90) {
            std::cerr << "Failed: Expected MySQL 9.0, got " 
                      << static_cast<int>(detected) << std::endl;
            return false;
        }
        std::cout << "  MySQL 9.0 detection: PASS" << std::endl;
    }
    
    return true;
}

/**
 * @brief 测试页创建
 */
bool test_page_creation() {
    std::cout << "Testing page creation..." << std::endl;
    
    // Test MySQL 5.6
    {
        auto page = create_test_page(MySQLVersion::MYSQL_56, PageType::INDEX);
        auto header = PageFactory::create_page_header(page.data(), page.size());
        if (!header) {
            std::cerr << "Failed: Could not create page header for MySQL 5.6" << std::endl;
            return false;
        }
        if (header->get_version() != MySQLVersion::MYSQL_56) {
            std::cerr << "Failed: Wrong version in header" << std::endl;
            return false;
        }
        std::cout << "  MySQL 5.6 page creation: PASS" << std::endl;
    }
    
    // Test MySQL 8.0
    {
        auto page = create_test_page(MySQLVersion::MYSQL_80, PageType::INDEX);
        auto header = PageFactory::create_page_header(page.data(), page.size());
        if (!header) {
            std::cerr << "Failed: Could not create page header for MySQL 8.0" << std::endl;
            return false;
        }
        if (header->get_version() != MySQLVersion::MYSQL_80) {
            std::cerr << "Failed: Wrong version in header" << std::endl;
            return false;
        }
        std::cout << "  MySQL 8.0 page creation: PASS" << std::endl;
    }
    
    // Test SDI page
    {
        auto page = create_test_page(MySQLVersion::MYSQL_80, PageType::SDI);
        auto header = PageFactory::create_page_header(page.data(), page.size());
        if (!header) {
            std::cerr << "Failed: Could not create page header for SDI page" << std::endl;
            return false;
        }
        std::cout << "  SDI page creation: PASS" << std::endl;
    }
    
    return true;
}

/**
 * @brief 测试行格式检测
 */
bool test_row_format_detection() {
    std::cout << "Testing row format detection..." << std::endl;
    
    // Test REDUNDANT format (MySQL 5.0/5.5)
    {
        auto format = PageFactory::detect_row_format_from_record(
            "\x06\x00\x00\x00\x00\x00", 6
        );
        if (format != RowFormat::REDUNDANT) {
            std::cerr << "Failed: Expected REDUNDANT, got " 
                      << static_cast<int>(format) << std::endl;
            return false;
        }
        std::cout << "  REDUNDANT format detection: PASS" << std::endl;
    }
    
    // Test COMPACT format
    {
        auto format = PageFactory::detect_row_format_from_record(
            "\x05\x00\x00\x00\x00\x00", 6
        );
        if (format != RowFormat::COMPACT) {
            std::cerr << "Failed: Expected COMPACT, got " 
                      << static_cast<int>(format) << std::endl;
            return false;
        }
        std::cout << "  COMPACT format detection: PASS" << std::endl;
    }
    
    // Test DYNAMIC format
    {
        uint8_t record[] = {0x05, 0x20, 0x00, 0x00, 0x00, 0x00};  // info_bits & 0x20 = 1
        auto format = PageFactory::detect_row_format_from_record(record, 6);
        if (format != RowFormat::DYNAMIC) {
            std::cerr << "Failed: Expected DYNAMIC, got " 
                      << static_cast<int>(format) << std::endl;
            return false;
        }
        std::cout << "  DYNAMIC format detection: PASS" << std::endl;
    }
    
    return true;
}

/**
 * @brief 测试字节序转换
 */
bool test_endian_conversion() {
    std::cout << "Testing endian conversion..." << std::endl;

    // Test 16-bit conversion
    {
        uint16_t value = 0x1234;
        uint16_t converted = endian::le16toh(endian::htole16(value));
        if (converted != value) {
            std::cerr << "Failed: 16-bit endian conversion" << std::endl;
            return false;
        }
        std::cout << "  16-bit endian conversion: PASS" << std::endl;
    }
    
    // Test 32-bit conversion
    {
        uint32_t value = 0x12345678;
        uint32_t converted = endian::le32toh(endian::htole32(value));
        if (converted != value) {
            std::cerr << "Failed: 32-bit endian conversion" << std::endl;
            return false;
        }
        std::cout << "  32-bit endian conversion: PASS" << std::endl;
    }
    
    // Test 64-bit conversion
    {
        uint64_t value = 0x123456789ABCDEF0ULL;
        uint64_t converted = endian::le64toh(endian::htole64(value));
        if (converted != value) {
            std::cerr << "Failed: 64-bit endian conversion" << std::endl;
            return false;
        }
        std::cout << "  64-bit endian conversion: PASS" << std::endl;
    }
    
    // Test read/write functions
    {
        uint8_t data[8];
        uint64_t value = 0x123456789ABCDEF0ULL;
        endian::write_le64(data, value);
        uint64_t read_value = endian::read_le64(data);
        if (read_value != value) {
            std::cerr << "Failed: read/write le64" << std::endl;
            return false;
        }
        std::cout << "  read/write le64: PASS" << std::endl;
    }
    
    return true;
}

/**
 * @brief 测试CRC32校验
 */
bool test_crc32() {
    std::cout << "Testing CRC32..." << std::endl;
    
    // Test CRC32 calculation
    {
        uint8_t data[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
        uint32_t crc = endian::crc32(data, sizeof(data));
        
        // Verify CRC is consistent
        uint32_t crc2 = endian::crc32(data, sizeof(data));
        if (crc != crc2) {
            std::cerr << "Failed: CRC32 not consistent" << std::endl;
            return false;
        }
        std::cout << "  CRC32 consistency: PASS (CRC = 0x" << std::hex << crc << ")" << std::dec << std::endl;
    }
    
    return true;
}

/**
 * @brief 测试页类型检测
 */
bool test_page_type_detection() {
    std::cout << "Testing page type detection..." << std::endl;
    
    // Test INDEX page
    {
        auto page = create_test_page(MySQLVersion::MYSQL_80, PageType::INDEX);
        uint16_t page_type = endian::read_le16(page.data(), 16);
        if (page_type != static_cast<uint16_t>(PageType::INDEX)) {
            std::cerr << "Failed: INDEX page type" << std::endl;
            return false;
        }
        std::cout << "  INDEX page type: PASS" << std::endl;
    }
    
    // Test SDI page
    {
        auto page = create_test_page(MySQLVersion::MYSQL_80, PageType::SDI);
        uint16_t page_type = endian::read_le16(page.data(), 16);
        if (page_type != static_cast<uint16_t>(PageType::SDI)) {
            std::cerr << "Failed: SDI page type" << std::endl;
            return false;
        }
        std::cout << "  SDI page type: PASS" << std::endl;
    }
    
    // Test UNDO_LOG page
    {
        auto page = create_test_page(MySQLVersion::MYSQL_80, PageType::UNDO_LOG);
        uint16_t page_type = endian::read_le16(page.data(), 16);
        if (page_type != static_cast<uint16_t>(PageType::UNDO_LOG)) {
            std::cerr << "Failed: UNDO_LOG page type" << std::endl;
            return false;
        }
        std::cout << "  UNDO_LOG page type: PASS" << std::endl;
    }
    
    return true;
}

/**
 * @brief 主测试函数
 */
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "MySQL Multi-Version Compatibility Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    bool all_passed = true;
    
    // Run all tests
    all_passed &= test_version_detection();
    std::cout << std::endl;
    
    all_passed &= test_page_creation();
    std::cout << std::endl;
    
    all_passed &= test_row_format_detection();
    std::cout << std::endl;
    
    all_passed &= test_endian_conversion();
    std::cout << std::endl;
    
    all_passed &= test_crc32();
    std::cout << std::endl;
    
    all_passed &= test_page_type_detection();
    std::cout << std::endl;
    
    // Summary
    std::cout << "========================================" << std::endl;
    if (all_passed) {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << "Some tests FAILED!" << std::endl;
        return 1;
    }
}
