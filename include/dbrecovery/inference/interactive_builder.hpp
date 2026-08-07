#ifndef DBRECOVERY_INFERENCE_INTERACTIVE_BUILDER_HPP
#define DBRECOVERY_INFERENCE_INTERACTIVE_BUILDER_HPP

// TODO: Implement per architecture_v2_blueprint.md
// Equivalent implementation exists at:
// include/dbrecovery/inference/statistical_classifier.hpp
// include/dbrecovery/catalog/schema_defs.hpp
#include "dbrecovery/inference/statistical_classifier.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <functional>

namespace dbrecovery {
namespace inference {

// Stub - full implementation deferred
// InteractiveBuilder provides an interactive schema refinement workflow:
// users can confirm/reject inferred column types, adjust DDL, and
// re-run recovery with the updated schema.

} // namespace inference
} // namespace dbrecovery

#endif // DBRECOVERY_INFERENCE_INTERACTIVE_BUILDER_HPP