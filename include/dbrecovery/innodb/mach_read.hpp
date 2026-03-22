#pragma once

#include <cstdint>
#include <stdexcept>

namespace dbrecovery {
namespace innodb {

/**
 * @brief 安全读取 little-endian 整数（支持 uint24 和 sint24）
 */
template<typename T>
inline T read_le(const uint8_t* ptr, size_t offset = 0) noexcept(false) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "T must be an integral type with size <= 8");

    if (ptr == nullptr) {
        throw std::invalid_argument("Pointer is null in read_le");
    }
    
    const uint8_t* p = ptr + offset;

    if constexpr (sizeof(T) == 1) {
        return static_cast<T>(p[0]);
    } else if constexpr (sizeof(T) == 2) {
        uint16_t val = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
        return static_cast<T>(val);
    } else if constexpr (sizeof(T) == 3) { // ✅ 新增：支持 sint24 和 uint24
        uint32_t val = static_cast<uint32_t>(p[0]) | 
                      (static_cast<uint32_t>(p[1]) << 8) | 
                      (static_cast<uint32_t>(p[2]) << 16);
        
        if constexpr (std::is_signed_v<T>) {
            // ✅ 符号扩展：如果最高位为1，则扩展符号位
            if (val & 0x800000) {
                val |= 0xFF000000;
            }
            return static_cast<T>(static_cast<int32_t>(val));
        } else {
            return static_cast<T>(val & 0xFFFFFF);
        }
    } else if constexpr (sizeof(T) == 4) {
        uint32_t val = static_cast<uint32_t>(p[0]) | 
                      (static_cast<uint32_t>(p[1]) << 8) |
                      (static_cast<uint32_t>(p[2]) << 16) | 
                      (static_cast<uint32_t>(p[3]) << 24);
        return static_cast<T>(val);
    } else if constexpr (sizeof(T) == 8) {
        uint64_t val = static_cast<uint64_t>(p[0]) | 
                      (static_cast<uint64_t>(p[1]) << 8) |
                      (static_cast<uint64_t>(p[2]) << 16) | 
                      (static_cast<uint64_t>(p[3]) << 24) |
                      (static_cast<uint64_t>(p[4]) << 32) | 
                      (static_cast<uint64_t>(p[5]) << 40) |
                      (static_cast<uint64_t>(p[6]) << 48) | 
                      (static_cast<uint64_t>(p[7]) << 56);
        return static_cast<T>(val);
    } else {
        static_assert(sizeof(T) <= 8, "Unsupported integer size for read_le");
    }
}

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
        p[0] = static_cast<uint8_t>(value & 0xFF);
        p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    } else if constexpr (sizeof(T) == 3) { // ✅ 新增：支持 uint24
        p[0] = static_cast<uint8_t>(value & 0xFF);
        p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    } else if constexpr (sizeof(T) == 4) {
        p[0] = static_cast<uint8_t>(value & 0xFF);
        p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    } else if constexpr (sizeof(T) == 8) {
        p[0] = static_cast<uint8_t>(value & 0xFF);
        p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        p[4] = static_cast<uint8_t>((value >> 32) & 0xFF);
        p[5] = static_cast<uint8_t>((value >> 40) & 0xFF);
        p[6] = static_cast<uint8_t>((value >> 48) & 0xFF);
        p[7] = static_cast<uint8_t>((value >> 56) & 0xFF);
    } else {
        static_assert(sizeof(T) <= 8, "Unsupported integer size for write_le");
    }
}

// Big-endian helpers
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
        return (static_cast<T>(p[0]) << 8) | static_cast<T>(p[1]);
    } else if constexpr (sizeof(T) == 3) { // ✅ 新增：支持 uint24 big-endian
        return (static_cast<T>(p[0]) << 16) | (static_cast<T>(p[1]) << 8) | static_cast<T>(p[2]);
    } else if constexpr (sizeof(T) == 4) {
        return (static_cast<T>(p[0]) << 24) | (static_cast<T>(p[1]) << 16) |
               (static_cast<T>(p[2]) << 8) | static_cast<T>(p[3]);
    } else if constexpr (sizeof(T) == 8) {
        return (static_cast<T>(p[0]) << 56) | (static_cast<T>(p[1]) << 48) |
               (static_cast<T>(p[2]) << 40) | (static_cast<T>(p[3]) << 32) |
               (static_cast<T>(p[4]) << 24) | (static_cast<T>(p[5]) << 16) |
               (static_cast<T>(p[6]) << 8) | static_cast<T>(p[7]);
    } else {
        static_assert(sizeof(T) <= 8, "Unsupported integer size for read_be");
    }
}

} // namespace innodb
} // namespace dbrecovery
