#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "nlohmann/json.hpp"
#include <openssl/sha.h>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <vector>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace dbrecovery {
namespace catalog {


} // namespace catalog
} // namespace dbrecovery

namespace dbrecovery {
namespace innodb {

// Global verbose flag definition
bool g_verbose = false;

namespace {

/// Compute SHA-256 hex digest of a string (lower-case hex).
std::string sha256_hex(const std::string& value) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}

/// Shared JSON-to-schema parsing logic (no decompression, no exception swallowing).
catalog::TableSchema parse_sdi_json_to_schema(const nlohmann::json& json_obj) {
    catalog::TableSchema schema;

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
                            std::string type_str = col["type"].get<std::string>();
                            if (g_verbose)                                 std::cerr << "[DEBUG SDI] Column '" << column_info.name << "' type_str='" << type_str << "'\n";
                            if (type_str == "DECIMAL") column_info.mysql_type_id = 1;
                            else if (type_str == "TINY") column_info.mysql_type_id = 2;
                            else if (type_str == "SHORT") column_info.mysql_type_id = 3;
                            else if (type_str == "LONG") column_info.mysql_type_id = 4;
                            else if (type_str == "FLOAT") column_info.mysql_type_id = 5;
                            else if (type_str == "DOUBLE") column_info.mysql_type_id = 6;
                            else if (type_str == "TIMESTAMP") column_info.mysql_type_id = 8;
                            else if (type_str == "LONGLONG") column_info.mysql_type_id = 9;
                            else if (type_str == "INT24") column_info.mysql_type_id = 10;
                            else if (type_str == "DATE") column_info.mysql_type_id = 11;
                            else if (type_str == "TIME") column_info.mysql_type_id = 12;
                            else if (type_str == "DATETIME") column_info.mysql_type_id = 13;
                            else if (type_str == "YEAR") column_info.mysql_type_id = 14;
                            else if (type_str == "NEWDATE") column_info.mysql_type_id = 15;
                            else if (type_str == "VARCHAR") column_info.mysql_type_id = 16;
                            else if (type_str == "BIT") column_info.mysql_type_id = 17;
                            else if (type_str == "TIMESTAMP2") column_info.mysql_type_id = 18;
                            else if (type_str == "DATETIME2") column_info.mysql_type_id = 19;
                            else if (type_str == "TIME2") column_info.mysql_type_id = 20;
                            else if (type_str == "NEWDECIMAL") column_info.mysql_type_id = 21;
                            else if (type_str == "ENUM") column_info.mysql_type_id = 22;
                            else if (type_str == "SET") column_info.mysql_type_id = 23;
                            else if (type_str == "TINY_BLOB") column_info.mysql_type_id = 24;
                            else if (type_str == "MEDIUM_BLOB") column_info.mysql_type_id = 25;
                            else if (type_str == "LONG_BLOB") column_info.mysql_type_id = 26;
                            else if (type_str == "BLOB") column_info.mysql_type_id = 27;
                            else if (type_str == "VAR_STRING") column_info.mysql_type_id = 28;
                            else if (type_str == "STRING") column_info.mysql_type_id = 29;
                            else if (type_str == "GEOMETRY") column_info.mysql_type_id = 30;
                            else if (type_str == "JSON") column_info.mysql_type_id = 31;
                            else if (type_str == "VECTOR") column_info.mysql_type_id = 32;
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

                    column_info.is_nullable = col.value("is_nullable", true);
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
                    }
                    if (col.contains("collation_id")) {
                        column_info.collation_id = col["collation_id"];
                        if (column_info.collation_id == 63 && g_verbose) {
                            std::cerr << "[DEBUG SDI] Binary column detected: " << column_info.name
                                      << " collation_id=" << column_info.collation_id << "\n";
                        }
                    }
                    if (col.contains("numeric_precision")) {
                        column_info.numeric_precision = col["numeric_precision"];
                    }
                    if (col.contains("numeric_scale")) {
                        column_info.numeric_scale = col["numeric_scale"];
                    }
                    if (col.contains("precision") && column_info.numeric_precision == 0) {
                        column_info.numeric_precision = col["precision"];
                    }
                    if (col.contains("scale") && column_info.numeric_scale == 0) {
                        column_info.numeric_scale = col["scale"];
                    }
                    if (col.contains("datetime_precision")) {
                        if (!col["datetime_precision"].is_null()) {
                            column_info.datetime_precision = col["datetime_precision"].get<int>();
                        }
                    } else if (col.contains("datetimePrec")) {
                        column_info.datetime_precision = col["datetimePrec"].get<int>();
                    } else if (col.contains("decimals")) {
                        column_info.datetime_precision = col["decimals"].get<int>();
                    }
                    if (col.contains("datetime_precision_null")) {
                        if (col["datetime_precision_null"].get<int>() == 1) {
                            column_info.datetime_precision = 0;
                        }
                    }
                    if (col.contains("elements") && col["elements"].is_array()) {
                        static const std::string base64_chars =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        std::vector<std::string>* target_values =
                            (column_info.mysql_type_id == 23) ? &column_info.set_values : &column_info.enum_values;
                        for (const auto& elem : col["elements"]) {
                            if (elem.contains("name")) {
                                std::string encoded = elem["name"].get<std::string>();
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
                    if (col.contains("physical_pos")) {
                        column_info.physical_pos = col["physical_pos"].get<uint32_t>();
                    }
                    if (col.contains("ordinal_position")) {
                        column_info.ordinal = col["ordinal_position"].get<uint32_t>();
                    }
                    if (column_info.physical_pos == 0 && col.contains("ordinal_position")) {
                        column_info.physical_pos = col["ordinal_position"].get<uint32_t>();
                    }
                    if (column_info.name == "DB_TRX_ID" || column_info.name == "DB_ROLL_PTR" || column_info.name == "DB_ROW_ID") {
                        continue;
                    }
                    if (column_info.ordinal == 0) {
                        column_info.ordinal = ordinal;
                    }
                    schema.columns[ordinal++] = column_info;
                }
            }

            if (!schema.columns.empty()) {
                std::vector<std::pair<uint32_t, catalog::ColumnInfo>> cols_vec;
                for (const auto& p : schema.columns) {
                    cols_vec.push_back(p);
                }
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
                                  << " type=" << p.second.mysql_type_id << "\n";
                    schema.columns[new_ordinal++] = p.second;
                }
            }

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
        schema.table_name = json_obj["schema"].value("name", "");
    }
    if (json_obj.contains("table")) {
        auto table_json = json_obj["table"];
        schema.table_name = table_json.value("name", schema.table_name);
        if (table_json.contains("columns")) {
            uint32_t ordinal = 1;
            for (const auto& col : table_json["columns"]) {
                catalog::ColumnInfo column_info;
                column_info.name = col.value("name", "");
                column_info.mysql_type_id = col.value("type", 0);
                column_info.is_nullable = col.value("is_nullable", true);
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
                if (column_info.name == "DB_TRX_ID" || column_info.name == "DB_ROLL_PTR" || column_info.name == "DB_ROW_ID") {
                    continue;
                }
                schema.columns[ordinal++] = column_info;
            }
        }
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

    return schema;
}

} // anonymous namespace

