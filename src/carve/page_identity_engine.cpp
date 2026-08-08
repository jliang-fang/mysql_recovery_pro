/**
 * @file page_identity_engine.cpp
 * @brief PageIdentityEngine — out-of-line definitions
 *
 * All PageIdentityEngine methods are currently inline in the header.
 * This .cpp is reserved for out-of-line definitions when the implementation
 * grows beyond the inline threshold.
 */

#include "dbrecovery/carve/page_identity_engine.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/page_type_inferrer.hpp"

namespace dbrecovery {
namespace carve {

// All methods are inline in page_identity_engine.hpp.
// This file exists to ensure the translation unit is registered in CMake.

} // namespace carve
} // namespace dbrecovery