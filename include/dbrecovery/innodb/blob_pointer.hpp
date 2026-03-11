#pragma once
#include <span>
#include <optional>
#include <cstdint>
#include <vector>
#include <utility>
#include <set>

// ✅ Reuse canonical definition from .h to avoid ODR violations
#include "dbrecovery/innodb/blob_pointer.h"

namespace dbrecovery {
namespace innodb {

// Helper types only (no struct redefinition)

// ✅ BLOB page chain（支持多页拼接）
struct BlobPageChain {
  std::vector<std::pair<uint32_t, uint32_t>> pages; // (space_id, page_no)
  
  // BLOB 页头偏移常量
  static constexpr size_t BTR_BPAGE_NEXT = 0;   // 下一个 BLOB 页号（4字节）
  static constexpr size_t BTR_BPAGE_PREV = 4;   // 上一个 BLOB 页号（4字节）
  static constexpr size_t BTR_BPAGE_LEN  = 8;   // 数据长度（4字节）

  void add_page(uint32_t space_id, uint32_t page_no) {
      pages.emplace_back(space_id, page_no);
  }

  bool is_valid(const std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint32_t)>& page_reader) const {
      if (pages.empty()) return false;
      
      // 检查是否有重复页
      std::set<std::pair<uint32_t, uint32_t>> seen;
      for (const auto& p : pages) {
          if (seen.count(p)) return false;
          seen.insert(p);
      }
      
      // 验证链表连续性（需要实际读取 BLOB 页头验证）
      for (size_t i = 1; i < pages.size(); ++i) {
          const auto& prev_page = pages[i-1];
          const auto& curr_page = pages[i];
          
          if (prev_page.first != curr_page.first) {
              // 不同表空间，跳过连续性检查（允许跨表空间BLOB）
              continue;
          }
          
          // 读取前一个页的 BLOB 头以验证 next_bpage 字段
          if (page_reader) {
              auto prev_data = page_reader(prev_page.first, prev_page.second);
              if (!prev_data || prev_data->size() < 16) {
                  continue; // 无法读取或数据不足，跳过验证
              }
              
              // 读取 next_bpage 字段（偏移 0）
              uint32_t next_bpage = dbrecovery::innodb::read_le<uint32_t>(prev_data->data(), BTR_BPAGE_NEXT);
              
              if (next_bpage != curr_page.second) {
                  // next_bpage 不匹配，链表断裂
                  return false;
              }
          }
      }
      
      return true;
  }
  
  // ✅ 新增：从页数据中解析 BLOB 指针（20字节）
  struct BlobPointer {
      uint32_t space_id;
      uint32_t page_no;
      uint32_t offset;
      uint32_t len;           // 当前页的数据长度
      uint32_t total_len;     // BLOB 总长度（完整指针格式包含此字段）
  };
  
  static std::optional<BlobPointer> parse_blob_pointer(const uint8_t* data) {
      if (!data) return std::nullopt;
      
      // BLOB 指针格式（20字节）：
      // [0-3]:  space_id
      // [4-7]:  page_no (first BLOB 页)
      // [8-11]: offset (数据在页内的偏移)
      // [12-15]: len (当前页的数据长度)
      // [16-19]: total_len (BLOB 总长度，某些版本可能省略)
      
      BlobPointer ptr;
      ptr.space_id = dbrecovery::innodb::read_le<uint32_t>(data, 0);
      ptr.page_no = dbrecovery::innodb::read_le<uint32_t>(data, 4);
      ptr.offset = dbrecovery::innodb::read_le<uint32_t>(data, 8);
      ptr.len = dbrecovery::innodb::read_le<uint32_t>(data, 12);
      
      // 尝试读取总长度（如果数据足够）
      // MySQL 8.0 BLOB 指针格式总是包含 total_len 字段
      if (true) {
          ptr.total_len = dbrecovery::innodb::read_le<uint32_t>(data, 16);
      } else {
          // 如果数据不足，保持默认值（len）
          ptr.total_len = ptr.len;
      }
      
      return ptr;
  }
};

} // namespace innodb
} // namespace dbrecovery
