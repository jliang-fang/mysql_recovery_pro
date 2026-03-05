#pragma once
#include <cstdint>
#include <type_traits>

namespace dbrecovery::innodb {

/**
 * @brief 安全读取 little-endian 整数（支持 uint24）
 */
template<typename T>
inline T read_le(const uint8_t* ptr) noexcept {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    if constexpr (sizeof(T) == 1) return *ptr;
    else if constexpr (sizeof(T) == 2) {
        #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return *reinterpret_cast<const uint16_t*>(ptr);
        #else
            return static_cast<T>(ptr[0]) | (static_cast<T>(ptr[1]) << 8);
        #endif
    }
    else if constexpr (sizeof(T) == 3) {
        // uint24: little-endian → [b0, b1, b2]
        return static_cast<T>(ptr[0]) |
               (static_cast<T>(ptr[1]) << 8) |
               (static_cast<T>(ptr[2]) << 16);
    }
    else if constexpr (sizeof(T) == 4) {
        #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return *reinterpret_cast<const uint32_t*>(ptr);
        #else
            T val = static_cast<T>(ptr[0]);
            for (size_t i = 1; i < 4; ++i) val |= static_cast<T>(ptr[i]) << (8 * i);
            return val;
        #endif
    }
    else if constexpr (sizeof(T) == 8) {
        #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return *reinterpret_cast<const uint64_t*>(ptr);
        #else
            T val = static_cast<T>(ptr[0]);
            for (size_t i = 1; i < 8; ++i) val |= static_cast<T>(ptr[i]) << (8 * i);
            return val;
        #endif
    }
}

// ✅ 正确 uint24 小端读取（InnoDB 标准）
template<>
inline uint32_t read_le<uint32_t>(const uint8_t* ptr) noexcept {
    return static_cast<uint32_t>(
        (static_cast<uint32_t>(ptr[0])) |
        (static_cast<uint32_t>(ptr[1]) << 8) |
        (static_cast<uint32_t>(ptr[2]) << 16)
    );
}

// ✅ 大端平台兼容：仅当主机为大端时启用 bswap
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
template<typename T>
inline T read_le(const uint8_t* ptr) noexcept {
    // 小端数据 → 拼接后交换字节
    T val = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        val |= static_cast<T>(ptr[i]) << (8 * i);
    return __builtin_bswap##sizeof(T)*8(val);
}
#endif

/**
 * @brief 安全读取 big-endian 整数（支持 uint24）
 */
template<typename T>
inline T read_be(const uint8_t* ptr) noexcept {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    if constexpr (sizeof(T) == 1) return *ptr;
    else if constexpr (sizeof(T) == 2) {
        #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return __builtin_bswap16(*reinterpret_cast<const uint16_t*>(ptr));
        #else
            return static_cast<T>(ptr[0]) << 8 | static_cast<T>(ptr[1]);
        #endif
    }
    else if constexpr (sizeof(T) == 3) {
        // uint24: big-endian → [b0, b1, b2]
        return static_cast<T>(ptr[0]) << 16 |
               static_cast<T>(ptr[1]) << 8  |
               static_cast<T>(ptr[2]);
    }
    else if constexpr (sizeof(T) == 4) {
        #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(ptr));
        #else
            T val = static_cast<T>(ptr[0]) << 24;
            for (size_t i = 1; i < 4; ++i) val |= static_cast<T>(ptr[i]) << (24 - 8 * i);
            return val;
        #endif
    }
    else if constexpr (sizeof(T) == 8) {
        #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return __builtin_bswap64(*reinterpret_cast<const uint64_t*>(ptr));
        #else
            T val = static_cast<T>(ptr[0]) << 56;
            for (size_t i = 1; i < 8; ++i) val |= static_cast<T>(ptr[i]) << (56 - 8 * i);
            return val;
        #endif
    }
}

/**
 * @brief 安全写入 little-endian 整数（支持 uint24）
 */
template<typename T>
inline void write_le(uint8_t* ptr, T value) noexcept {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    if constexpr (sizeof(T) == 1) *ptr = static_cast<uint8_t>(value);
    else if constexpr (sizeof(T) == 2) {
        #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            *reinterpret_cast<uint16_t*>(ptr) = value;
        #else
            ptr[0] = static_cast<uint8_t>(value);
            ptr[1] = static_cast<uint8_t>(value >> 8);
        #endif
    }
    else if constexpr (sizeof(T) == 3) {
        ptr[0] = static_cast<uint8_t>(value);
        ptr[1] = static_cast<uint8_t>(value >> 8);
        ptr[2] = static_cast<uint8_t>(value >> 16);
    }
    else if constexpr (sizeof(T) == 4) {
        #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            *reinterpret_cast<uint32_t*>(ptr) = value;
        #else
            for (size_t i = 0; i < 4; ++i)
                ptr[i] = static_cast<uint8_t>(value >> (8 * i));
        #endif
    }
}

} // namespace dbrecovery::innodb
