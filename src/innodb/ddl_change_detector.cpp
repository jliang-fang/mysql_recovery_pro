/**
 * @file ddl_change_detector.cpp
 * @brief DDL Change Chain Detector implementation
 *
 * Scans .ibd files for multiple schema versions (e.g., INSTANT ADD COLUMN).
 * Extracted from ddl_change_detector.hpp for better compilation.
 */

#include "dbrecovery/innodb/ddl_change_detector.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

namespace dbrecovery {
namespace innodb {

// DDLChangeChainDetector is fully inline in the header.
// This file exists to ensure the ODR is satisfied and to provide
// a compilation unit for future non-inline extensions.

} // namespace innodb
} // namespace dbrecovery