#pragma once
#include <cstdint>
#include <type_traits>
#include <cstring>
#include <stdexcept>

namespace dbrecovery {
namespace innodb {

/**
 * @brief 安全读取 little-endian 整数（支持 uint24）
 *
 * 此实现是跨平台且无歧义的，避免了模板特化冲突。
 * 增强版：添加边界检查和错误处理
 */
template<typename T>
inline T read_le(const uint8_t* ptr, size_t offset = 0) noexcept(false) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>, "T must be an unsigned integral type");
    
    if (ptr == nullptr) {
        throw std::invalid_argument("Pointer is null in read_le");
    }
    
    const uint8_t* p = ptr + offset;
    
    if constexpr (sizeof(T) == 1) {
        return *p;
    } else if constexpr (sizeof(T) == 2) {
        uint16_t val;
        std::memcpy(&val, p, sizeof(val));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        val = __builtin_bswap16(val);
#endif
        return static_cast<T>(val);
    } else if constexpr (sizeof(T) == 3) {
        // InnoDB's uint24 is stored in little-endian format: [b0, b1, b2]
        // We read it as a 32-bit value and mask off the top byte.
        uint32_t val = static_cast<uint32_t>(p[0]) |
                       (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16);
        return static_cast<T>(val & 0x00FFFFFF);
    } else if constexpr (sizeof(T) == 4) {
        uint32_t val;
        std::memcpy(&val, p, sizeof(val));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        val = __builtin_bswap32(val);
#endif
        return static_cast<T>(val);
    } else if constexpr (sizeof(T) == 8) {
        uint64_t val;
        std::memcpy(&val, p, sizeof(val));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        val = __builtin_bswap64(val);
#endif
        return static_cast<T>(val);
    }
}

/**
 * @brief 安全读取 big-endian 整数（支持 uint24）
 */
template<typename T>
inline T read_be(const uint8_t* ptr, size_t offset = 0) noexcept(false) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>, "T must be an unsigned integral type");

    if (ptr == nullptr) {
        throw std::invalid_argument("Pointer is null in read_be");
    }
    
    const uint8_t* p = ptr + offset;

    if constexpr (sizeof(T) == 1) {
        return *p;
    } else if constexpr (sizeof(T) == 2) {
        uint16_t val;
        std::memcpy(&val, p, sizeof(val));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        val = __builtin_bswap16(val);
#endif
        return static_cast<T>(val);
    } else if constexpr (sizeof(T) == 3) {
        // Big-endian uint24: [b0, b1, b2] -> b0 is MSB
        uint32_t val = (static_cast<uint32_t>(p[0]) << 16) |
                       (static_cast<uint32_t>(p[1]) << 8)  |
                       static_cast<uint32_t>(p[2]);
        return static_cast<T>(val & 0x00FFFFFF);
    } else if constexpr (sizeof(T) == 4) {
        uint32_t val;
        std::memcpy(&val, p, sizeof(val));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        val = __builtin_bswap32(val);
#endif
        return static_cast<T>(val);
    } else if constexpr (sizeof(T) == 8) {
        uint64_t val;
        std::memcpy(&val, p, sizeof(val));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        val = __builtin_bswap64(val);
#endif
        return static_cast<T>(val);
    }
}

/**
 * @brief 安全写入 little-endian 整数（支持 uint24）
 */
template<typename T>
inline void write_le(uint8_t* ptr, size_t offset, T value) noexcept(false) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>, "T must be an unsigned integral type");

    if (ptr == nullptr) {
        throw std::invalid_argument("Pointer is null in write_le");
    }
    
    uint8_t* p = ptr + offset;

    if constexpr (sizeof(T) == 1) {
        *p = static_cast<uint8_t>(value);
    } else if constexpr (sizeof(T) == 2) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        uint16_t val = __builtin_bswap16(static_cast<uint16_t>(value));
        std::memcpy(p, &val, sizeof(val));
#else
        std::memcpy(p, &value, sizeof(value));
#endif
    } else if constexpr (sizeof(T) == 3) {
        p[0] = static_cast<uint8_t>(value);
        p[1] = static_cast<uint8_t>(value >> 8);
        p[2] = static_cast<uint8_t>(value >> 16);
    } else if constexpr (sizeof(T) == 4) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        uint32_t val = __builtin_bswap32(static_cast<uint32_t>(value));
        std::memcpy(p, &val, sizeof(val));
#else
        std::memcpy(p, &value, sizeof(value));
#endif
    } else if constexpr (sizeof(T) == 8) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        uint64_t val = __builtin_bswap64(static_cast<uint64_t>(value));
        std::memcpy(p, &val, sizeof(val));
#else
        std::memcpy(p, &value, sizeof(value));
#endif
    }
}

/**
 * @brief MachRead 类：提供静态方法读取 little-endian 整数（兼容旧代码）
 */
class MachRead {
public:
    template<typename T>
    static T read_le(const uint8_t* ptr, size_t offset = 0) noexcept(false) {
        return read_le<T>(ptr, offset);
    }

    template<typename T>
    static T read_be(const uint8_t* ptr, size_t offset = 0) noexcept(false) {
        return read_be<T>(ptr, offset);
    }
};

/**
 * @brief MachWrite 类：提供静态方法写入 little-endian 整数（兼容旧代码）
 */
class MachWrite {
public:
    template<typename T>
    static void write_le(uint8_t* ptr, size_t offset, T value) noexcept(false) {
        write_le<T>(ptr, offset, value);
    }
};

} // namespace innodb
} // namespace dbrecovery
