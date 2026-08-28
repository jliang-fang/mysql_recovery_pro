#ifndef DBRECOVERY_UNDO_UNDO_RECORD_V2_HPP
#define DBRECOVERY_UNDO_UNDO_RECORD_V2_HPP

// Stable forwarding aliases for the authoritative InnoDB V2 undo model.
#include "dbrecovery/innodb/deep_undo_analyzer.hpp"

namespace dbrecovery {
namespace undo {

using UndoFieldValue = innodb::UndoFieldValue;
using UndoRecordV2 = innodb::UndoRecordV2;
using UndoFormatProfile = innodb::UndoFormatProfile;
using UndoRecordV2Parser = innodb::UndoRecordV2Parser;
using HistoryIntegrity = innodb::HistoryIntegrity;
using RowVersionHistory = innodb::RowVersionHistory;

} // namespace undo
} // namespace dbrecovery

#endif // DBRECOVERY_UNDO_UNDO_RECORD_V2_HPP