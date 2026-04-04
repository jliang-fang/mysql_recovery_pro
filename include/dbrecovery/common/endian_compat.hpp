/**
 * @file include/dbrecovery/common/endian_compat.hpp
 * @brief 跨字节序支持 - 支持大端和小端系统
 * 
 * 本文件提供跨平台的字节序处理功能，
 * 确保在大端和小端系统上都能正确读取数据。
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace dbrecovery {
namespace endian {

// ==================== 字节序检测 ====================
#if defined(__BYTE_ORDER__)
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #define ENDIAN_IS_LITTLE 1
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define ENDIAN_IS_BIG 1
    #else
        #define ENDIAN_IS_UNKNOWN 1
    #endif
#elif defined(_WIN32)
    #define ENDIAN_IS_LITTLE 1
#elif defined(__BYTE_ORDER)
    #if __BYTE_ORDER == __LITTLE_ENDIAN
        #define ENDIAN_IS_LITTLE 1
    #elif __BYTE_ORDER == __BIG_ENDIAN
        #define ENDIAN_IS_BIG 1
    #else
        #define ENDIAN_IS_UNKNOWN 1
    #endif
#else
    // 运行时检测
    inline bool is_little_endian() {
        uint16_t test = 1;
        return *reinterpret_cast<uint8_t*>(&test) == 1;
    }
    #define ENDIAN_IS_LITTLE is_little_endian()
#endif

// ==================== 字节序转换函数 ====================

/**
 * @brief 16位整数字节序转换（小端到主机）
 */
inline uint16_t le16toh(uint16_t value) {
#ifdef ENDIAN_IS_BIG
    return ((value & 0xFF) << 8) | ((value >> 8) & 0xFF);
#else
    return value;
#endif
}

/**
 * @brief 32位整数字节序转换（小端到主机）
 */
inline uint32_t le32toh(uint32_t value) {
#ifdef ENDIAN_IS_BIG
    return ((value & 0xFF) << 24) |
           ((value & 0xFF00) << 8) |
           ((value >> 8) & 0xFF00) |
           ((value >> 24) & 0xFF);
#else
    return value;
#endif
}

/**
 * @brief 64位整数字节序转换（小端到主机）
 */
inline uint64_t le64toh(uint64_t value) {
#ifdef ENDIAN_IS_BIG
    return ((value & 0xFFULL) << 56) |
           ((value & 0xFF00ULL) << 40) |
           ((value & 0xFF0000ULL) << 24) |
           ((value & 0xFF000000ULL) << 8) |
           ((value >> 8) & 0xFF000000ULL) |
           ((value >> 24) & 0xFF0000ULL) |
           ((value >> 40) & 0xFF00ULL) |
           ((value >> 56) & 0xFFULL);
#else
    return value;
#endif
}

// ==================== 主机到小端转换辅助函数 ====================
// 这些函数用于写入操作，在小端系统上直接返回值

/**
 * @brief 16位整数字节序转换（主机到小端）- 内联版本
 */
inline uint16_t htole16_inline(uint16_t value) {
    return value;
}

/**
 * @brief 32位整数字节序转换（主机到小端）- 内联版本
 */
inline uint32_t htole32_inline(uint32_t value) {
    return value;
}

/**
 * @brief 64位整数字节序转换（主机到小端）- 内联版本
 */
inline uint64_t htole64_inline(uint64_t value) {
    return value;
}

// ==================== 带对齐的安全读取函数 ====================

/**
 * @brief 安全读取16位小端整数（支持未对齐访问）
 */
inline uint16_t read_le16(const uint8_t* data, size_t offset = 0) {
    uint16_t value;
    std::memcpy(&value, data + offset, sizeof(uint16_t));
    return le16toh(value);
}

/**
 * @brief 安全读取24位小端整数（支持未对齐访问）
 */
inline uint32_t read_le24(const uint8_t* data, size_t offset = 0) {
    uint32_t value = 0;
    value |= static_cast<uint32_t>(data[offset]) << 0;
    value |= static_cast<uint32_t>(data[offset + 1]) << 8;
    value |= static_cast<uint32_t>(data[offset + 2]) << 16;
    return value;
}