catalog::TableSchema SDIManager::parse_sdi_record(const uint8_t* payload, size_t payload_len) {
    return parse_sdi_record_structured(payload, payload_len).schema;
}

SDIManager::SDISchemaParseResult SDIManager::parse_sdi_record_structured(const uint8_t* payload, size_t payload_len) {
    SDISchemaParseResult result;

    if (g_verbose)
        std::cerr << "[DEBUG] parse_sdi_record_structured called with payload_len=" << payload_len << "\n";

    if (!payload || payload_len < 20) {
        result.status = SDIParseStatus::EMPTY_INPUT;
        result.failure_reason = "payload too small for SDI header";
        return result;
    }

    // Use the standard extraction path for decompression and validation.
    auto extraction = extract_standard_payload(payload, payload_len);
    if (!extraction.success()) {
        // Map the extraction failure_reason to a structured status.
        if (extraction.failure_reason == "payload too small") {
            result.status = SDIParseStatus::EMPTY_INPUT;
        } else if (extraction.failure_reason == "declared output exceeds limit" ||
                   extraction.failure_reason == "declared compression ratio exceeds limit") {
            result.status = SDIParseStatus::DECOMPRESSION_LIMIT;
        } else if (extraction.failure_reason == "invalid JSON") {
            result.status = SDIParseStatus::INVALID_JSON;
        } else {
            result.status = SDIParseStatus::CORRUPTED_STREAM;
        }
        result.failure_reason = extraction.failure_reason;
        return result;
    }

    // Parse JSON and extract the table schema.
    try {
        auto json_obj = nlohmann::json::parse(extraction.json);
        result.schema = parse_sdi_json_to_schema(json_obj);
        result.schema.schema_hash = sha256_hex(extraction.json);
        result.schema.schema_source = "SDI";
        result.schema.trust_level = "AUTHORITATIVE";

        if (result.schema.table_name.empty() || result.schema.columns.empty()) {
            result.status = SDIParseStatus::MISSING_SCHEMA_OBJECT;
            result.failure_reason = "SDI JSON parsed but no table schema found";
            return result;
        }

        result.status = SDIParseStatus::SUCCESS;
        return result;
    } catch (const nlohmann::json::exception& e) {
        result.status = SDIParseStatus::INVALID_JSON;
        result.failure_reason = std::string("JSON parse error: ") + e.what();
        return result;
    } catch (const std::exception& e) {
        result.status = SDIParseStatus::CATCH_ALL_FAILURE;
        result.failure_reason = std::string("unexpected error: ") + e.what();
        return result;
    } catch (...) {
        result.status = SDIParseStatus::CATCH_ALL_FAILURE;
        result.failure_reason = "unknown error during SDI JSON parsing";
        return result;
    }
}

} // namespace innodb
} // namespace dbrecovery
