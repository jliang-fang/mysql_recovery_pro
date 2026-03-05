#ifndef DBRECOVERY_INNODB_BINARY_READER_HPP
#define DBRECOVERY_INNODB_BINARY_READER_HPP
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

class BinaryReader {
public:
    static void parse_table_name(const std::string& json_str, catalog::TableSchema& schema);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BINARY_READER_HPP
