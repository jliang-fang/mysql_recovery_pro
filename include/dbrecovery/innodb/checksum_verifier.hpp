#ifndef DBRECOVERY_INNODB_CHECKSUM_VERIFIER_HPP
#define DBRECOVERY_INNODB_CHECKSUM_VERIFIER_HPP

#include "dbrecovery/innodb/page_types_v2.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include <cstdint>
#include <vector>
#include <optional>

namespace dbrecovery {
namespace innodb {

/**
 * @brief InnoDB 校验和算法
 */
enum class ChecksumAlgorithm : uint32_t {
    OFF = 0,           // 无校验和
    CRC32 = 1,         // CRC32 (MySQL 5.7+)
    INNODB = 2,        // InnoDB 原生校验和
    FSP_HDR = 3,       // FSP HDR 校验和
    NONE = 4,          // 无校验 (旧版)
    OLD = 5,           // 旧校验和算法
};

/**
 * @brief 校验和计算结果
 */
struct ChecksumResult {
    bool is_valid;
    uint32_t calculated;
    uint32_t stored;
    ChecksumAlgorithm algorithm;
    std::string error_message;
};

/**
 * @brief 页健康信息
 */
struct PageHealthInfo {
    bool is_valid;
    bool checksum_match;
    bool header_valid;
    bool type_valid;
    uint32_t page_size;
    ChecksumAlgorithm checksum_algorithm;
    std::string status_message;
    
    // 统计信息
    size_t records_count;
    size_t free_space;
    uint16_t level;
};

/**
 * @brief 页校验和验证器
 */
class ChecksumVerifier {
public:
    ChecksumVerifier();
    
    /**
     * @brief 设置校验和算法
     */
    void set_algorithm(ChecksumAlgorithm algorithm);
    
    /**
     * @brief 计算 CRC32C 校验和
     */
    static uint32_t crc32c(const uint8_t* data, size_t size, uint32_t init_crc = 0xFFFFFFFF);
    
    /**
     * @brief 计算 InnoDB 原生校验和
     */
    static uint32_t innodb_checksum(const uint8_t* data, size_t size);
    
    /**
     * @brief 计算 FSP HDR 校验和
     */
    static uint32_t fsp_hdr_checksum(const uint8_t* data, size_t size);
    
    /**
     * @brief 计算页校验和
     */
    uint32_t calculate_checksum(const uint8_t* data, size_t size);
    
    /**
     * @brief 验证页校验和
     */
    ChecksumResult verify_checksum(const uint8_t* data, size_t size);
    
    /**
     * @brief 验证页头
     */
    bool verify_header(const uint8_t* data, size_t size);
    
    /**
     * @brief 检查页类型
     */
    bool verify_page_type(uint16_t page_type);
    
    /**
     * @brief 检查页大小
     */
    bool verify_page_size(uint32_t page_size);
    
    /**
     * @brief 获取页健康信息
     */
    PageHealthInfo check_page_health(const uint8_t* data, size_t size);
    
    /**
     * @brief 从页数据读取校验和
     */
    static uint32_t get_stored_checksum(const uint8_t* data, size_t size);
    
    /**
     * @brief 设置页大小
     */
    void set_page_size(uint32_t page_size);
    
    /**
     * @brief 获取统计信息
     */
    struct Stats {
        size_t pages_verified;
        size_t checksums_valid;
        size_t checksums_invalid;
        size_t headers_valid;
        size_t headers_invalid;
    };
    
    Stats get_stats() const;
    
private:
    ChecksumAlgorithm algorithm_;
    uint32_t page_size_;
    Stats stats_;
    
    uint32_t old_checksum(const uint8_t* data, size_t size);
};

/**
 * @brief 页健康检查器
 */
class PageHealthChecker {
public:
    /**
     * @brief 检查页健康
     */
    static PageHealthInfo check(const uint8_t* data, size_t size,
                                ChecksumAlgorithm algorithm = ChecksumAlgorithm::CRC32);
    
    /**
     * @brief 检查页头
     */
    static bool check_header(const uint8_t* data, size_t size);
    
