/**
 * @file shared_tablespace.cpp
 * @brief SharedTablespaceRecoveryEngine — out-of-line definitions
 *
 * The SharedTablespaceRecoveryEngine facade is fully inline in the header.
 * This file provides out-of-line definitions for any non-inline methods.
 */

#include "dbrecovery/enterprise/shared_tablespace.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace dbrecovery {
namespace enterprise {

// SharedTablespaceRecoveryEngine methods are inline in shared_tablespace.hpp.
// This file exists to ensure the translation unit is registered in CMake.

} // namespace enterprise
} // namespace dbrecovery