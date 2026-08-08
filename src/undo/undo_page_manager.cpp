/**
 * @file undo_page_manager.cpp
 * @brief UndoPageManager implementation — unified facade over innodb undo modules
 */

#include "dbrecovery/undo/undo_page_manager.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace dbrecovery {
namespace undo {

// UndoPageManager is an inline facade — all methods are in the header.
// This file exists for ABI stability and future extension points.

} // namespace undo
} // namespace dbrecovery