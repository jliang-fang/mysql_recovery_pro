/**
 * @file recovery_cli.cpp
 * @brief RecoveryCLI helper implementations
 *
 * Contains the helper functions used by RecoveryCLI that were previously
 * in main_cli.cpp: parse_schema_from_sdi(), create_default_schema(), to_lower().
 */

#include "dbrecovery/tools/recovery_cli.hpp"
#include "dbrecovery/innodb.h"
#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/sdi_extractor.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/utils/cli_parser.hpp"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstring>
#include <zlib.h>

namespace dbrecovery {
namespace tools {

std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::optional<catalog::TableSchema> parse_schema_from_sdi(
    const std::string& ibd_path, uint64_t table_id) {

    innodb::InnoDBFileReader reader(ibd_path);
    if (!reader.is_valid()) {
        std::cerr << "[ERROR] Failed to open file for SDI parsing: " << ibd_path << "\n";
        return std::nullopt;
    }

    uint32_t total_pages = reader.get_total_pages();
    uint32_t page_size = reader.get_page_size();

    for (uint32_t page_no = 0; page_no < std::min(total_pages, static_cast<uint32_t>(10)); ++page_no) {
        auto page_data = reader.read_page_raw(page_no);
        if (page_data.empty()) continue;

        uint16_t page_type = innodb::read_be<uint16_t>(
            page_data.data(), innodb::FIL_PAGE_TYPE_ABS);

        if (page_type == innodb::FIL_PAGE_SDI ||
            page_type == innodb::FIL_PAGE_SDI_ZIP) {

            if (page_size < 100) continue;

            int16_t slot_offset = static_cast<int16_t>(
                (static_cast<uint16_t>(page_data[97]) << 8) |
                static_cast<uint16_t>(page_data[98]));

            uint32_t record_offset = static_cast<uint32_t>(slot_offset + 99);

            std::vector<std::pair<uint32_t, std::string>> sdi_payloads;

            // Approach 1: Slot-based offset
            if (record_offset > 0 && record_offset < page_size - 50) {
                uint32_t offset1 = record_offset;
                if (offset1 + 50 < page_size) {
                    uint32_t uncompressed_len = innodb::read_be<uint32_t>(
                        page_data.data(), offset1 + 25);
                    uint32_t compressed_len = innodb::read_be<uint32_t>(
                        page_data.data(), offset1 + 29);

                    if (compressed_len > 0 && compressed_len < page_size &&
                        offset1 + 33 + compressed_len <= page_size) {
                        const uint8_t* compressed_data = page_data.data() + offset1 + 33;

                        z_stream strm = {};
                        strm.next_in = const_cast<Bytef*>(compressed_data);
                        strm.avail_in = compressed_len;

                        std::vector<uint8_t> decompressed(
                            uncompressed_len > 0 ? uncompressed_len : compressed_len * 4);
                        strm.next_out = decompressed.data();
                        strm.avail_out = decompressed.size();

                        int ret = inflateInit(&strm);
                        if (ret == Z_OK) {
                            do {
                                ret = inflate(&strm, Z_NO_FLUSH);
                                if (ret == Z_BUF_ERROR && strm.avail_out == 0) {
                                    decompressed.resize(decompressed.size() * 2);
                                    strm.next_out = decompressed.data() + strm.total_out;
                                    strm.avail_out = decompressed.size() - strm.total_out;
                                }
                            } while (ret == Z_OK);

                            if (ret == Z_STREAM_END) {
                                decompressed.resize(strm.total_out);
                                std::string json_str(
                                    reinterpret_cast<const char*>(decompressed.data()),
                                    decompressed.size());
                                sdi_payloads.push_back({offset1, json_str});
                            }
                            inflateEnd(&strm);
                        }
                    }
                }
            }

            // Approach 2: Simple offset for uncompressed SDI (0x45BE)
            if (page_type == innodb::FIL_PAGE_SDI) {
                const uint8_t* sdi_payload = page_data.data() + 38;
                if (page_data.size() >= 38 + 20) {
                    auto json_opt = innodb::SDIManager::extract_sdi_json(
                        sdi_payload, page_data.size() - 38);
                    if (json_opt.has_value()) {
                        sdi_payloads.push_back({38, json_opt.value()});
                    }
                }
            }

            for (const auto& [offset, json_str] : sdi_payloads) {
                try {
                    auto json = nlohmann::json::parse(json_str);
                    catalog::TableSchema schema;

                    if (json.contains("dd_object")) {
                        if (schema.from_json(json)) {
                            std::cout << "[INFO] Successfully parsed schema from SDI (offset "
                                      << offset << "): "
                                      << schema.table_name << " (" << schema.columns.size()
                                      << " columns)\n";
                            return schema;
                        }
                    } else if (json.contains("dictionary")) {
                        auto dict = json["dictionary"];
                        if (dict.contains("table") && schema.from_json(dict["table"])) {
                            std::cout << "[INFO] Successfully parsed schema from SDI dictionary: "
                                      << schema.table_name << "\n";
                            return schema;
                        }
                    } else {
                        if (schema.from_json(json)) {
                            std::cout << "[INFO] Successfully parsed schema from SDI: "
                                      << schema.table_name << "\n";
                            return schema;
                        }
                    }
                } catch (const nlohmann::json::parse_error& e) {
                    std::cerr << "[WARN] JSON parse error at offset " << offset << ": "
                              << e.what() << "\n";
                }
            }
        }
    }

    std::cerr << "[WARN] No valid SDI schema found in " << ibd_path << "\n";
    return std::nullopt;
}

catalog::TableSchema create_default_schema() {
    catalog::TableSchema schema;
    schema.table_name = "recovered_table";
    schema.row_format = 2;  // DYNAMIC
    schema.page_size = 16384;

    catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = innodb::mysql_types::LONGLONG;
    col1.is_nullable = false;
    col1.column_length = 8;
    schema.columns[1] = col1;

    catalog::ColumnInfo col2;
    col2.name = "name";
    col2.mysql_type_id = innodb::mysql_types::VARCHAR;
    col2.is_nullable = true;
    col2.column_length = 255;
    schema.columns[2] = col2;

    catalog::ColumnInfo col3;
    col3.name = "email";
    col3.mysql_type_id = innodb::mysql_types::VARCHAR;
    col3.is_nullable = true;
    col3.column_length = 255;
    schema.columns[3] = col3;

    catalog::ColumnInfo col4;
    col4.name = "created_at";
    col4.mysql_type_id = innodb::mysql_types::DATETIME;
    col4.is_nullable = true;
    schema.columns[4] = col4;

    return schema;
}

} // namespace tools
} // namespace dbrecovery