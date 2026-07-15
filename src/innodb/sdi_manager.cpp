#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "nlohmann/json.hpp"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <vector>

using json = nlohmann::json;

namespace dbrecovery {
namespace catalog {


} // namespace catalog
} // namespace dbrecovery

namespace dbrecovery {
namespace innodb {

// Global verbose flag definition
bool g_verbose = false;

catalog::TableSchema SDIManager::parse_sdi_record(const uint8_t* payload, size_t payload_len) {
    catalog::TableSchema schema;

    if (g_verbose) 
    std::cerr << "[DEBUG] parse_sdi_record called with payload_len=" << payload_len << "\n";

    if (payload_len < 20) {
        if (g_verbose)         std::cerr << "[DEBUG] payload_len < 20, returning empty schema\n";
        return schema;
    }

    // Read header (type and id are reserved for future use)
    // uint32_t type = dbrecovery::innodb::read_le<uint32_t>(payload);
    // uint64_t id   = dbrecovery::innodb::read_le<uint64_t>(payload + 4);
    uint32_t uncompressed_len = dbrecovery::innodb::read_le<uint32_t>(payload + 12);
    uint32_t compressed_len   = dbrecovery::innodb::read_le<uint32_t>(payload + 16);

    if (g_verbose) 
    std::cerr << "[DEBUG] uncompressed_len=" << uncompressed_len << ", compressed_len=" << compressed_len << "\n";

    if (compressed_len == 0 || compressed_len > payload_len - 20) {
        if (g_verbose)         std::cerr << "[DEBUG] compressed_len validation failed\n";
        return schema;
    }

    const uint8_t* raw_data = payload + 20;

    // Decompress using zlib directly (avoid dependency on dbrecovery::utils)
    z_stream strm = {};
    strm.next_in = const_cast<Bytef*>(raw_data);
    strm.avail_in = static_cast<uInt>(compressed_len);

    std::vector<uint8_t> decompressed(uncompressed_len);
    strm.next_out = decompressed.data();
    strm.avail_out = static_cast<uInt>(decompressed.size());

    int ret = inflateInit2(&strm, -15); // raw deflate
    if (ret != Z_OK) {
        if (g_verbose)         std::cerr << "[DEBUG] inflateInit2 failed with ret=" << ret << "\n";
        return schema;
    }

    do {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_BUF_ERROR) {
            decompressed.resize(decompressed.size() * 2);
            strm.next_out = decompressed.data() + strm.total_out;
            strm.avail_out = static_cast<uInt>(decompressed.size() - strm.total_out);
        } else if (ret != Z_OK && ret != Z_STREAM_END) {
            if (g_verbose)             std::cerr << "[DEBUG] inflate failed with ret=" << ret << "\n";
            inflateEnd(&strm);
            return schema;
        }
    } while (ret != Z_STREAM_END);

    decompressed.resize(strm.total_out);
    inflateEnd(&strm);

    if (g_verbose) 
    std::cerr << "[DEBUG] Decompressed size=" << decompressed.size() << "\n";
    // ✅ 调试：输出完整的 JSON 内容以检查字段名
    std::string json_str(decompressed.begin(), decompressed.end());
    if (g_verbose)     std::cerr << "[DEBUG SDI JSON] " << json_str.substr(0, std::min(json_str.length(), (size_t)5000)) << "\n";

