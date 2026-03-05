#pragma once
#include <span>
#include <optional>
#include <cstdint>
#include <vector>
#include <utility>

// ✅ Reuse canonical definition from .h to avoid ODR violations
#include "dbrecovery/innodb/blob_pointer.h"

namespace dbrecovery::innodb {

// Helper types only (no struct redefinition)

// ✅ BLOB page chain（支持多页拼接）
struct BlobPageChain {
  std::vector<std::pair<uint32_t, uint32_t>> pages; // (space_id, page_no)

  void add_page(uint32_t space_id, uint32_t page_no);
  bool is_valid() const;
};

} // namespace dbrecovery::innodb
