#include "dbrecovery/catalog/schema_defs.hpp"
#include <iostream>

namespace dbrecovery {
namespace catalog {

void TableSchema::debug_print() const {
    std::cout << "Table: " << table_name 
              << ", Engine: " << engine 
              << ", Row Format: " << static_cast<int>(row_format) << std::endl;
    
    std::cout << "Columns:" << std::endl;
    for (const auto& [pos, col] : columns) {
        std::cout << "  [" << pos << "] " << col.name 
                  << " (type=" << col.mysql_type_id << ", nullable=" << col.is_nullable
                  << ", length=" << col.column_length << ")" << std::endl;
    }
    
    std::cout << "Indexes:" << std::endl;
    for (const auto& idx : indexes) {
        std::cout << "  - " << idx.name << " (primary=" << idx.is_primary 
                  << ", unique=" << idx.is_unique << ")" << std::endl;
        for (const auto& col : idx.columns) {
            std::cout << "    Column: " << col << std::endl;
        }
    }
    
    if (partition_info.has_value()) {
        const auto& p = partition_info.value();
        std::cout << "Partitioning: method=" << p.method 
                  << ", column=" << p.column_name << std::endl;
    }
}

} // namespace catalog
} // namespace dbrecovery
