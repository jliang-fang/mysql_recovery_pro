/**
 * @file system_table_sdi.hpp
 * @brief SDI definitions for MySQL system tables
 *
 * Contains hardcoded SDI for SYS_TABLES, SYS_COLUMNS, SYS_INDEXES, SYS_FIELDS
 * Used for parsing MySQL 5.7 system tablespace (ibdata1)
 */

#ifndef DBRECOVERY_ENGINE_SYSTEM_TABLE_SDI_HPP
#define DBRECOVERY_ENGINE_SYSTEM_TABLE_SDI_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace dbrecovery {
namespace engine {

// MySQL type constants
namespace mysql_types {
    constexpr uint32_t DECIMAL = 0;
    constexpr uint32_t TINY = 1;
    constexpr uint32_t SHORT = 2;
    constexpr uint32_t LONG = 3;
    constexpr uint32_t FLOAT = 4;
    constexpr uint32_t DOUBLE = 5;
    constexpr uint32_t NULL_TYPE = 6;
    constexpr uint32_t TIMESTAMP = 7;
    constexpr uint32_t LONGLONG = 8;
    constexpr uint32_t INT24 = 9;
    constexpr uint32_t DATE = 10;
    constexpr uint32_t TIME = 11;
    constexpr uint32_t DATETIME = 12;
    constexpr uint32_t YEAR = 13;
    constexpr uint32_t NEWDATE = 14;
    constexpr uint32_t VARCHAR = 15;
    constexpr uint32_t BIT = 16;
    constexpr uint32_t JSON = 17;
    constexpr uint32_t NEWDECIMAL = 18;
    constexpr uint32_t ENUM = 19;
    constexpr uint32_t SET = 20;
    constexpr uint32_t TINY_BLOB = 21;
    constexpr uint32_t MEDIUM_BLOB = 22;
    constexpr uint32_t LONG_BLOB = 23;
    constexpr uint32_t BLOB = 24;
    constexpr uint32_t VAR_STRING = 25;
    constexpr uint32_t STRING = 26;
    constexpr uint32_t GEOMETRY = 27;
}

// InnoDB main type constants
namespace innodb_mtypes {
    constexpr uint32_t DATA_MISSING = 0;
    constexpr uint32_t DATA_VARCHAR = 1;
    constexpr uint32_t DATA_CHAR = 2;
    constexpr uint32_t DATA_FIXBINARY = 3;
    constexpr uint32_t DATA_BINARY = 4;
    constexpr uint32_t DATA_BLOB = 5;
    constexpr uint32_t DATA_INT = 6;
    constexpr uint32_t DATA_SYS_CHILD = 7;
    constexpr uint32_t DATA_SYS = 8;
    constexpr uint32_t DATA_FLOAT = 9;
    constexpr uint32_t DATA_DOUBLE = 10;
    constexpr uint32_t DATA_DECIMAL = 11;
    constexpr uint32_t DATA_VARMYSQL = 12;
    constexpr uint32_t DATA_MYSQL = 13;
    constexpr uint32_t DATA_GEOMETRY = 14;
    constexpr uint32_t DATA_POINT = 15;
}

// prtype flags
namespace prtype_flags {
    constexpr uint32_t DATA_NOT_NULL = 256;
    constexpr uint32_t DATA_UNSIGNED = 512;
    constexpr uint32_t DATA_BINARY_TYPE = 1024;
    constexpr uint32_t DATA_GIS_MBR = 2048;
    constexpr uint32_t DATA_LONG_TRUE_VARCHAR = 4096;
    constexpr uint32_t DATA_VIRTUAL = 8192;
    constexpr uint32_t DATA_MYSQL_TYPE_MASK = 255;
    constexpr uint32_t DATA_SYS_PRTYPE_MASK = 0xF;
}

// MySQL 5.7 system table index IDs
namespace sys_index_ids {
    constexpr uint64_t SYS_TABLES = 1;
    constexpr uint64_t SYS_COLUMNS = 2;
    constexpr uint64_t SYS_INDEXES = 3;
    constexpr uint64_t SYS_FIELDS = 4;
    constexpr uint64_t SYS_FOREIGN = 11;
    constexpr uint64_t SYS_FOREIGN_COLS = 14;
    constexpr uint64_t SYS_TABLESPACES = 15;
    constexpr uint64_t SYS_DATAFILES = 16;
    constexpr uint64_t SYS_VIRTUAL = 17;
}

// MySQL 8.0 DD table index IDs
namespace dd_index_ids {
    constexpr uint64_t TABLES = 1;
}

// Column definition for system tables
struct SystemColumn {
    std::string name;
    uint32_t mysql_type;
    uint32_t innodb_type;
    bool is_nullable;
    bool is_unsigned;
    uint32_t length;
    uint32_t ordinal_position;
};

// SYS_TABLES column definitions (MySQL 5.7)
inline std::vector<SystemColumn> get_sys_tables_columns() {
    return {
        {"name", mysql_types::VARCHAR, innodb_mtypes::DATA_VARCHAR, false, false, 654, 1},
        {"id", mysql_types::LONGLONG, innodb_mtypes::DATA_INT, false, true, 8, 2},
        {"n_cols", mysql_types::LONG, innodb_mtypes::DATA_INT, false, true, 4, 3},
        {"type", mysql_types::LONG, innodb_mtypes::DATA_INT, false, true, 4, 4},
        {"mix_id", mysql_types::LONGLONG, innodb_mtypes::DATA_INT, false, true, 8, 5},
        {"mix_len", mysql_types::LONG, innodb_mtypes::DATA_INT, false, true, 4, 6},
        {"cluster_name", mysql_types::VARCHAR, innodb_mtypes::DATA_VARCHAR, true, false, 255, 7},
        {"space", mysql_types::LONG, innodb_mtypes::DATA_INT, false, true, 4, 8},
        // Hidden system columns
        {"DB_TRX_ID", mysql_types::INT24, innodb_mtypes::DATA_SYS, false, false, 6, 9},
        {"DB_ROLL_PTR", mysql_types::LONGLONG, innodb_mtypes::DATA_SYS, false, false, 7, 10}
    };
}

// SYS_COLUMNS column definitions (MySQL 5.7)
inline std::vector<SystemColumn> get_sys_columns_columns() {
    return {
        {"table_id", mysql_types::LONGLONG, innodb_mtypes::DATA_INT, false, true, 8, 1},
        {"pos", mysql_types::LONG, innodb_mtypes::DATA_INT, false, true, 4, 2},
        {"name", mysql_types::VARCHAR, innodb_mtypes::DATA_VARCHAR, true, false, 255, 3},
        {"mtype", mysql_types::LONG, innodb_mtypes::DATA_INT, true, true, 4, 4},
        {"prtype", mysql_types::LONG, innodb_mtypes::DATA_INT, true, true, 4, 5},
        {"len", mysql_types::LONG, innodb_mtypes::DATA_INT, true, true, 4, 6},
        {"prec", mysql_types::LONG, innodb_mtypes::DATA_INT, true, true, 4, 7},
        // Hidden system columns
        {"DB_TRX_ID", mysql_types::INT24, innodb_mtypes::DATA_SYS, false, false, 6, 8},
        {"DB_ROLL_PTR", mysql_types::LONGLONG, innodb_mtypes::DATA_SYS, false, false, 7, 9}
    };
}

// SYS_INDEXES column definitions (MySQL 5.7)
inline std::vector<SystemColumn> get_sys_indexes_columns() {
    return {
        {"table_id", mysql_types::LONGLONG, innodb_mtypes::DATA_INT, false, true, 8, 1},
        {"id", mysql_types::LONGLONG, innodb_mtypes::DATA_INT, false, true, 8, 2},
        {"name", mysql_types::VARCHAR, innodb_mtypes::DATA_VARCHAR, true, false, 255, 3},
        {"n_fields", mysql_types::LONG, innodb_mtypes::DATA_INT, true, true, 4, 4},
        {"type", mysql_types::LONG, innodb_mtypes::DATA_INT, true, true, 4, 5},
        {"space", mysql_types::LONG, innodb_mtypes::DATA_INT, true, true, 4, 6},
        {"page_no", mysql_types::LONG, innodb_mtypes::DATA_INT, true, true, 4, 7},
        // Hidden system columns
        {"DB_TRX_ID", mysql_types::INT24, innodb_mtypes::DATA_SYS, false, false, 6, 8},
        {"DB_ROLL_PTR", mysql_types::LONGLONG, innodb_mtypes::DATA_SYS, false, false, 7, 9}
    };
}

// SYS_FIELDS column definitions (MySQL 5.7)
inline std::vector<SystemColumn> get_sys_fields_columns() {
    return {
        {"index_id", mysql_types::LONGLONG, innodb_mtypes::DATA_INT, false, true, 8, 1},
        {"pos", mysql_types::LONG, innodb_mtypes::DATA_INT, false, true, 4, 2},
        {"col_name", mysql_types::VARCHAR, innodb_mtypes::DATA_VARCHAR, true, false, 255, 3},
        // Hidden system columns
        {"DB_TRX_ID", mysql_types::INT24, innodb_mtypes::DATA_SYS, false, false, 6, 4},
        {"DB_ROLL_PTR", mysql_types::LONGLONG, innodb_mtypes::DATA_SYS, false, false, 7, 5}
    };
}

// MySQL type to InnoDB type conversion
inline uint32_t mysql_to_innodb_type(uint32_t mysql_type) {
    switch (mysql_type) {
        case mysql_types::TINY: return innodb_mtypes::DATA_INT;
        case mysql_types::SHORT: return innodb_mtypes::DATA_INT;
        case mysql_types::LONG: return innodb_mtypes::DATA_INT;
        case mysql_types::LONGLONG: return innodb_mtypes::DATA_INT;
        case mysql_types::INT24: return innodb_mtypes::DATA_INT;
        case mysql_types::FLOAT: return innodb_mtypes::DATA_FLOAT;
        case mysql_types::DOUBLE: return innodb_mtypes::DATA_DOUBLE;
        case mysql_types::DECIMAL:
        case mysql_types::NEWDECIMAL: return innodb_mtypes::DATA_DECIMAL;
        case mysql_types::TIMESTAMP: return innodb_mtypes::DATA_INT;
        case mysql_types::DATE:
        case mysql_types::DATETIME: return innodb_mtypes::DATA_INT;
        case mysql_types::TIME: return innodb_mtypes::DATA_INT;
        case mysql_types::VARCHAR:
        case mysql_types::VAR_STRING: return innodb_mtypes::DATA_VARCHAR;
        case mysql_types::STRING: return innodb_mtypes::DATA_CHAR;
        case mysql_types::BIT: return innodb_mtypes::DATA_INT;
        case mysql_types::JSON:
        case mysql_types::BLOB:
        case mysql_types::TINY_BLOB:
        case mysql_types::MEDIUM_BLOB:
        case mysql_types::LONG_BLOB: return innodb_mtypes::DATA_BLOB;
        case mysql_types::GEOMETRY: return innodb_mtypes::DATA_GEOMETRY;
        case mysql_types::ENUM:
        case mysql_types::SET: return innodb_mtypes::DATA_VARCHAR;
        default: return innodb_mtypes::DATA_VARCHAR;
    }
}

// Get MySQL type name
inline std::string mysql_type_name(uint32_t mysql_type) {
    static const std::map<uint32_t, std::string> type_names = {
        {0, "DECIMAL"}, {1, "TINY"}, {2, "SHORT"}, {3, "LONG"},
        {4, "FLOAT"}, {5, "DOUBLE"}, {6, "NULL"}, {7, "TIMESTAMP"},
        {8, "LONGLONG"}, {9, "INT24"}, {10, "DATE"}, {11, "TIME"},
        {12, "DATETIME"}, {13, "YEAR"}, {14, "NEWDATE"}, {15, "VARCHAR"},
        {16, "BIT"}, {17, "JSON"}, {18, "NEWDECIMAL"}, {19, "ENUM"},
        {20, "SET"}, {21, "TINY_BLOB"}, {22, "MEDIUM_BLOB"},
        {23, "LONG_BLOB"}, {24, "BLOB"}, {25, "VAR_STRING"},
        {26, "STRING"}, {27, "GEOMETRY"},
    };
    auto it = type_names.find(mysql_type);
    return (it != type_names.end()) ? it->second : "UNKNOWN";
}

// Get row format from table flags
inline std::string row_format_name(uint32_t flags) {
    if (!(flags & 1)) return "REDUNDANT";
    else if (!(flags & 32)) return "COMPACT";
    else if ((flags & 30) >> 1) return "COMPRESSED";
    else return "DYNAMIC";
}

// Index type conversion for MySQL 5.7
inline uint32_t index57_type_to_int(uint32_t type) {
    if (type & 1) return 1;  // PRIMARY
    else if (type & 2) return 2;  // UNIQUE
    else if (type & 32) return 4;  // FULLTEXT
    else if (type & 64) return 5;  // SPATIAL
    else return 3;  // MULTIPLE
}

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_SYSTEM_TABLE_SDI_HPP
