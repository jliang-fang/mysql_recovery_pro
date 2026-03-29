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
    if (payload_len < 20) {
        throw std::runtime_error("SDI payload too small");
    }

    // Read header (type and id are reserved for future use)
    // uint32_t type = dbrecovery::innodb::read_le<uint32_t>(payload);
    // uint64_t id   = dbrecovery::innodb::read_le<uint64_t>(payload + 4);
    uint32_t uncompressed_len = dbrecovery::innodb::read_le<uint32_t>(payload + 12);
    uint32_t compressed_len   = dbrecovery::innodb::read_le<uint32_t>(payload + 16);

    if (compressed_len == 0 || compressed_len > payload_len - 20) {
        throw std::runtime_error("Invalid SDI compressed length");
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
    if (ret != Z_OK) throw std::runtime_error("Failed to init zlib");
    
    do {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_BUF_ERROR) {
            decompressed.resize(decompressed.size() * 2);
            strm.next_out = decompressed.data() + strm.total_out;
            strm.avail_out = static_cast<uInt>(decompressed.size() - strm.total_out);
        } else if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&strm);
            throw std::runtime_error("Decompression failed");
        }
    } while (ret != Z_STREAM_END);
    
    decompressed.resize(strm.total_out);
    inflateEnd(&strm);

    // Parse JSON
    try {
        auto json_obj = nlohmann::json::parse(decompressed.begin(), decompressed.end());
        
        catalog::TableSchema schema;
        
        if (json_obj.contains("schema")) {
            schema.table_name = json_obj["schema"].value("name", "");
        }
        if (json_obj.contains("table")) {
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
        
        return schema;
    } catch (...) {
        throw std::runtime_error("Failed to parse SDI JSON");
    }
}

} // namespace innodb
} // namespace dbrecovery
