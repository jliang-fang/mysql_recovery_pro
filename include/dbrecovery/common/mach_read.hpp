#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace dbrecovery {
namespace mach {

/**
 * @brief 从字节流中读取 little-endian 格式的无符号整数（1 字节）
 */
template<typename T>
inline typename std::enable_if<std::is_same<T, uint8_t>::value, T>::type
read_uint1(const uint8_t *ptr) {
    return *ptr;
}

/**
 * @brief 从字节流中读取 little-endian 格式的无符号整数（2 字节）
 */
template<typename T>
inline typename std::enable_if<std::is_same<T, uint16_t>::value, T>::type
read_uint2(const uint8_t *ptr) {
    return static_cast<uint16_t>(static_cast<uint16_t>(ptr[0]) |
                                 (static_cast<uint16_t>(ptr[1]) << 8));
}

/**
 * @brief 从字节流中读取 little-endian 格式的无符号整数（4 字节）
 */
template<typename T>
inline typename std::enable_if<std::is_same<T, uint32_t>::value, T>::type
read_uint4(const uint8_t *ptr) {
    return static_cast<uint32_t>(static_cast<uint32_t>(ptr[0]) |
                                 (static_cast<uint32_t>(ptr[1]) << 8) |
                                 (static_cast<uint32_t>(ptr[2]) << 16) |
                                 (static_cast<uint32_t>(ptr[3]) << 24));
}

/**
 * @brief 从字节流中读取 little-endian 格式的无符号整数（8 字节）
 */
template<typename T>
inline typename std::enable_if<std::is_same<T, uint64_t>::value, T>::type
read_uint8(const uint8_t *ptr) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val |= static_cast<uint64_t>(ptr[i]) << (8 * i);
    }
    return val;
}

/**
 * @brief 泛化读取函数，根据类型自动选择合适大小的 little-endian 读取方式
 */
template<typename T>
inline T read_uint(const uint8_t *ptr) {
    static_assert(std::is_integral<T>::value && std::is_unsigned<T>::value,
                  "read_uint only supports unsigned integral types");
    if constexpr (sizeof(T) == 1) return read_uint1<T>(ptr);
    else if constexpr (sizeof(T) == 2) return read_uint2<T>(ptr);
    else if constexpr (sizeof(T) == 4) return read_uint4<T>(ptr);
    else if constexpr (sizeof(T) == 8) return read_uint8<T>(ptr);
}

/**
 * @brief 从字节流中读取 little-endian 格式的有符号整数（1 字节）
 */
template<typename T>
inline typename std::enable_if<std::is_same<T, int8_t>::value, T>::type
read_int1(const uint8_t *ptr) {
    return static_cast<int8_t>(*ptr);
}

/**
 * @brief 从字节流中读取 little-endian 格式的有符号整数（2 字节）
 */
template<typename T>
inline typename std::enable_if<std::is_same<T, int16_t>::value, T>::type
read_int2(const uint8_t *ptr) {
    return static_cast<int16_t>(static_cast<uint16_t>(ptr[0]) |
                                (static_cast<uint16_t>(ptr[1]) << 8));
}

/**
 * @brief 从字节流中读取 little-endian 格式的有符号整数（4 字节）
 */
template<typename T>
inline typename std::enable_if<std::is_same<T, int32_t>::value, T>::type
read_int4(const uint8_t *ptr) {
    return static_cast<int32_t>(static_cast<uint32_t>(ptr[0]) |
                                (static_cast<uint32_t>(ptr[1]) << 8) |
                                (static_cast<uint32_t>(ptr[2]) << 16) |
                                (static_cast<uint32_t>(ptr[3]) << 24));
}

/**
 * @brief 从字节流中读取 little-endian 格式的有符号整数（8 字节）
 */
template<typename T>
inline typename std::enable_if<std::is_same<T, int64_t>::value, T>::type
read_int8(const uint8_t *ptr) {
    return static_cast<int64_t>(read_uint8<uint64_t>(ptr));
}

/**
 * @brief 泛化读取函数，根据类型自动选择合适大小的 little-endian 有符号整数读取方式
 */
template<typename T>
inline T read_int(const uint8_t *ptr) {
    static_assert(std::is_integral<T>::value && std::is_signed<T>::value,
                  "read_int only supports signed integral types");
    if constexpr (sizeof(T) == 1) return read_int1<T>(ptr);
    else if constexpr (sizeof(T) == 2) return read_int2<T>(ptr);
    else if constexpr (sizeof(T) == 4) return read_int4<T>(ptr);
    else if constexpr (sizeof(T) == 8) return read_int8<T>(ptr);
}

} // namespace mach
} // namespace dbrecovery
