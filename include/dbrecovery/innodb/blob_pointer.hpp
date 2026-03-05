#pragma once
#include "mach_read.hpp"
#include <span>
#include <optional>
#include <cstdint>
#include <vector>
#include <utility>

namespace dbrecovery::innodb {

// ✅ 配置化页大小（支持 4K/8K/16K/32K/64K）
constexpr size_t UNIV_PAGE_SIZE_MIN = 4096;
constexpr size_t UNIV_PAGE_SIZE_MAX = 65536;

// ✅ InnoDB BLOB 指针固定长度：20 字节（space_id(4) + page_no(4) + offset(4) + len(8)）
constexpr size_t BLOB_POINTER_LEN = 20;
constexpr uint64_t MAX_BLOB_LENGTH = 4ULL * 1024 * 1024 * 1024; // 4GB

struct BlobPointer {
  uint32_t space_id;
  uint32_t page_no;
  uint32_t offset;      // ✅ 清除溢出标志后的偏移（bit31=overflow flag）
  uint64_t length;
  bool is_overflow_ = false; // ✅ 新增：存储 overflow 标志

  static std::optional<BlobPointer> parse(const uint8_t* data);

  [[nodiscard]] bool is_overflow() const { return is_overflow_; }
};

// ✅ BLOB page chain（支持多页拼接）
struct BlobPageChain {
  std::vector<std::pair<uint32_t, uint32_t>> pages; // (space_id, page_no)

  void add_page(uint32_t space_id, uint32_t page_no);
  bool is_valid() const;
};

} // namespace dbrecovery::innodb
