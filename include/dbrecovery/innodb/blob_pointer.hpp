#pragma once
#include <span>
#include <optional>
#include <cstdint>
#include <vector>
#include <utility>

namespace dbrecovery::innodb {

// ✅ 配置化页大小（支持 4K/8K/16K/32K/64K）
constexpr size_t UNIV_PAGE_SIZE_MIN = 4096;
constexpr size_t UNIV_PAGE_SIZE_MAX = 65536;

// ✅ InnoDB BLOB 指针长度：Barracuda (MySQL 8.0+) 固定为 24 字节
//    Antelope (MySQL 5.6/5.7) 为 20 字节，但已逐步淘汰
constexpr size_t BLOB_POINTER_LEN = 24;
constexpr uint64_t MAX_BLOB_LENGTH = 4ULL * 1024 * 1024 * 1024; // 4GB

struct BlobPointer {
  uint32_t space_id;
  uint32_t page_no;
  uint32_t offset;      // ✅ bit31=overflow flag — caller must check (offset & 0x80000000)
  uint64_t length;

  static std::optional<BlobPointer> parse(const uint8_t* data);

  [[nodiscard]] bool is_overflow() const { return (offset & 0x80000000) != 0; }
  
  // ✅ 新增：清除 overflow 标志后的偏移
  [[nodiscard]] uint32_t clear_overflow_flag() const { return offset & 0x7FFFFFFF; }
};

// ✅ BLOB page chain（支持多页拼接）
struct BlobPageChain {
  std::vector<std::pair<uint32_t, uint32_t>> pages; // (space_id, page_no)

  void add_page(uint32_t space_id, uint32_t page_no);
  bool is_valid() const;
};

} // namespace dbrecovery::innodb
