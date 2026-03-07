#include "dbrecovery/innodb/binary_reader.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <iostream>
#include <sstream>

namespace dbrecovery {
namespace innodb {

void BinaryReader::parse_table_name(const std::string& json_str, catalog::TableSchema& schema) {
    if (json_str.empty()) return;
    
    // 简化解析：查找 "name" 字段
    size_t name_pos = json_str.find("\"name\"");
    if (name_pos != std::string::npos) {
        size_t colon_pos = json_str.find(':', name_pos);
        if (colon_pos != std::string::npos) {
            size_t start = json_str.find('"', colon_pos + 1);
            if (start != std::string::npos) {
                size_t end = json_str.find('"', start + 1);
                if (end != std::string::npos) {
                    schema.table_name = json_str.substr(start + 1, end - start - 1);
                }
            }
        }
    }
}

} // namespace innodb
} // namespace dbrecovery
