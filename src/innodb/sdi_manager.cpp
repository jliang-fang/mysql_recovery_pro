#include "dbrecovery/innodb/sdi_manager.hpp"
#include "nlohmann/json.hpp"
#include <stdexcept>
#include <iostream>

using json = nlohmann::json;

namespace dbrecovery {
namespace catalog {


} // namespace catalog
} // namespace dbrecovery

namespace dbrecovery {
namespace innodb {

catalog::TableSchema SDIManager::parse_sdi_record(const uint8_t* payload, size_t payload_len) {
    catalog::TableSchema schema;

    std::cerr << "[DEBUG] parse_sdi_record called with payload_len=" << payload_len << "\n";

    if (payload_len < 20) {
        std::cerr << "[DEBUG] payload_len < 20, returning empty schema\n";
        return schema;
    }

    // Read header (type and id are reserved for future use)
    // uint32_t type = dbrecovery::innodb::read_le<uint32_t>(payload);
    // uint64_t id   = dbrecovery::innodb::read_le<uint64_t>(payload + 4);
    uint32_t uncompressed_len = dbrecovery::innodb::read_le<uint32_t>(payload + 12);
    uint32_t compressed_len   = dbrecovery::innodb::read_le<uint32_t>(payload + 16);

    std::cerr << "[DEBUG] uncompressed_len=" << uncompressed_len << ", compressed_len=" << compressed_len << "\n";

    if (compressed_len == 0 || compressed_len > payload_len - 20) {
        std::cerr << "[DEBUG] compressed_len validation failed\n";
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
        std::cerr << "[DEBUG] inflateInit2 failed with ret=" << ret << "\n";
        return schema;
    }

    do {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_BUF_ERROR) {
            decompressed.resize(decompressed.size() * 2);
            strm.next_out = decompressed.data() + strm.total_out;
            strm.avail_out = static_cast<uInt>(decompressed.size() - strm.total_out);
        } else if (ret != Z_OK && ret != Z_STREAM_END) {
            std::cerr << "[DEBUG] inflate failed with ret=" << ret << "\n";
            inflateEnd(&strm);
            return schema;
        }
    } while (ret != Z_STREAM_END);

    decompressed.resize(strm.total_out);
    inflateEnd(&strm);

    std::cerr << "[DEBUG] Decompressed size=" << decompressed.size() << "\n";
    std::cerr << "[DEBUG] First 500 bytes: " << std::string(decompressed.begin(), decompressed.begin() + std::min((size_t)500, decompressed.size())) << "\n";

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
                    std::cerr << "[DEBUG] Found " << table_json["columns"].size() << " columns in SDI\n";
                    for (const auto& col : table_json["columns"]) {
                        catalog::ColumnInfo column_info;
                        column_info.name = col.value("name", "");
                        
                        // Handle type field - can be string or number
                        if (col.contains("type")) {
                            if (col["type"].is_string()) {
                                // Type is string like "LONGLONG", "VARCHAR", etc.
                                std::string type_str = col["type"].get<std::string>();
                                // Map type string to MySQL type ID
                                if (type_str == "LONGLONG") column_info.mysql_type_id = 8;  // MYSQL_TYPE_LONGLONG
                                else if (type_str == "VARCHAR") column_info.mysql_type_id = 15;  // MYSQL_TYPE_VARCHAR
                                else if (type_str == "CHAR") column_info.mysql_type_id = 254;  // MYSQL_TYPE_STRING
                                else if (type_str == "TINYINT") column_info.mysql_type_id = 1;  // MYSQL_TYPE_TINY
                                else if (type_str == "INT") column_info.mysql_type_id = 3;  // MYSQL_TYPE_LONG
                                else if (type_str == "TEXT") column_info.mysql_type_id = 252;  // MYSQL_TYPE_TINY_BLOB
                                else if (type_str == "JSON") column_info.mysql_type_id = 245;  // MYSQL_TYPE_JSON
                                else if (type_str == "LONGBLOB") column_info.mysql_type_id = 252;  // MYSQL_TYPE_BLOB
                                else if (type_str == "DATETIME") column_info.mysql_type_id = 12;  // MYSQL_TYPE_DATETIME
                                else if (type_str == "TIMESTAMP") column_info.mysql_type_id = 7;  // MYSQL_TYPE_TIMESTAMP
                                else if (type_str == "DATE") column_info.mysql_type_id = 10;  // MYSQL_TYPE_DATE
                                else if (type_str == "TIME") column_info.mysql_type_id = 11;  // MYSQL_TYPE_TIME
                                else if (type_str == "FLOAT") column_info.mysql_type_id = 4;  // MYSQL_TYPE_FLOAT
                                else if (type_str == "DOUBLE") column_info.mysql_type_id = 5;  // MYSQL_TYPE_DOUBLE
                                else if (type_str == "DECIMAL") column_info.mysql_type_id = 3;  // MYSQL_TYPE_NEWDECIMAL
                                else column_info.mysql_type_id = 0;
                            } else {
                                column_info.mysql_type_id = col.value("type", 0);
                            }
                        }
                        
                        column_info.is_nullable = !col.value("is_nullable", true); // Invert

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

                        if (col.contains("precision") && col.contains("scale")) {
                            column_info.numeric_precision = col["precision"];
                            column_info.numeric_scale = col["scale"];
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
                    column_info.is_nullable = !col.value("is_nullable", true); // Invert

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
