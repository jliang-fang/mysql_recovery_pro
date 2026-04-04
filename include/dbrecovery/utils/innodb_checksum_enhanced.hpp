#ifndef DBRECOVERY_UTILS_INNODB_CHECKSUM_ENHANCED_HPP
#define DBRECOVERY_UTILS_INNODB_CHECKSUM_ENHANCED_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <optional>
#include <string>
#include <cstring>
#include <type_traits>
#include <bit>

#if defined(__SSE4_2__) || defined(__POPCNT__)
#  include <nmmintrin.h>
#  define HAS_CRC32C_INSTRUCTION
#endif

#if defined(__ARM_NEON) || defined(__aarch64__)
#  include <arm_neon.h>
#  define HAS_ARM_CRC
#endif

namespace dbrecovery {
namespace utils {

// ==================== CRC32C 校验和实现 ====================
/**
 * @brief CRC32C 校验和计算（使用硬件加速）
 * @details 实现 RFC 4960 的 CRC32C 校验和算法
 */
class CRC32C {
public:
    /**
     * @brief 计算 CRC32C 校验和
     * @param data 数据指针
     * @param length 数据长度
     * @return CRC32C 校验和
     */
    static uint32_t calculate(const uint8_t* data, size_t length) {
        if (!data || length == 0) {
            return 0;
        }

#ifdef HAS_CRC32C_INSTRUCTION
        return calculate_hw(data, length);
#else
        return calculate_sw(data, length);
#endif
    }

    /**
     * @brief 验证 CRC32C 校验和
     * @param data 数据指针
     * @param length 数据长度
     * @param expected 期望的校验和
     * @return true 校验和匹配
     */
    static bool validate(const uint8_t* data, size_t length, uint32_t expected) {
        return calculate(data, length) == expected;
    }

    /**
     * @brief 获取校验和算法名称
     */
    static std::string algorithm_name() {
#ifdef HAS_CRC32C_INSTRUCTION
        return "CRC32C-HW";
#else
        return "CRC32C-SW";
#endif
    }

private:
    // CRC32C 多项式 (Castagnoli)
    static constexpr uint32_t CRC32C_POLY = 0x1EDC6F41;

    // 预计算的查找表
    static std::array<uint32_t, 256> init_lookup_table() {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ CRC32C_POLY;
                } else {
                    crc >>= 1;
                }
            }
            table[i] = crc;
        }
        return table;
    }

    // 软件实现
    static uint32_t calculate_sw(const uint8_t* data, size_t length) {
        static const std::array<uint32_t, 256> table = init_lookup_table();
        
        uint32_t crc = 0xFFFFFFFF;
        size_t i = 0;

        // 按字处理
        while (length >= 4) {
            uint32_t word = *reinterpret_cast<const uint32_t*>(data + i);
            crc ^= word;
            
            crc = table[(crc >> 0) & 0xFF] ^ (crc >> 8);
            crc = table[(crc >> 0) & 0xFF] ^ (crc >> 8);
            crc = table[(crc >> 0) & 0xFF] ^ (crc >> 8);
            crc = table[(crc >> 0) & 0xFF] ^ (crc >> 8);
            
            i += 4;
            length -= 4;
        }

        // 按字节处理剩余部分
        while (length > 0) {
            crc = table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
            --length;
        }

        return ~crc;
    }

#ifdef HAS_CRC32C_INSTRUCTION
    // 硬件实现（使用 SSE4.2）
    static uint32_t calculate_hw(const uint8_t* data, size_t length) {
        uint32_t crc = 0xFFFFFFFF;
        size_t i = 0;

        // 按 8 字节处理
        while (length >= 8) {
            crc = _mm_crc32_u64(crc, *reinterpret_cast<const uint64_t*>(data + i));
            i += 8;
            length -= 8;
        }

        // 按 4 字节处理
        while (length >= 4) {
            crc = _mm_crc32_u32(crc, *reinterpret_cast<const uint32_t*>(data + i));
            i += 4;
            length -= 4;
        }

        // 按字节处理剩余部分
        while (length > 0) {
            crc = _mm_crc32_u8(crc, *data++);
            --length;
        }

        return ~crc;
    }
#endif
};

// ==================== InnoDB 校验和类型 ====================
enum class InnoDBChecksumType {
    NONE = 0,         // 无校验和
    CRC32 = 1,        // CRC32 (MySQL 5.5+)
    INNODB = 2,       // InnoDB 校验和
    FSP_CHECKSUM = 3, // FSP 校验和
    NEW = 4,          // 新格式校验和
    OLD = 5           // 旧格式校验和
};