    // Parse JSON
    try {
        auto json_obj = nlohmann::json::parse(decompressed.begin(), decompressed.end());

        // MySQL 8.0+ SDI uses "dictionary" as root object
        if (json_obj.contains("dictionary")) {
            auto dict = json_obj["dictionary"];
            if (dict.contains("table")) {
                auto table_json = dict["table"];
                schema.table_name = table_json.value("name", "");

                // Parse columns
                if (table_json.contains("columns")) {
                    uint32_t ordinal = 1;
                    if (g_verbose)                     std::cerr << "[DEBUG] Found " << table_json["columns"].size() << " columns in SDI\n";
                    for (const auto& col : table_json["columns"]) {
                        catalog::ColumnInfo column_info;
                        column_info.name = col.value("name", "");

                        // Handle type field - can be string or number
                        if (col.contains("type")) {
                            if (col["type"].is_string()) {
                                // Type is string like "LONGLONG", "VARCHAR", etc.
                                // Map to InnoDB internal type IDs
                                std::string type_str = col["type"].get<std::string>();
                                // CRITICAL: Use InnoDB internal type IDs, NOT MySQL server type IDs!
                                if (g_verbose)                                 std::cerr << "[DEBUG SDI] Column '" << column_info.name << "' type_str='" << type_str << "'\n";
                                if (type_str == "DECIMAL") column_info.mysql_type_id = 1;   // InnoDB DECIMAL
                                else if (type_str == "TINY") column_info.mysql_type_id = 2;   // InnoDB TINY (TINYINT)
                                else if (type_str == "SHORT") column_info.mysql_type_id = 3;  // InnoDB SHORT (SMALLINT)
                                else if (type_str == "LONG") column_info.mysql_type_id = 4;   // InnoDB LONG (INT)
                                else if (type_str == "FLOAT") column_info.mysql_type_id = 5;  // InnoDB FLOAT
                                else if (type_str == "DOUBLE") column_info.mysql_type_id = 6; // InnoDB DOUBLE
                                else if (type_str == "TIMESTAMP") column_info.mysql_type_id = 8;  // InnoDB TIMESTAMP
                                else if (type_str == "LONGLONG") column_info.mysql_type_id = 9;  // InnoDB LONGLONG (BIGINT)
                                else if (type_str == "INT24") column_info.mysql_type_id = 10; // InnoDB INT24 (MEDIUMINT)
                                else if (type_str == "DATE") column_info.mysql_type_id = 11; // InnoDB DATE
                                else if (type_str == "TIME") column_info.mysql_type_id = 12; // InnoDB TIME
                                else if (type_str == "DATETIME") column_info.mysql_type_id = 13;  // InnoDB DATETIME
                                else if (type_str == "YEAR") column_info.mysql_type_id = 14; // InnoDB YEAR
                                else if (type_str == "NEWDATE") column_info.mysql_type_id = 15;  // InnoDB NEWDATE
                                else if (type_str == "VARCHAR") column_info.mysql_type_id = 16;  // InnoDB VARCHAR
                                else if (type_str == "BIT") column_info.mysql_type_id = 17;  // InnoDB BIT
                                else if (type_str == "TIMESTAMP2") column_info.mysql_type_id = 18;  // InnoDB TIMESTAMP2
                                else if (type_str == "DATETIME2") column_info.mysql_type_id = 19;  // InnoDB DATETIME2
                                else if (type_str == "TIME2") column_info.mysql_type_id = 20;  // InnoDB TIME2
                                else if (type_str == "NEWDECIMAL") column_info.mysql_type_id = 21;  // InnoDB NEWDECIMAL
                                else if (type_str == "ENUM") column_info.mysql_type_id = 22;  // InnoDB ENUM
                                else if (type_str == "SET") column_info.mysql_type_id = 23;   // InnoDB SET
                                else if (type_str == "TINY_BLOB") column_info.mysql_type_id = 24;  // InnoDB TINY_BLOB
                                else if (type_str == "MEDIUM_BLOB") column_info.mysql_type_id = 25;  // InnoDB MEDIUM_BLOB
                                else if (type_str == "LONG_BLOB") column_info.mysql_type_id = 26;  // InnoDB LONG_BLOB
                                else if (type_str == "BLOB") column_info.mysql_type_id = 27;  // InnoDB BLOB (TEXT)
                                else if (type_str == "VAR_STRING") column_info.mysql_type_id = 28;  // InnoDB VAR_STRING
                                else if (type_str == "STRING") column_info.mysql_type_id = 29;  // InnoDB STRING (CHAR)
                                else if (type_str == "GEOMETRY") column_info.mysql_type_id = 30;  // InnoDB GEOMETRY
                                else if (type_str == "JSON") column_info.mysql_type_id = 31;  // InnoDB JSON
                                else if (type_str == "VECTOR") column_info.mysql_type_id = 32;  // InnoDB VECTOR
                                // Legacy MySQL server type names (fallback)
                                else if (type_str == "TINYINT") column_info.mysql_type_id = 2;
                                else if (type_str == "SMALLINT") column_info.mysql_type_id = 3;
                                else if (type_str == "INT") column_info.mysql_type_id = 4;
                                else if (type_str == "BIGINT") column_info.mysql_type_id = 9;
                                else if (type_str == "CHAR") column_info.mysql_type_id = 29;
                                else if (type_str == "TEXT") column_info.mysql_type_id = 27;
                                else if (type_str == "LONGBLOB") column_info.mysql_type_id = 26;
                                else column_info.mysql_type_id = 0;
                                if (g_verbose)                                 std::cerr << "[DEBUG SDI] Mapped to mysql_type_id=" << static_cast<int>(column_info.mysql_type_id) << "\n";
                            } else {
                                column_info.mysql_type_id = col.value("type", 0);
                                if (g_verbose)                                 std::cerr << "[DEBUG SDI] Column '" << column_info.name << "' numeric type=" << static_cast<int>(column_info.mysql_type_id) << "\n";
                            }
                        }
                        
                        column_info.is_nullable = col.value("is_nullable", true); // SDI stores true when nullable
                        bool is_unsigned = col.value("is_unsigned", false);
                        column_info.is_signed = !is_unsigned;

                        if (col.contains("default_value")) {
                            column_info.default_value = col["default_value"].get<std::string>();
                        }

                        if (col.contains("length")) {
                            column_info.column_length = col["length"];
                        }

                        // CRITICAL FIX: Read charset_id and collation_id independently
                        // collation_id=63 indicates BINARY charset (for BINARY, VARBINARY, BLOB)
                        if (col.contains("charset_id")) {
                            column_info.charset_id = col["charset_id"];
                        }
                        if (col.contains("collation_id")) {
                            column_info.collation_id = col["collation_id"];
                            // Debug output for binary columns
                            if (column_info.collation_id == 63) {
                                if (g_verbose)                                 std::cerr << "[DEBUG SDI] Binary column detected: " << column_info.name
                                          << " collation_id=" << column_info.collation_id << "\n";
                            }
                        }

                        // Parse numeric precision/scale for DECIMAL types
                        // SDI uses "numeric_precision" and "numeric_scale"
                        if (col.contains("numeric_precision")) {
                            column_info.numeric_precision = col["numeric_precision"];
                        }
                        if (col.contains("numeric_scale")) {
                            column_info.numeric_scale = col["numeric_scale"];
                        }
                        // Also check for "precision" and "scale" as fallback
                        if (col.contains("precision") && column_info.numeric_precision == 0) {
                            column_info.numeric_precision = col["precision"];
                        }
                        if (col.contains("scale") && column_info.numeric_scale == 0) {
                            column_info.numeric_scale = col["scale"];
                        }

                        // Parse datetime_precision for temporal types (DATETIME2, TIMESTAMP2, TIME2)
                        // ✅ 修复：检查多个可能的字段名
                        // 注意: SDI JSON 中可能使用 datetime_precision 或其他名称

                        if (col.contains("datetime_precision")) {
                            if (!col["datetime_precision"].is_null()) {
                                column_info.datetime_precision = col["datetime_precision"].get<int>();
                            }
                        } else if (col.contains("datetimePrec")) {
                            column_info.datetime_precision = col["datetimePrec"].get<int>();
                        } else if (col.contains("decimals")) {
                            // Some MySQL versions use "decimals" for fractional seconds precision
                            column_info.datetime_precision = col["decimals"].get<int>();
                        }

                        // 检查 datetime_precision_null 标志
                        if (col.contains("datetime_precision_null")) {
                            int null_flag = col["datetime_precision_null"].get<int>();
                            if (null_flag == 1) {
                                // 精度为空，使用默认值 0
                                column_info.datetime_precision = 0;
                            }
                        }

                        // Parse ENUM/SET elements (stored as base64 in SDI)
                        // ✅ 修复：区分 ENUM 和 SET，将元素放入正确的列表
                        if (col.contains("elements") && col["elements"].is_array()) {
                            static const std::string base64_chars =
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                            std::vector<std::string>* target_values =
                                (column_info.mysql_type_id == 23) ? &column_info.set_values : &column_info.enum_values;
                            for (const auto& elem : col["elements"]) {
                                if (elem.contains("name")) {
                                    std::string encoded = elem["name"].get<std::string>();
                                    // Simple base64 decode
                                    std::string decoded;
                                    std::vector<int> T(256, -1);
                                    for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;
                                    int val = 0, valb = -8;
                                    for (unsigned char c : encoded) {
                                        if (T[c] == -1) break;
                                        val = (val << 6) + T[c];
                                        valb += 6;
                                        if (valb >= 0) {
                                            decoded.push_back(char((val >> valb) & 0xFF));
                                            valb -= 8;
                                        }
                                    }
                                    target_values->push_back(decoded);
                                }
                            }
                            if (g_verbose)                             std::cerr << "[DEBUG SDI] Column '" << column_info.name << "' has "
                                      << target_values->size() << " elements (type="
                                      << (column_info.mysql_type_id == 23 ? "SET" : "ENUM") << ")\n";
                        }

                        // Parse physical_pos - critical for column storage order
                        // InnoDB stores column DATA by physical_pos
                        // But variable-length SIZES are stored by ordinal_position (schema definition order)
                        if (col.contains("physical_pos")) {
                            column_info.physical_pos = col["physical_pos"].get<uint32_t>();
                        }
                        // Also read ordinal_position for variable-length size byte ordering
                        // This is the schema definition order (CREATE TABLE column order)
                        if (col.contains("ordinal_position")) {
                            column_info.ordinal = col["ordinal_position"].get<uint32_t>();
                        }
                        // Fallback: if physical_pos not present, use ordinal_position
                        if (column_info.physical_pos == 0 && col.contains("ordinal_position")) {
                            column_info.physical_pos = col["ordinal_position"].get<uint32_t>();
                        }

                        // Skip InnoDB system columns (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID)
                        // These are internal columns not part of user table schema
                        if (column_info.name == "DB_TRX_ID" || column_info.name == "DB_ROLL_PTR" || column_info.name == "DB_ROW_ID") {
                            continue;
                        }

                        // If ordinal wasn't set from SDI, use the iteration counter
                        if (column_info.ordinal == 0) {
                            column_info.ordinal = ordinal;
                        }
                        schema.columns[ordinal++] = column_info;
                    }
                }

                // Sort columns by physical_pos (InnoDB storage order)
                // CRITICAL: InnoDB stores columns by physical_pos, NOT by "fixed-first then variable"!
                // The physical_pos field already reflects the actual storage order determined by InnoDB.
                // We must NOT reorder fixed/variable columns - just sort by physical_pos.
                if (!schema.columns.empty()) {
                    std::vector<std::pair<uint32_t, catalog::ColumnInfo>> cols_vec;
                    for (const auto& p : schema.columns) {
                        cols_vec.push_back(p);
                    }
                    // Sort by physical_pos only (this is the actual InnoDB storage order)
                    std::sort(cols_vec.begin(), cols_vec.end(),
                        [](const auto& a, const auto& b) {
                            return a.second.physical_pos < b.second.physical_pos;
                        });
                    schema.columns.clear();
                    uint32_t new_ordinal = 1;
                    for (const auto& p : cols_vec) {
                        if (g_verbose)                         std::cerr << "[DEBUG SDI SORT] Column '" << p.second.name << "' new_ordinal=" << new_ordinal
                                  << " physical_pos=" << p.second.physical_pos
                                  << " ordinal=" << p.second.ordinal
                                  << " type=" << p.second.mysql_type_id
                                  << " is_fixed=" << catalog::TableSchema::is_fixed_length(p.second) << "\n";
                        schema.columns[new_ordinal++] = p.second;
                    }
                }

                // Parse indexes
                if (table_json.contains("indexes")) {
                    for (const auto& idx : table_json["indexes"]) {
                        catalog::IndexInfo index_info;
                        index_info.name = idx.value("name", "");
                        index_info.is_primary = idx.value("is_primary", false);
                        index_info.is_unique = idx.value("is_unique", false);

                        if (idx.contains("columns")) {
                            for (const auto& col : idx["columns"]) {
                                index_info.columns.push_back(col.get<std::string>());
                            }
                        }

                        schema.indexes.push_back(index_info);
                    }
                }
            }
        } else if (json_obj.contains("schema")) {
            // Legacy format
            schema.table_name = json_obj["schema"].value("name", "");
        }
        if (json_obj.contains("table")) {
            // Fallback format
            auto table_json = json_obj["table"];
            schema.table_name = table_json.value("name", schema.table_name);

            // Parse columns
            if (table_json.contains("columns")) {
                uint32_t ordinal = 1;
                for (const auto& col : table_json["columns"]) {
                    catalog::ColumnInfo column_info;
                    column_info.name = col.value("name", "");
                    column_info.mysql_type_id = col.value("type", 0);
                    column_info.is_nullable = col.value("is_nullable", true); // SDI stores true when nullable
                        bool is_unsigned = col.value("is_unsigned", false);
                        column_info.is_signed = !is_unsigned;

                    if (col.contains("default_value")) {
                        column_info.default_value = col["default_value"].get<std::string>();
                    }

                    if (col.contains("length")) {
                        column_info.column_length = col["length"];
                    }

                    if (col.contains("charset_id")) {
                        column_info.charset_id = col["charset_id"];
                        column_info.collation_id = col.value("collation_id", 255);
                    }

                    // Skip InnoDB system columns (DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID)
                    // These are internal columns not part of user table schema
                    if (column_info.name == "DB_TRX_ID" || column_info.name == "DB_ROLL_PTR" || column_info.name == "DB_ROW_ID") {
                        continue;
                    }

                    schema.columns[ordinal++] = column_info;
                }
            }

            // Parse indexes
            if (table_json.contains("indexes")) {
                for (const auto& idx : table_json["indexes"]) {
                    catalog::IndexInfo index_info;
                    index_info.name = idx.value("name", "");
                    index_info.is_primary = idx.value("is_primary", false);
                    index_info.is_unique = idx.value("is_unique", false);

                    if (idx.contains("columns")) {
                        for (const auto& col : idx["columns"]) {
                            index_info.columns.push_back(col.get<std::string>());
                        }
                    }

                    schema.indexes.push_back(index_info);
                }
            }
        }
    } catch (...) {
        // SDI JSON 解析失败，返回空 schema
    }

    return schema;
}

} // namespace innodb
} // namespace dbrecovery
