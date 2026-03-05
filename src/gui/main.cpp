#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>

namespace dbrecovery::innodb {

// ✅ 显式实例化模板（避免链接错误）
template uint32_t MachRead::read_le<std::uint32_t>(std::span<const std::uint8_t>, size_t);
template uint64_t MachRead::read_le<std::uint64_t>(std::span<const std::uint8_t>, size_t);

// ✅ 模板实现（确保跨平台字节序安全）
template<typename T>
T MachRead::read_le(std::span<const uint8_t> data, size_t offset) {
  static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "Only integral types supported");
  if (offset + sizeof(T) > data.size()) {
    throw std::out_of_range("MachRead: read_le out of range");
  }
  const uint8_t* ptr = data.data() + offset;
  T val = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    val |= static_cast<T>(ptr[i]) << (8 * i);
  }
  return val;
}

} // namespace dbrecovery::innodb