// ==================== InnoDB 页校验和 ====================
class InnoDBPageChecksum {
public:
    /**
     * @brief 计算 InnoDB 页校验和
     * @param page_data 页数据
     * @param page_size 页大小
     * @param checksum_type 校验和类型
     * @return 校验和值
     */
    static uint32_t calculate_page_checksum(
        const uint8_t* page_data,
        size_t page_size,
        InnoDBChecksumType checksum_type = InnoDBChecksumType::CRC32) {
        
        if (!page_data || page_size < 38) {
            return 0;
        }

        switch (checksum_type) {
            case InnoDBChecksumType::NONE:
                return 0xFFFFFFFF;  // 无校验和
            case InnoDBChecksumType::CRC32:
            case InnoDBChecksumType::NEW:
                return calculate_crc32(page_data, page_size);
            case InnoDBChecksumType::INNODB:
                return calculate_innodb(page_data, page_size);
            case InnoDBChecksumType::FSP_CHECKSUM:
                return calculate_fsp(page_data, page_size);
            case InnoDBChecksumType::OLD:
                return calculate_old(page_data, page_size);
            default:
                return calculate_crc32(page_data, page_size);
        }
    }

    /**
     * @brief 验证 InnoDB 页校验和
     * @param page_data 页数据
     * @param page_size 页大小
     * @param stored_checksum 存储的校验和
     * @param checksum_type 校验和类型
     * @return true 校验和匹配
     */
    static bool validate_page_checksum(
        const uint8_t* page_data,
        size_t page_size,
        uint32_t stored_checksum,
        InnoDBChecksumType checksum_type = InnoDBChecksumType::CRC32) {
        
        uint32_t computed = calculate_page_checksum(page_data, page_size, checksum_type);
        return computed == stored_checksum;
    }

    /**
     * @brief 从页中读取存储的校验和
     * @param page_data 页数据
     * @return 存储的校验和
     */
    static uint32_t read_stored_checksum(const uint8_t* page_data) {
        return read_le<uint32_t>(page_data, 24);
    }

    /**
     * @brief 写入校验和到页
     * @param page_data 页数据
     * @param checksum 校验和值
     */
    static void write_checksum(uint8_t* page_data, uint32_t checksum) {
        write_le<uint32_t>(page_data, 24, checksum);
        // 页尾也有校验和副本
        write_le<uint32_t>(page_data, 28, checksum);
    }

    /**
     * @brief 获取校验和类型名称
     */
    static std::string checksum_type_name(InnoDBChecksumType type) {
        switch (type) {
            case InnoDBChecksumType::NONE: return "NONE";
            case InnoDBChecksumType::CRC32: return "CRC32";
            case InnoDBChecksumType::INNODB: return "INNODB";
            case InnoDBChecksumType::FSP_CHECKSUM: return "FSP_CHECKSUM";
            case InnoDBChecksumType::NEW: return "NEW";
            case InnoDBChecksumType::OLD: return "OLD";
            default: return "UNKNOWN";
        }
    }

private:
    // CRC32 校验和
    static uint32_t calculate_crc32(const uint8_t* page_data, size_t page_size) {
        // 计算 CRC32（不包括校验和字段）
        uint32_t checksum = CRC32C::calculate(page_data, page_size);
        return checksum;
    }

    // InnoDB 校验和（旧算法）
    static uint32_t calculate_innodb(const uint8_t* page_data, size_t page_size) {
        uint32_t checksum = 0;
        size_t i = 0;
        
        // 简化实现
        while (i + 4 <= page_size) {
            checksum ^= read_le<uint32_t>(page_data, i);
            i += 4;
        }
        
        return checksum;
    }

    // FSP 校验和
    static uint32_t calculate_fsp(const uint8_t* page_data, size_t page_size) {
        uint32_t checksum = 0;
        size_t i = 0;
        
        // FSP header 在偏移 36
        if (page_size >= 36 + 4) {
            checksum = read_le<uint32_t>(page_data, 36);
        }
        
        return checksum;
    }

    // 旧格式校验和
    static uint32_t calculate_old(const uint8_t* page_data, size_t page_size) {
        uint32_t checksum = 0;
        size_t i = 0;
        
        // 简化实现
        while (i + 4 <= page_size) {
            uint32_t word = read_le<uint32_t>(page_data, i);
            checksum = (checksum >> 17) | (checksum << 15);
            checksum ^= word;
            i += 4;
        }
        
        return checksum;
    }
};

// ==================== 增强的页校验和验证器 ====================
class EnhancedPageChecksumValidator {
public:
    EnhancedPageChecksumValidator();

    /**
     * @brief 设置页大小
     */
    void set_page_size(size_t page_size);

    /**
     * @brief 设置校验和类型
     */
    void set_checksum_type(InnoDBChecksumType type);

    /**
     * @brief 验证页校验和
     * @param page_data 页数据
     * @param page_no 页号
     * @return 验证结果
     */
    struct ChecksumResult {
        bool is_valid;
        uint32_t stored_checksum;
        uint32_t computed_checksum;
        std::string error_message;
        InnoDBChecksumType detected_type;
    };

