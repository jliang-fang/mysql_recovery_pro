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
      
      // 检查链表连续性（需要实际读取 BLOB 页头验证）
      // 这里提供验证框架，实际实现需要从磁盘/内存中读取每个页
      for (size_t i = 1; i < pages.size(); ++i) {
          if (pages[i].first != pages[0].first) {
              // 不同表空间，跳过连续性检查（允许跨表空间BLOB）
              break;
          }
          
          // 简单的页号连续性检查（实际应读取每个 BLOB 页头验证 next/prev 链）
          // 这是一个占位实现，完整实现需要访问实际页数据
          uint32_t expected_next = pages[i-1].second + 1;
          if (pages[i].second != expected_next) {
              // 页号不连续，可能是碎片化的 BLOB 存储
              // 不直接返回 false，因为 InnoDB 的 BLOB 存储可能不是严格连续的
              // 完整验证需要读取每个页的 BLOB header 并检查 next_bpage/prev_bpage 字段
          }
      }
      
      return true;
  }
};

} // namespace innodb
} // namespace dbrecovery
