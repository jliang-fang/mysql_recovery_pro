#ifndef DBRECOVERY_INNODB_SDI_TYPES_HPP
#define DBRECOVERY_INNODB_SDI_TYPES_HPP

#include <string>
#include <vector>
#include <map>
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// SDI 描述符：用于缓存解析后的表结构信息
struct SDIDescriptor {
    uint32_t space_id;
    catalog::TableSchema table_schema;
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SDI_TYPES_HPP
