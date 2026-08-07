#ifndef DBRECOVERY_INFERENCE_DDL_CHANGE_DETECTOR_HPP
#define DBRECOVERY_INFERENCE_DDL_CHANGE_DETECTOR_HPP

// TODO: Implement per architecture_v2_blueprint.md
// Equivalent implementation exists at:
// include/dbrecovery/innodb/ddl_change_detector.hpp
#include "dbrecovery/innodb/ddl_change_detector.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <chrono>

namespace dbrecovery {
namespace inference {

// Stub - full implementation deferred
// DDLChangeDetector (inference namespace) extends the innodb version
// with schema-aware detection of DDL changes across page versions,
// enabling recovery across schema evolution boundaries.

} // namespace inference
} // namespace dbrecovery

#endif // DBRECOVERY_INFERENCE_DDL_CHANGE_DETECTOR_HPP