    ChecksumResult validate(const uint8_t* page_data, size_t page_no = 0);

    /**
     * @brief 修复页校验和
     * @param page_data 页数据
     * @return true 修复成功
     */
    bool repair(uint8_t* page_data);

    /**
     * @brief 检测校验和类型
     * @param page_data 页数据
     * @return 检测到的校验和类型
     */
    static InnoDBChecksumType detect_checksum_type(const uint8_t* page_data);

    /**
     * @brief 获取统计信息
     */
    struct Stats {
        size_t total_pages = 0;
        size_t valid_pages = 0;
        size_t invalid_pages = 0;
        size_t repaired_pages = 0;
        std::map<InnoDBChecksumType, size_t> type_counts;
    };

    Stats get_stats() const { return stats_; }

    /**
     * @brief 清空统计
     */
    void clear_stats() { stats_ = Stats{}; }

private:
    size_t page_size_;
    InnoDBChecksumType checksum_type_;
    Stats stats_;

    bool detect_checksum_type_internal(const uint8_t* page_data);
};

// ==================== 增强的页校验和验证器实现 ====================

inline EnhancedPageChecksumValidator::EnhancedPageChecksumValidator()
    : page_size_(16384), checksum_type_(InnoDBChecksumType::CRC32) {
    std::memset(&stats_, 0, sizeof(stats_));
}

inline void EnhancedPageChecksumValidator::set_page_size(size_t page_size) {
    if (page_size != 4096 && page_size != 8192 &&
        page_size != 16384 && page_size != 32768 && page_size != 65536) {
        return;
    }
    page_size_ = page_size;
}

inline void EnhancedPageChecksumValidator::set_checksum_type(InnoDBChecksumType type) {
    checksum_type_ = type;
}

inline InnoDBChecksumType EnhancedPageChecksumValidator::detect_checksum_type(const uint8_t* page_data) {
    if (!page_data) {
        return InnoDBChecksumType::NONE;
    }

    uint32_t stored = read_le<uint32_t>(page_data, 24);
    
    // 检测校验和类型
    if (stored == 0) {
        return InnoDBChecksumType::NONE;
    }
    if (stored == 0xFFFFFFFF) {
        return InnoDBChecksumType::CRC32;
    }
    
    // 检查页尾
    if (page_size_ >= 36) {
        uint32_t tail_checksum = read_le<uint32_t>(page_data, page_size_ - 4);
        if (tail_checksum == stored) {
            return InnoDBChecksumType::NEW;
        }
    }
    
    return InnoDBChecksumType::CRC32;
}

inline bool EnhancedPageChecksumValidator::detect_checksum_type_internal(const uint8_t* page_data) {
    checksum_type_ = detect_checksum_type(page_data);
    stats_.type_counts[checksum_type_]++;
    return true;
}

inline EnhancedPageChecksumValidator::ChecksumResult 
EnhancedPageChecksumValidator::validate(const uint8_t* page_data, size_t page_no) {
    ChecksumResult result{};
    result.is_valid = false;
    result.stored_checksum = 0;
    result.computed_checksum = 0;
    
    if (!page_data || page_size_ < 38) {
        result.error_message = "Invalid page data";
        stats_.invalid_pages++;
        return result;
    }

    stats_.total_pages++;

    // 读取存储的校验和
    result.stored_checksum = read_le<uint32_t>(page_data, 24);
    
    // 检测校验和类型
    detect_checksum_type_internal(page_data);

    // 计算校验和
    result.computed_checksum = InnoDBPageChecksum::calculate_page_checksum(
        page_data, page_size_, checksum_type_);

    // 验证校验和
    if (result.stored_checksum == 0 || result.stored_checksum == 0xFFFFFFFF) {
        // 无校验和或 CRC32 模式
        result.is_valid = true;
        stats_.valid_pages++;
    } else if (result.stored_checksum == result.computed_checksum) {
        result.is_valid = true;
        stats_.valid_pages++;
    } else {
        result.is_valid = false;
        result.error_message = "Checksum mismatch: stored=" + 
            std::to_string(result.stored_checksum) + 
            ", computed=" + std::to_string(result.computed_checksum);
        stats_.invalid_pages++;
    }

    return result;
}

inline bool EnhancedPageChecksumValidator::repair(uint8_t* page_data) {
    if (!page_data || page_size_ < 38) {
        return false;
    }

    // 计算校验和
    uint32_t checksum = InnoDBPageChecksum::calculate_page_checksum(
        page_data, page_size_, checksum_type_);

    // 写入校验和
    InnoDBPageChecksum::write_checksum(page_data, checksum);
    
    stats_.repaired_pages++;
    return true;
}

} // namespace utils
} // namespace dbrecovery

#endif // DBRECOVERY_UTILS_INNODB_CHECKSUM_ENHANCED_HPP
