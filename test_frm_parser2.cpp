#include "dbrecovery/innodb/frm_parser.hpp"
#include <iostream>

int main() {
    dbrecovery::innodb::FRMParser parser;
    auto schema = parser.parse("/app/mysql_recovery_test/mysql56/data/recovery_test/transaction_records.frm");
    
    if (!schema) {
        std::cerr << "Failed to parse FRM" << std::endl;
        return 1;
    }
    
    std::cout << "transaction_records columns (" << schema->columns.size() << "):\n";
    for (const auto& [pos, col] : schema->columns) {
        std::cout << "  [" << pos << "] " << col.name 
                  << " type=" << (int)col.mysql_type_id 
                  << " len=" << col.column_length
                  << "\n";
    }
    
    return 0;
}
