#ifndef DBRECOVERY_CATALOG_SCHEMA_TYPE_CONVERSION_HPP
#define DBRECOVERY_CATALOG_SCHEMA_TYPE_CONVERSION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace catalog {

/**
 * Source domains for column type IDs. Callers must declare which domain the
 * numeric ID was read from; mixing domains is a conversion error.
 */
enum class TypeIdDomain {
    kMysqlServer,   ///< MySQL server type IDs (enum_field_types: 0-255)
    kInnodbParser,  ///< InnoDB internal type IDs already in parser domain (1-32)
};

/**
 * Result of a schema type-ID conversion. Either carries a verified InnoDB
 * parser type ID or an explicit diagnostic describing the rejection.
 */
struct TypeConversionResult {
    bool ok = false;
    uint8_t parser_type = 0;   ///< Valid only when ok == true (InnoDB ID 1-32)
    std::string diagnostic;    ///< Rejection reason when ok == false
};

/**
 * Build an IndexColumnDef from a catalog ColumnInfo, routing the type ID
 * through the fail-closed conversion boundary and propagating every other
 * supported field only after the type validates.
 *
 * Returns std::nullopt with a diagnostic appended to `diagnostics` when the
 * column type cannot be converted. `physical_pos_override` (0 = keep
 * source.physical_pos) lets callers substitute a computed position.
 */
std::optional<innodb::IndexColumnDef> build_index_column(
    const ColumnInfo& source,
    uint32_t ordinal_position,
    uint32_t physical_pos_override,
    TypeIdDomain domain,
    std::vector<std::string>& diagnostics);

/**
 * Charset-ID to InnoDB mb_maxlen lookup shared by all conversion sites.
 * Returns 1 for unknown/binary charsets.
 */
uint32_t charset_mb_maxlen(uint32_t charset_id);

/**
 * Convert a MySQL server-domain type ID to the InnoDB parser-domain ID.
 *
 * Fail-closed contract:
 *  - Only IDs listed in the explicit mapping table are accepted.
 *  - Unknown, ambiguous, or out-of-range IDs are rejected; no fallback
 *    promotes an unverified ID into the parser domain.
 *  - IDs that are already in the InnoDB parser domain must be supplied with
 *    kInnodbParser; passing a parser-domain ID as kMysqlServer is rejected
 *    unless the ID is unambiguous in the server domain.
 */
TypeConversionResult convert_column_type_id(uint32_t source_type_id,
                                            TypeIdDomain domain);

/**
 * Validate that a value is already a supported InnoDB parser type ID.
 * Equivalent to convert_column_type_id(id, kInnodbParser).
 */
TypeConversionResult validate_parser_type_id(uint32_t parser_type_id);

} // namespace catalog
} // namespace dbrecovery

#endif // DBRECOVERY_CATALOG_SCHEMA_TYPE_CONVERSION_HPP