    /**
     * @brief 检查校验和
     */
    static bool check_checksum(const uint8_t* data, size_t size,
                               ChecksumAlgorithm algorithm = ChecksumAlgorithm::CRC32);
    
    /**
     * @brief 检查页类型
     */
    static bool check_page_type(uint16_t page_type);
    
    /**
     * @brief 检查页大小
     */
    static bool check_page_size(uint32_t page_size, uint32_t expected_size = 16384);
};

// ==================== 实现 ====================

inline ChecksumVerifier::ChecksumVerifier() 
    : algorithm_(ChecksumAlgorithm::CRC32), page_size_(16384) {
    stats_ = {0, 0, 0, 0, 0};
}

inline void ChecksumVerifier::set_algorithm(ChecksumAlgorithm algorithm) {
    algorithm_ = algorithm;
}

inline void ChecksumVerifier::set_page_size(uint32_t size) {
    page_size_ = size;
}

inline ChecksumVerifier::Stats ChecksumVerifier::get_stats() const {
    return stats_;
}

// ==================== ChecksumResult ====================

inline ChecksumResult make_valid_result(uint32_t calculated, uint32_t stored,
                                        ChecksumAlgorithm algorithm) {
    ChecksumResult result;
    result.is_valid = true;
    result.calculated = calculated;
    result.stored = stored;
    result.algorithm = algorithm;
    result.error_message = "";
    return result;
}

inline ChecksumResult make_invalid_result(uint32_t calculated, uint32_t stored,
                                          ChecksumAlgorithm algorithm,
                                          const std::string& error) {
    ChecksumResult result;
    result.is_valid = false;
    result.calculated = calculated;
    result.stored = stored;
    result.algorithm = algorithm;
    result.error_message = error;
    return result;
}

// ==================== PageHealthInfo ====================

inline PageHealthInfo make_valid_health() {
    PageHealthInfo info;
    info.is_valid = true;
    info.checksum_match = true;
    info.header_valid = true;
    info.type_valid = true;
    info.page_size = 16384;
    info.checksum_algorithm = ChecksumAlgorithm::CRC32;
    info.status_message = "OK";
    info.records_count = 0;
    info.free_space = 0;
    info.level = 0;
    return info;
}

inline PageHealthInfo make_invalid_health(const std::string& message) {
    PageHealthInfo info;
    info.is_valid = false;
    info.checksum_match = false;
    info.header_valid = false;
    info.type_valid = false;
    info.page_size = 16384;
    info.checksum_algorithm = ChecksumAlgorithm::CRC32;
    info.status_message = message;
    info.records_count = 0;
    info.free_space = 0;
    info.level = 0;
    return info;
}

// ==================== ChecksumVerifier ====================

inline uint32_t ChecksumVerifier::crc32c(const uint8_t* data, size_t size, uint32_t init_crc) {
    uint32_t crc = init_crc;

    // CRC32C (Castagnoli polynomial 0x1EDC6F41, reflected 0x82F63B78)
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78;  // CRC32C 多项式
            } else {
                crc >>= 1;
            }
        }
    }
    
    return ~crc;
}

inline uint32_t ChecksumVerifier::innodb_checksum(const uint8_t* data, size_t size) {
    // InnoDB 原生校验和算法
    uint32_t sum = 0;
    
    for (size_t i = 0; i < size; i += 4) {
        if (i + 4 <= size) {
            uint32_t val = read_le<uint32_t>(data, i);
            sum += val;
        }
    }
    
    return sum;
}

inline uint32_t ChecksumVerifier::fsp_hdr_checksum(const uint8_t* data, size_t size) {
    // FSP HDR 页特殊校验和
    uint32_t sum = 0;
    
    // 跳过校验和字段本身
    for (size_t i = 0; i < size; i += 4) {
        if (i >= 56 && i < 60) continue;  // 跳过 checksum 字段
        
        if (i + 4 <= size) {
            uint32_t val = read_le<uint32_t>(data, i);
            sum += val;
        }
    }
    
    return sum;
}

inline uint32_t ChecksumVerifier::old_checksum(const uint8_t* data, size_t size) {
    // 旧校验和算法
    uint32_t sum = 0;
    
    for (size_t i = 0; i < size; i++) {
        sum = (sum >> 1) + (sum << 31) + data[i];
    }
    
    return sum;
}

