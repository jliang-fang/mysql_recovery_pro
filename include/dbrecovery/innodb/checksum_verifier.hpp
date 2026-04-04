#ifndef DBRECOVERY_INNODB_CHECKSUM_VERIFIER_HPP
#define DBRECOVERY_INNODB_CHECKSUM_VERIFIER_HPP

#include "dbrecovery/innodb/page_types_v2.hpp"
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
    // 使用硬件加速的 CRC32C (如果可用)
    uint32_t crc = init_crc;
    
    // 简化实现：使用标准 CRC32C 多项式 0x1EDC6F41
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
    // 读取存储的校验和 (在页末尾)
    if (size < 60) {
        return 0;
    }
    
    // CRC32: 在 FIL_PAGE_END_LSN (偏移 8)
    // 其他算法: 在页末尾
    return read_le<uint32_t>(data, size - 4);
}

inline ChecksumResult ChecksumVerifier::verify_checksum(const uint8_t* data, size_t size) {
    stats_.pages_verified++;
    
    uint32_t stored = get_stored_checksum(data, size);
    uint32_t calculated = calculate_checksum(data, size);
    
    if (stored == calculated) {
        stats_.checksums_valid++;
        return make_valid_result(calculated, stored, algorithm_);
    }
    
    stats_.checksums_invalid++;
    return make_invalid_result(calculated, stored, algorithm_, "Checksum mismatch");
}

inline bool ChecksumVerifier::verify_header(const uint8_t* data, size_t size) {
    if (size < 38) {  // FIL_HEADER_SIZE
        stats_.headers_invalid++;
        return false;
    }
    
    stats_.headers_valid++;
    return true;
}

inline bool ChecksumVerifier::verify_page_type(uint16_t page_type) {
    // 检查页类型是否有效
    switch (page_type) {
        case 0x0000:  // ALLOCATED
        case 0x45bf:  // INDEX
        case 0x45bb:  // UNDO_LOG
        case 0x45bd:  // INODE
        case 0x45be:  // IBUF_FREE_LIST
        case 0x45b8:  // TRX_SYS
        case 0x45b9:  // FSP_HDR
        case 0x45ba:  // XDES
        case 0x45b7:  // BLOB
        case 0x5441:  // SDI
        case 0x5a5a:  // SDI2
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
    uint16_t page_type = read_le<uint16_t>(data, 16);
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