/**
 * @brief 安全读取32位小端整数（支持未对齐访问）
 */
inline uint32_t read_le32(const uint8_t* data, size_t offset = 0) {
    uint32_t value;
    std::memcpy(&value, data + offset, sizeof(uint32_t));
    return le32toh(value);
}

/**
 * @brief 安全读取48位小端整数（支持未对齐访问）
 */
inline uint64_t read_le48(const uint8_t* data, size_t offset = 0) {
    uint64_t value = 0;
    value |= static_cast<uint64_t>(data[offset]) << 0;
    value |= static_cast<uint64_t>(data[offset + 1]) << 8;
    value |= static_cast<uint64_t>(data[offset + 2]) << 16;
    value |= static_cast<uint64_t>(data[offset + 3]) << 24;
    value |= static_cast<uint64_t>(data[offset + 4]) << 32;
    value |= static_cast<uint64_t>(data[offset + 5]) << 40;
    return value;
}

/**
 * @brief 安全读取64位小端整数（支持未对齐访问）
 */
inline uint64_t read_le64(const uint8_t* data, size_t offset = 0) {
    uint64_t value;
    std::memcpy(&value, data + offset, sizeof(uint64_t));
    return le64toh(value);
}

/**
 * @brief 安全读取16位大端整数（支持未对齐访问）
 */
inline uint16_t read_be16(const uint8_t* data, size_t offset = 0) {
    uint16_t value = 0;
    value |= static_cast<uint16_t>(data[offset]) << 8;
    value |= static_cast<uint16_t>(data[offset + 1]) << 0;
    return value;
}

/**
 * @brief 安全读取32位大端整数（支持未对齐访问）
 */
inline uint32_t read_be32(const uint8_t* data, size_t offset = 0) {
    uint32_t value = 0;
    value |= static_cast<uint32_t>(data[offset]) << 24;
    value |= static_cast<uint32_t>(data[offset + 1]) << 16;
    value |= static_cast<uint32_t>(data[offset + 2]) << 8;
    value |= static_cast<uint32_t>(data[offset + 3]) << 0;
    return value;
}

/**
 * @brief 安全读取64位大端整数（支持未对齐访问）
 */
inline uint64_t read_be64(const uint8_t* data, size_t offset = 0) {
    uint64_t value = 0;
    value |= static_cast<uint64_t>(data[offset]) << 56;
    value |= static_cast<uint64_t>(data[offset + 1]) << 48;
    value |= static_cast<uint64_t>(data[offset + 2]) << 40;
    value |= static_cast<uint64_t>(data[offset + 3]) << 32;
    value |= static_cast<uint64_t>(data[offset + 4]) << 24;
    value |= static_cast<uint64_t>(data[offset + 5]) << 16;
    value |= static_cast<uint64_t>(data[offset + 6]) << 8;
    value |= static_cast<uint64_t>(data[offset + 7]) << 0;
    return value;
}

// ==================== 写入函数 ====================

/**
 * @brief 安全写入16位小端整数（支持未对齐访问）
 */
inline void write_le16(uint8_t* data, uint16_t value, size_t offset = 0) {
    value = htole16_inline(value);
    std::memcpy(data + offset, &value, sizeof(uint16_t));
}

/**
 * @brief 安全写入24位小端整数（支持未对齐访问）
 */
inline void write_le24(uint8_t* data, uint32_t value, size_t offset = 0) {
    data[offset + 0] = static_cast<uint8_t>(value >> 0);
    data[offset + 1] = static_cast<uint8_t>(value >> 8);
    data[offset + 2] = static_cast<uint8_t>(value >> 16);
}

/**
 * @brief 安全写入32位小端整数（支持未对齐访问）
 */
inline void write_le32(uint8_t* data, uint32_t value, size_t offset = 0) {
    value = htole32_inline(value);
    std::memcpy(data + offset, &value, sizeof(uint32_t));
}