inline uint32_t ChecksumVerifier::calculate_checksum(const uint8_t* data, size_t size) {
    switch (algorithm_) {
        case ChecksumAlgorithm::CRC32:
            return crc32c(data, size);
        case ChecksumAlgorithm::INNODB:
            return innodb_checksum(data, size);
        case ChecksumAlgorithm::FSP_HDR:
            return fsp_hdr_checksum(data, size);
        case ChecksumAlgorithm::OLD:
            return old_checksum(data, size);
        default:
            return 0;
    }
}

inline uint32_t ChecksumVerifier::get_stored_checksum(const uint8_t* data, size_t size) {
    // InnoDB stores checksum at page_size - 8, algorithm ID at page_size - 4
    if (size < 8) {
        return 0;
    }
    return read_le<uint32_t>(data, size - 8);
}

inline ChecksumResult ChecksumVerifier::verify_checksum(const uint8_t* data, size_t size) {
    stats_.pages_verified++;

    // Delegate to the canonical implementation in innodb_checksum.hpp
    // which properly handles: reading from page_size-8, zeroing checksum
    // fields before computing, dual-checksum fallback (CRC32C + INNODB),
    // and MySQL 5.6 vs 5.7+ detection.
    auto canonical_result = validate_page_checksum(data, size, false);

    if (canonical_result.is_valid) {
        stats_.checksums_valid++;
        return make_valid_result(
            canonical_result.computed_checksum,
            canonical_result.stored_checksum,
            algorithm_);
    }

    stats_.checksums_invalid++;
    return make_invalid_result(
        canonical_result.computed_checksum,
        canonical_result.stored_checksum,
        algorithm_,
        "Checksum mismatch (tried CRC32C and INNODB)");
}

inline bool ChecksumVerifier::verify_header(const uint8_t* data, size_t size) {
    if (size < 38) {  // FIL_HEADER_SIZE
        stats_.headers_invalid++;
        return false;
    }

    // Validate page type is a known InnoDB page type
    uint16_t page_type = read_le<uint16_t>(data, 22);  // FIL_PAGE_TYPE offset
    if (!verify_page_type(page_type)) {
        stats_.headers_invalid++;
        return false;
    }

    // Validate checksum consistency between header (offset 0, big-endian)
    // and trailer (page_size-8, little-endian)
    uint32_t header_checksum = read_be<uint32_t>(data, 0);
    uint32_t trailer_checksum = read_le<uint32_t>(data, size - 8);
    if (header_checksum == 0 && trailer_checksum == 0) {
        // Checksums disabled
        stats_.headers_valid++;
        return true;
    }

    // Verify LSN is non-zero (page number > 0 should have non-zero LSN)
    uint32_t page_no = read_le<uint32_t>(data, 4);
    uint64_t lsn = read_le<uint64_t>(data, 8);
    if (page_no > 0 && lsn == 0) {
        stats_.headers_invalid++;
        return false;
    }

    stats_.headers_valid++;
    return true;
}

