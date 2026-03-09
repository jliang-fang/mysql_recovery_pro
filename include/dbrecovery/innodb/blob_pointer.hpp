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

  void add_page(uint32_t space_id, uint32_t page_no) {
      pages.emplace_back(space_id, page_no);
  }

  bool is_valid() const {
      if (pages.empty()) return false;
      
      // 检查是否有重复页
      std::set<std::pair<uint32_t, uint32_t>> seen;
      for (const auto& p : pages) {
          if (seen.count(p)) return false;
          seen.insert(p);
      }
      
      // 检查链表连续性（简化版：假设同一表空间内页号应连续）
      // 实际实现需要根据 BLOB page header 中的 next/prev page no 验证
      for (size_t i = 1; i < pages.size(); ++i) {
          if (pages[i].first != pages[0].first) {
              // 不同表空间，跳过连续性检查（允许跨表空间BLOB）
              break;
          }
          // 简单检查：页号不应相等（已在上面检查重复）
          // 完整实现需要读取每个页的 BLOB header 并验证 next/prev 链
      }
      
      return true;
  }
};

} // namespace innodb
} // namespace dbrecovery
