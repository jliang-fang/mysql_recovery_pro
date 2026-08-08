/**
 * @file flashback_engine.cpp
 * @brief FlashbackEngine implementation — delegation wrapper around innodb::FlashbackGenerator
 *
 * All core logic lives in innodb::FlashbackGenerator. This file provides
 * the undo::FlashbackEngine thin wrapper, plus any undo-namespace-specific
 * orchestration that may be added in the future.
 */

#include "dbrecovery/undo/flashback_engine.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <chrono>

namespace dbrecovery {
namespace undo {

// FlashbackEngine is a thin wrapper — all methods are inlined in the header.
// This file exists for ABI stability and future extension points.

} // namespace undo
} // namespace dbrecovery