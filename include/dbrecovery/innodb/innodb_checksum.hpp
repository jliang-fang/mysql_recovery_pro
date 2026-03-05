#pragma once

#include <cstdint>
#include <cstddef>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace innodb {

/**
 * @brief 运行时检测 CPU 是否支持 CRC32C 指令集（SSE4.2）
 *
 * @return bool true 表示支持硬件加速 CRC32C；false 表示不支持
 */
inline bool cpu_supports_crc32c() {
#if defined(__SSE4_2__) && !defined(__APPLE__)
    if (std::is_constant_evaluated()) return false;
    unsigned int eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (ecx & (1u << 20)) != 0; // SSE4.2 bit
#else
    return false;
#endif
}

/**
 * @brief 计算InnoDB页校验和（CRC32C）
 *
 * 使用硬件加速的CRC32C算法计算给定缓冲区的校验和。
 * 适用于InnoDB数据页、日志块等结构。
 * 若 CPU 不支持，则回退到 zlib 的 crc32() 实现。
 *
 * @param buf 待校验的数据缓冲区
 * @param len 缓冲区长度（字节）
 * @return uint32_t 校验和值
 */
uint32_t calc_crc32c(const void* buf, size_t len);

/**
 * @brief 计算InnoDB页校验和（传统算法：innodb_checksum_algorithm=INNODB）
 *
 * 使用InnoDB原始的异或+累加方式计算校验和。
 * 用于兼容旧版本或特定配置场景。
 *
 * @param buf 待校验的数据缓冲区
 * @param len 缓冲区长度（字节），应为页大小整数倍
 * @return uint32_t 校验和值
 */
uint32_t calc_innodb_checksum(const void* buf, size_t len);

/**
 * @brief 计算InnoDB页校验和（CRC32算法：innodb_checksum_algorithm=CRC32）
 *
 * 使用标准CRC32（非CRC32C）计算校验和。
 * 注意：与zlib的crc32()实现一致，但不使用硬件加速。
 *
 * @param buf 待校验的数据缓冲区
 * @param len 缓冲区长度（字节）
 * @return uint32_t 校验和值
 */
uint32_t calc_crc32(const void* buf, size_t len);

/**
 * @brief 根据配置选择校验和算法并计算
 *
 * @param buf 待校验的数据缓冲区
 * @param len 缓冲区长度（字节）
 * @param algorithm 校验和算法类型：0=INNODB, 1=CRC32, 2=CRC32C (默认)
 * @return uint32_t 计算得到的校验和
 */
uint32_t calc_checksum(const void* buf, size_t len, int algorithm = 2);

} // namespace innodb