#ifndef DBRECOVERY_UNDO_DEEP_UNDO_ANALYZER_HPP
#define DBRECOVERY_UNDO_DEEP_UNDO_ANALYZER_HPP

/**
 * @file deep_undo_analyzer.hpp
 * @brief Undo namespace forwarding header
 *
 * Re-exports dbrecovery::innodb::DeepUndoAnalyzer into the dbrecovery::undo
 * namespace for backward compatibility with code using the undo namespace path.
 *
 * The full implementation lives in include/dbrecovery/innodb/deep_undo_analyzer.hpp
 * and src/innodb/deep_undo_analyzer.cpp.
 */

#include "dbrecovery/innodb/deep_undo_analyzer.hpp"

namespace dbrecovery {
namespace undo {

// Re-export the innodb implementation into the undo namespace
using DeepUndoAnalyzer = innodb::DeepUndoAnalyzer;

} // namespace undo
} // namespace dbrecovery

#endif // DBRECOVERY_UNDO_DEEP_UNDO_ANALYZER_HPP