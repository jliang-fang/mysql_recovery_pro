#ifndef DBRECOVERY_INNODB_BINARY_READER_HPP
#define DBRECOVERY_INNODB_BINARY_READER_HPP
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

class BinaryReader {
public:
    static void parse_table_name(const std::string& json_str, catalog::TableSchema& schema);
    
    // ✅ 新增：解析表的列定义
    static void parse_columns(const std::string& json_str, catalog::TableSchema& schema);
    
    // ✅ 新增：解析单个列定义
    static void parse_single_column(const std::string& json_str, catalog::TableSchema& schema);
    
    // ✅ 新增：解析索引定义
    static void parse_indexes(const std::string& json_str, catalog::TableSchema& schema);
    
    // ✅ 新增：解析单个索引定义
    static void parse_single_index(const std::string& json_str, catalog::TableSchema& schema);
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_BINARY_READER_HPP
