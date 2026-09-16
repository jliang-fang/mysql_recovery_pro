#include "dbrecovery/catalog/schema_type_conversion.hpp"

namespace dbrecovery {
namespace catalog {

namespace {

TypeConversionResult reject(std::string reason) {
    TypeConversionResult result;
    result.ok = false;
    result.parser_type = 0;
    result.diagnostic = std::move(reason);
    return result;
}

TypeConversionResult accept(uint8_t parser_type) {
    TypeConversionResult result;
    result.ok = true;
    result.parser_type = parser_type;
    return result;
}

bool is_known_parser_type(uint8_t id) {
    using namespace innodb::mysql_types;
    switch (id) {
        case DECIMAL: case TINY: case SHORT: case LONG: case FLOAT: case DOUBLE:
        case TIMESTAMP: case LONGLONG: case INT24: case DATE: case TIME:
        case DATETIME: case YEAR: case NEWDATE: case VARCHAR: case BIT:
        case TIMESTAMP2: case DATETIME2: case TIME2: case NEWDECIMAL:
        case ENUM: case SET: case TINY_BLOB: case MEDIUM_BLOB: case LONG_BLOB:
        case BLOB: case VAR_STRING: case STRING: case GEOMETRY: case JSON:
        case VECTOR:
            return true;
        default:
            return false;
    }
}

} // namespace

uint32_t charset_mb_maxlen(uint32_t charset_id) {
    switch (charset_id) {
        case 33: case 83: case 224: return 3;   // utf8 / utf8mb3 family
        case 45: case 46: case 94: case 255: return 4;  // utf8mb4 family
        default: return 1;
    }
}

TypeConversionResult convert_column_type_id(uint32_t source_type_id,
                                            TypeIdDomain domain) {
    if (domain == TypeIdDomain::kInnodbParser) {
        if (source_type_id > 255) {
            return reject("parser-domain type id out of range: " +
                          std::to_string(source_type_id));
        }
        const auto id = static_cast<uint8_t>(source_type_id);
        if (!is_known_parser_type(id)) {
            return reject("unsupported parser-domain type id: " +
                          std::to_string(source_type_id));
        }
        return accept(id);
    }

    // MySQL server domain (enum_field_types). The mapping below mirrors the
    // historical mysql_to_innodb_type() table but rejects every unmapped ID
    // instead of passing it through into the parser domain.
    using namespace innodb::mysql_types;
    switch (source_type_id) {
        case 0:   return accept(DECIMAL);     // MYSQL_TYPE_DECIMAL
        case 1:   return accept(TINY);        // MYSQL_TYPE_TINY
        case 2:   return accept(SHORT);       // MYSQL_TYPE_SHORT
        case 3:   return accept(LONG);        // MYSQL_TYPE_LONG
        case 4:   return accept(FLOAT);       // MYSQL_TYPE_FLOAT
        case 5:   return accept(DOUBLE);      // MYSQL_TYPE_DOUBLE
        case 6:   return reject("MYSQL_TYPE_NULL cannot be a column type");
        case 7:   return accept(TIMESTAMP2);  // MYSQL_TYPE_TIMESTAMP
        case 8:   return accept(LONGLONG);    // MYSQL_TYPE_LONGLONG
        case 9:   return accept(INT24);       // MYSQL_TYPE_INT24
        case 10:  return accept(NEWDATE);     // MYSQL_TYPE_DATE
        case 11:  return accept(TIME2);       // MYSQL_TYPE_TIME
        case 12:  return accept(DATETIME2);   // MYSQL_TYPE_DATETIME
        case 13:  return accept(YEAR);        // MYSQL_TYPE_YEAR
        case 14:  return accept(NEWDATE);     // MYSQL_TYPE_NEWDATE
        case 15:  return accept(VARCHAR);     // MYSQL_TYPE_VARCHAR
        case 16:  return accept(BIT);         // MYSQL_TYPE_BIT
        case 17:  return accept(TIMESTAMP2);  // MYSQL_TYPE_TIMESTAMP2
        case 18:  return accept(DATETIME2);   // MYSQL_TYPE_DATETIME2
        case 19:  return accept(TIME2);       // MYSQL_TYPE_TIME2
        case 245: return accept(JSON);        // MYSQL_TYPE_JSON
        case 246: return accept(NEWDECIMAL);  // MYSQL_TYPE_NEWDECIMAL
        case 247: return accept(ENUM);        // MYSQL_TYPE_ENUM
        case 248: return accept(SET);         // MYSQL_TYPE_SET
        case 249: return accept(TINY_BLOB);   // MYSQL_TYPE_TINY_BLOB
        case 250: return accept(MEDIUM_BLOB); // MYSQL_TYPE_MEDIUM_BLOB
        case 251: return accept(LONG_BLOB);   // MYSQL_TYPE_LONG_BLOB
        case 252: return accept(BLOB);        // MYSQL_TYPE_BLOB
        case 253: return accept(VARCHAR);     // MYSQL_TYPE_VAR_STRING
        case 254: return accept(STRING);      // MYSQL_TYPE_STRING
        case 255: return accept(GEOMETRY);    // MYSQL_TYPE_GEOMETRY
        default:
            return reject("unmapped MySQL server type id: " +
                          std::to_string(source_type_id));
    }
}

TypeConversionResult validate_parser_type_id(uint32_t parser_type_id) {
    return convert_column_type_id(parser_type_id, TypeIdDomain::kInnodbParser);
}

std::optional<innodb::IndexColumnDef> build_index_column(
    const ColumnInfo& source,
    uint32_t ordinal_position,
    uint32_t physical_pos_override,
    TypeIdDomain domain,
    std::vector<std::string>& diagnostics) {

    const auto conversion = convert_column_type_id(source.mysql_type_id, domain);
    if (!conversion.ok) {
        diagnostics.push_back("column '" + source.name + "': " +
                              conversion.diagnostic);
        return std::nullopt;
    }

    innodb::IndexColumnDef column;
    column.name = source.name;
    column.col_type = conversion.parser_type;
    column.col_length = source.column_length;
    column.is_nullable = source.is_nullable;
    column.is_unsigned = !source.is_signed;
    column.char_set_id = source.charset_id;
    column.is_binary = (source.charset_id == 63 || source.collation_id == 63);
    column.precision = source.numeric_precision > 0
        ? static_cast<uint16_t>(source.numeric_precision) : 0;
    column.scale = source.numeric_scale > 0
        ? static_cast<uint16_t>(source.numeric_scale) : 0;
    column.datetime_precision = source.datetime_precision >= 0
        ? static_cast<uint8_t>(source.datetime_precision) : 0;
    column.mb_minlen = 1;
    column.mb_maxlen = charset_mb_maxlen(source.charset_id);
    if (column.is_binary) {
        column.mb_maxlen = 1;
    }
    column.is_big = column.col_length * column.mb_maxlen > 255;
    column.enum_values = source.enum_values;
    column.set_values = source.set_values;
    column.ordinal_position = ordinal_position;
    column.physical_pos = physical_pos_override != 0
        ? physical_pos_override : source.physical_pos;
    return column;
}

} // namespace catalog
} // namespace dbrecovery