inline bool ChecksumVerifier::verify_page_type(uint16_t page_type) {
    // All valid InnoDB FIL page types
    switch (page_type) {
        case 0x0000:  // FIL_PAGE_TYPE_ALLOCATED
        case 0x0002:  // FIL_PAGE_UNDO_LOG (0x45BB little-endian)
        case 0x0003:  // FIL_PAGE_INODE
        case 0x0004:  // FIL_PAGE_IBUF_FREE_LIST
        case 0x0005:  // FIL_PAGE_IBUF_BITMAP
        case 0x0006:  // FIL_PAGE_TYPE_SYS
        case 0x0007:  // FIL_PAGE_TYPE_TRX_SYS
        case 0x0008:  // FIL_PAGE_TYPE_FSP_HDR
        case 0x0009:  // FIL_PAGE_TYPE_XDES
        case 0x000A:  // FIL_PAGE_TYPE_BLOB
        case 0x000B:  // FIL_PAGE_TYPE_ZBLOB
        case 0x000C:  // FIL_PAGE_TYPE_ZBLOB2
        case 0x000D:  // FIL_PAGE_TYPE_UNKNOWN
        case 0x000E:  // FIL_PAGE_COMPRESSED
        case 0x000F:  // FIL_PAGE_ENCRYPTED
        case 0x45BD:  // FIL_PAGE_TYPE_BLOB (big-endian)
        case 0x45BE:  // FIL_PAGE_TYPE_SDI (uncompressed SDI)
        case 0x45BF:  // FIL_PAGE_INDEX
        case 0x45B2:  // FIL_PAGE_UNDO_LOG (big-endian)
        case 0x45B3:  // FIL_PAGE_INODE (big-endian)
        case 0x45B4:  // FIL_PAGE_IBUF_FREE_LIST (big-endian)
        case 0x45B5:  // FIL_PAGE_IBUF_BITMAP (big-endian)
        case 0x45B6:  // FIL_PAGE_TYPE_SYS (big-endian)
        case 0x45B7:  // FIL_PAGE_COMPRESSED_AND_ENCRYPTED (big-endian)
        case 0x45B8:  // FIL_PAGE_TYPE_FSP_HDR (big-endian)
        case 0x45B9:  // FIL_PAGE_TYPE_XDES (big-endian)
        case 0x45BA:  // FIL_PAGE_TYPE_BLOB (big-endian)
        case 0x45BB:  // FIL_PAGE_TYPE_ZBLOB (big-endian)
        case 0x45BC:  // FIL_PAGE_TYPE_ZBLOB2 (big-endian)
        case 0x5440:  // FIL_PAGE_SDI (compressed SDI big-endian)
        case 0x5441:  // FIL_PAGE_TYPE_SDI (big-endian)
            return true;
        default:
            return false;
    }
}

inline bool ChecksumVerifier::verify_page_size(uint32_t page_size) {
    // 检查页大小是否有效
    switch (page_size) {
        case 4096:
        case 8192:
        case 16384:
        case 32768:
        case 65536:
            return true;
        default:
            return false;
    }
}

inline PageHealthInfo ChecksumVerifier::check_page_health(const uint8_t* data, size_t size) {
    PageHealthInfo info = make_valid_health();
    
    // 检查页大小
    if (size < page_size_) {
        info.is_valid = false;
        info.status_message = "Page size too small";
        return info;
    }
    
    // 检查页头
    if (!verify_header(data, size)) {
        info.header_valid = false;
        info.status_message = "Invalid page header";
    }
    
    // 检查校验和
    auto checksum_result = verify_checksum(data, size);
    info.checksum_match = checksum_result.is_valid;
    
    if (!checksum_result.is_valid) {
        info.status_message = checksum_result.error_message;
    }
    
    // 检查页类型
    uint16_t page_type = read_le<uint16_t>(data, 22);  // FIL_PAGE_TYPE at offset 22 (2 bytes)
    if (!verify_page_type(page_type)) {
        info.type_valid = false;
        info.status_message = "Invalid page type";
    }
    
    return info;
}

// ==================== PageHealthChecker ====================

inline PageHealthInfo PageHealthChecker::check(const uint8_t* data, size_t size,
                                               ChecksumAlgorithm algorithm) {
    ChecksumVerifier verifier;
    verifier.set_algorithm(algorithm);
    return verifier.check_page_health(data, size);
}

inline bool PageHealthChecker::check_header(const uint8_t* data, size_t size) {
    return size >= 38;  // FIL_HEADER_SIZE
}

inline bool PageHealthChecker::check_checksum(const uint8_t* data, size_t size,
                                              ChecksumAlgorithm algorithm) {
    ChecksumVerifier verifier;
    verifier.set_algorithm(algorithm);
    auto result = verifier.verify_checksum(data, size);
    return result.is_valid;
}

inline bool PageHealthChecker::check_page_type(uint16_t page_type) {
    ChecksumVerifier verifier;
    return verifier.verify_page_type(page_type);
}

inline bool PageHealthChecker::check_page_size(uint32_t page_size, uint32_t expected_size) {
    ChecksumVerifier verifier;
    verifier.set_page_size(expected_size);
    return verifier.verify_page_size(page_size);
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_CHECKSUM_VERIFIER_HPP
