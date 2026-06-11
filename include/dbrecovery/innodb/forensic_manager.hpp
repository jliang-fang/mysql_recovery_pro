#ifndef DBRECOVERY_INNODB_FORENSIC_MANAGER_HPP
#define DBRECOVERY_INNODB_FORENSIC_MANAGER_HPP

#include <string>
#include <vector>
#include <memory>
#include "dbrecovery/innodb/forensic_recovery.hpp"
#include "dbrecovery/innodb/heuristic_parser.hpp"

namespace dbrecovery {
namespace innodb {

/**
 * @brief ForensicRecoveryManager coordinates the binary scanning and tuple extraction process.
 * It bridges the gap between raw binary pages and heuristic record parsing.
 */
class ForensicRecoveryManager {
public:
    ForensicRecoveryManager();
    ~ForensicRecoveryManager();

    // Performs full forensic recovery from a file or disk image
    bool perform_deep_recovery(const std::string& target_path, const std::string& output_csv);

private:
    void save_to_csv(const std::string& path, const std::vector<HeuristicRecord>& records);

    std::unique_ptr<InodeScanner> scanner_;
    std::unique_ptr<TupleExtractor> extractor_;
    std::unique_ptr<HeuristicParser> heuristic_parser_;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_FORENSIC_MANAGER_HPP
