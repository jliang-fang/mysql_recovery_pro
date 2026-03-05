#pragma once

#include <cstdint>
#include <string>
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"

namespace dbrecovery {
namespace innodb {

// Minimal stub for RecoveryCache (v2 dependency)
class RecoveryCache {
public:
    void add_page(uint64_t page_id, const uint8_t* data) {}
};

} // namespace innodb
} // namespace dbrecovery

