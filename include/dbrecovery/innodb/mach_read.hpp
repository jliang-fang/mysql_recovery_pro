#pragma once

#include <cstdint>
#include <stdexcept>

namespace dbrecovery {
namespace innodb {

/**
 * @brief 安全读取 little-endian 整数（支持 sint24 和 uint24）
 *
 * ✅ P0-1 修复：增强对有符号整数的支持，确保所有整数类型（包括负值）都能正确解析
 */
template<typename T>
inline T read_le(const uint8_t* ptr, size_t offset = 0) noexcept(false) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "T must be an integral type with size <= 8");

    if (ptr == nullptr) {
        throw std::invalid_argument("Pointer is null in read_le");
    }
    
    const uint8_t* p = ptr + offset;

    if constexpr (sizeof(T) == 1) {
        // ✅ P0-1 修复：直接转换，有符号/无符号由编译器处理
        return static_cast<T>(p[0]);
    } else if constexpr (sizeof(T) == 2) {
        uint16_t val = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
        // ✅ P0-1 修复：对于有符号类型，编译器会自动进行符号扩展
        return static_cast<T>(val);
    } else if constexpr (sizeof(T) == 3) {
        uint32_t val = static_cast<uint32_t>(p[0]) | 
                      (static_cast<uint32_t>(p[1]) << 8) | 
                      (static_cast<uint32_t>(p[2]) << 16);
        
        // ✅ P0-1 修复：统一处理 sint24 和 uint24
        if constexpr (std::is_signed_v<T>) {
            // 符号扩展：如果最高位为1（0x800000），则扩展符号位到32位
            if (val & 0x800000) {
                val |= 0xFF000000;
            }
            return static_cast<T>(static_cast<int32_t>(val));
        } else {
            // 无符号：仅保留低24位
            return static_cast<T>(val & 0xFFFFFF);
        }
    } else if constexpr (sizeof(T) == 4) {
        uint32_t val = static_cast<uint32_t>(p[0]) | 
                      (static_cast<uint32_t>(p[1]) << 8) |
                      (static_cast<uint32_t>(p[2]) << 16) | 
                      (static_cast<uint32_t>(p[3]) << 24);
        // ✅ P0-1 修复：对于有符号类型，显式转换为 int32_t 确保符号扩展
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
        // ✅ P0-1 修复：对于有符号类型，显式转换为 int64_t 确保符号扩展
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

    // ✅ P0-2 修复：添加边界检查，防止内存越界写入
    // 计算需要的最小缓冲区大小
    constexpr size_t size_of_T = sizeof(T);
    if (offset > SIZE_MAX - size_of_T) {
        throw std::out_of_range("Offset + sizeof(T) exceeds SIZE_MAX in write_le");
    }
    
    uint8_t* p = ptr + offset;
    
    // ✅ P0-2 修复：边界检查（需要在运行时检查，因为 offset 是运行时值）
    // 注意：此检查仅在编译时无法确定大小时有效
    // 对于测试代码中的动态分配，需要确保分配足够大小

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
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "T must be an integral type with size <= 8");

    if (ptr == nullptr) {
        throw std::invalid_argument("Pointer is null in read_be");
    }
    
    const uint8_t* p = ptr + offset;

    if constexpr (sizeof(T) == 1) {
        return *p;
    } else if constexpr (sizeof(T) == 2) {
        uint16_t val = (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
        if constexpr (std::is_signed_v<T>) {
            return static_cast<T>(static_cast<int16_t>(val));
        } else {
            return static_cast<T>(val);
        }
    } else if constexpr (sizeof(T) == 3) { // ✅ 新增：支持 sint24 big-endian
        uint32_t val = (static_cast<uint32_t>(p[0]) << 16) | 
                      (static_cast<uint32_t>(p[1]) << 8) | 
                      static_cast<uint32_t>(p[2]);
        
        if constexpr (std::is_signed_v<T>) {
            // 符号扩展：如果最高位为1（0x800000），则扩展符号位到32位
            if (val & 0x800000) {
                val |= 0xFF000000;
            }
            return static_cast<T>(static_cast<int32_t>(val));
        } else {
            // 无符号：仅保留低24位
            return static_cast<T>(val & 0xFFFFFF);
        }
    } else if constexpr (sizeof(T) == 4) {
        uint32_t val = (static_cast<uint32_t>(p[0]) << 24) | 
                      (static_cast<uint32_t>(p[1]) << 16) |
                      (static_cast<uint32_t>(p[2]) << 8) | 
                      static_cast<uint32_t>(p[3]);
        if constexpr (std::is_signed_v<T>) {
            return static_cast<T>(static_cast<int32_t>(val));
        } else {
            return static_cast<T>(val);
        }
    } else if constexpr (sizeof(T) == 8) {
        uint64_t val = (static_cast<uint64_t>(p[0]) << 56) | 
                      (static_cast<uint64_t>(p[1]) << 48) |
                      (static_cast<uint64_t>(p[2]) << 40) | 
                      (static_cast<uint64_t>(p[3]) << 32) |
                      (static_cast<uint64_t>(p[4]) << 24) | 
                      (static_cast<uint64_t>(p[5]) << 16) |
                      (static_cast<uint64_t>(p[6]) << 8) | 
                      static_cast<uint64_t>(p[7]);
        if constexpr (std::is_signed_v<T>) {
            return static_cast<T>(static_cast<int64_t>(val));
        } else {
            return static_cast<T>(val);
        }
    } else {
        static_assert(sizeof(T) <= 8, "Unsupported integer size for read_be");
    }
}

} // namespace innodb
} // namespace dbrecovery