/**
 * @brief 安全写入64位小端整数（支持未对齐访问）
 */
inline void write_le64(uint8_t* data, uint64_t value, size_t offset = 0) {
    value = htole64_inline(value);
    std::memcpy(data + offset, &value, sizeof(uint64_t));
}

// ==================== 浮点数转换 ====================

/**
 * @brief 安全读取32位小端浮点数（支持未对齐访问）
 */
inline float read_le_float(const uint8_t* data, size_t offset = 0) {
    uint32_t value;
    std::memcpy(&value, data + offset, sizeof(float));
    value = le32toh(value);
    float result;
    std::memcpy(&result, &value, sizeof(float));
    return result;
}

/**
 * @brief 安全读取64位小端浮点数（支持未对齐访问）
 */
inline double read_le_double(const uint8_t* data, size_t offset = 0) {
    uint64_t value;
    std::memcpy(&value, data + offset, sizeof(double));
    value = le64toh(value);
    double result;
    std::memcpy(&result, &value, sizeof(double));
    return result;
}

// ==================== CRC32校验和 ====================

/**
 * @brief 计算CRC32校验和（支持大端/小端）
 */
inline uint32_t crc32(const uint8_t* data, size_t length, uint32_t initial = 0) {
    static const uint32_t crc_table[256] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
        0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
        0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
        0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
        0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
        0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
        0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
        0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
        0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
        0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
        0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
        0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
        0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
        0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
        0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
        0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
        0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
        0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
        0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA,
        0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
        0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
        0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
        0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
        0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
        0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
        0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
        0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
        0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
        0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
        0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
        0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
        0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
        0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
        0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
        0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
        0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
        0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
        0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
        0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
        0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
        0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693,
        0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
        0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
    };
    
    uint32_t crc = initial ^ 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = (crc >> 8) ^ crc_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

// ==================== 字节序检测类 ====================
/**
 * @brief 字节序检测类
 */
class EndianDetector {
public:
    static bool is_little_endian() {
        #ifdef ENDIAN_IS_LITTLE
            return true;
        #elif defined(ENDIAN_IS_BIG)
            return false;
        #else
            static bool result = is_little_endian_runtime();
            return result;
        #endif
    }

    static bool is_big_endian() {
        return !is_little_endian();
    }

private:
    static bool is_little_endian_runtime() {
        uint16_t test = 1;
        return *reinterpret_cast<uint8_t*>(&test) == 1;
    }
};

// ==================== 模板辅助函数 ====================

/**
 * @brief 读取任意大小的小端整数
 */
template<typename T>
inline T read_le(const uint8_t* data, size_t offset = 0) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "T must be an integral type");
    
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    
    if constexpr (sizeof(T) == 1) {
        return value;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(le16toh(static_cast<uint16_t>(value)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(le32toh(static_cast<uint32_t>(value)));
    } else if constexpr (sizeof(T) == 8) {
        return static_cast<T>(le64toh(static_cast<uint64_t>(value)));
    } else {
        return value;
    }
}

/**
 * @brief 写入任意大小的小端整数
 */
template<typename T>
inline void write_le(uint8_t* data, T value, size_t offset = 0) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "T must be an integral type");

    if constexpr (sizeof(T) == 1) {
        data[offset] = static_cast<uint8_t>(value);
    } else if constexpr (sizeof(T) == 2) {
        uint16_t v = htole16_inline(static_cast<uint16_t>(value));
        std::memcpy(data + offset, &v, sizeof(uint16_t));
    } else if constexpr (sizeof(T) == 4) {
        uint32_t v = htole32_inline(static_cast<uint32_t>(value));
        std::memcpy(data + offset, &v, sizeof(uint32_t));
    } else if constexpr (sizeof(T) == 8) {
        uint64_t v = htole64_inline(static_cast<uint64_t>(value));
        std::memcpy(data + offset, &v, sizeof(uint64_t));
    } else {
        std::memcpy(data + offset, &value, sizeof(T));
    }
}

} // namespace endian
} // namespace dbrecovery
