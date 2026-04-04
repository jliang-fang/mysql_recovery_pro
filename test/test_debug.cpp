#include <iostream>
#include <vector>
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"

using namespace dbrecovery::innodb;
using namespace dbrecovery::catalog;

int main() {
    std::vector<uint8_t> page_data(16384, 0);

    // Set page type to INDEX
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 16, 9969);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 42, 0);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 80, 0x0701);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 40, 150);
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 38, 3);

    // Set INFIMUM record
    page_data[54] = REC_STATUS_INFIMUM;
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 55, 75);

    // Set user record 1 (deleted)
    page_data[75] = REC_INFO_DELETED_FLAG;
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 76, 99);

    // Set user record 2 (valid)
    page_data[99] = 0x00;
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 100, 120);

    // Set SUPREMUM record at offset 120
    page_data[120] = REC_STATUS_SUPREMUM;
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 121, 0xFFFF);

    // Set n_heap
    dbrecovery::innodb::write_le<uint16_t>(page_data.data(), 42, 5);

    // Create table schema
    TableSchema schema;
    schema.table_name = "deleted_table";
    schema.row_format = 1;

    ColumnInfo col;
    col.name = "id";
    col.mysql_type_id = mysql_types::LONGLONG;
    col.is_nullable = false;
    schema.columns[1] = col;

    // Parse the page
    BPlusTreeParser parser(page_data);
    bool parse_result = parser.parse();
    std::cout << "parse() result: " << parse_result << std::endl;
    
    auto records = parser.parse_records(schema);
    std::cout << "records.size(): " << records.size() << std::endl;

    return 0;
}
