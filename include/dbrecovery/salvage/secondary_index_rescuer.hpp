#ifndef DBRECOVERY_SALVAGE_SECONDARY_INDEX_RESCUER_HPP
#define DBRECOVERY_SALVAGE_SECONDARY_INDEX_RESCUER_HPP

// TODO: Implement per architecture_v2_blueprint.md
// Equivalent implementation exists at:
// include/dbrecovery/innodb/secondary_index_rescuer.hpp
// include/dbrecovery/innodb/sec_index_btree_repair.hpp
#include "dbrecovery/innodb/secondary_index_rescuer.hpp"
#include "dbrecovery/innodb/sec_index_btree_repair.hpp"

#include <cstdint>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <memory>
#include <set>

namespace dbrecovery {
namespace salvage {

// Stub - full implementation deferred
// SecondaryIndexRescuer (salvage namespace) extends the innodb version
// with salvage-specific features: cross-index row reconstruction,
// partial index merging, and confidence scoring for secondary-index-only
// recovery when the primary index is unavailable.

} // namespace salvage
} // namespace dbrecovery

#endif // DBRECOVERY_SALVAGE_SECONDARY_INDEX_RESCUER_HPP