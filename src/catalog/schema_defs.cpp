#include "dbrecovery/catalog/schema_defs.hpp"
#include <iostream>

namespace dbrecovery {
namespace catalog {

void TableSchema::debug_print() const {
    std::cout << "Table: " << table_name 
              << ", Engine: " << engine 
              << ", Row Format: " << row_format << std::endl;
    for (const auto& [pos, col] : columns) {
        std::cout << "  [" << pos << "] " << col.name 
                  << " (type=" << col.mysql_type_id << ")" << std::endl;
    }
}

} // namespace catalog
} // namespace dbrecovery
