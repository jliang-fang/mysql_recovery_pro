#include <iostream>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <iomanip>
#include <chrono>

#include "dbrecovery/innodb.h"
#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/page_chain_rebuilder.hpp"
#include "dbrecovery/innodb/sdi_extractor.hpp"
#include "dbrecovery/innodb/btree_node_parser.hpp"
#include "dbrecovery/innodb/record_chain_rebuilder.hpp"
#include "dbrecovery/innodb/blob_chain_rebuilder.hpp"
#include "dbrecovery/tools/recovery_cli.hpp"
#include "dbrecovery/utils/cli_parser.hpp"
#include "dbrecovery/recovery/engine.hpp"
#include "dbrecovery/recovery/rmrf_recovery.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace fs = std::filesystem;

// 辅助函数：转换字符串为小写
std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

// 辅助函数：从 SDI 页解析表结构 (支持压缩 SDI 页 0x45BD)
//   offset = struct.unpack('>h',tdata[97:99])[0] + 99
//   dunzip_len,dzip_len = struct.unpack('>LL',tdata[offset+33-8:offset+33])
//   unzbdata = zlib.decompress(tdata[offset+33:offset+33+dzip_len])
std::optional<dbrecovery::catalog::TableSchema> parse_schema_from_sdi(
    const std::string& ibd_path, uint64_t table_id) {

    dbrecovery::innodb::InnoDBFileReader reader(ibd_path);
    if (!reader.is_valid()) {
        std::cerr << "[ERROR] Failed to open file for SDI parsing: " << ibd_path << "\n";
        return std::nullopt;
    }

    uint32_t total_pages = reader.get_total_pages();
    uint32_t page_size = reader.get_page_size();

    // 遍历所有页查找 SDI 页
    // MySQL 8.0+ SDI 页通常在 page_no=3 和 page_no=4
    for (uint32_t page_no = 0; page_no < std::min(total_pages, static_cast<uint32_t>(10)); ++page_no) {
        auto page_data = reader.read_page_raw(page_no);
        if (page_data.empty()) continue;

        // 检查页类型
        // FIL header fields are stored in BIG-ENDIAN format
        uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(
            page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

        // 支持 FIL_PAGE_SDI (0x45BE) 和 FIL_PAGE_SDI_ZIP (0x45BD)
        if (page_type == dbrecovery::innodb::FIL_PAGE_SDI ||
            page_type == dbrecovery::innodb::FIL_PAGE_SDI_ZIP) {

            // Compressed SDI pages: slot at offset 97-99 points to first record
            if (page_size < 100) continue;

            // Read the first slot from page directory (at page_end - 2)
            // Page directory starts at page_size - (n_dir_slots * 2)
            // First slot points to the first user record (infimum is ignored)

            // For SDI pages, the record offset calculation:
            // Read slot offset from byte 97-99 (big-endian signed short)
            int16_t slot_offset = static_cast<int16_t>(
                (static_cast<uint16_t>(page_data[97]) << 8) |
                static_cast<uint16_t>(page_data[98]));

            uint32_t record_offset = static_cast<uint32_t>(slot_offset + 99);

            // Alternative: try simpler offset calculation
            // The SDI payload often starts right after the FIL header + page header
            // FIL header = 38 bytes, page header = 56 bytes (for SDI page)

            // Try both approaches:
            std::vector<std::pair<uint32_t, std::string>> sdi_payloads;

            // Approach 1: Slot-based offset
            if (record_offset > 0 && record_offset < page_size - 50) {
                // Read SDI header: type(4) + id(8) + uncompressed_len(4) + compressed_len(4) = 20 bytes
                uint32_t offset1 = record_offset;
                if (offset1 + 50 < page_size) {
                    // Read compressed/uncompressed lengths
                    uint32_t uncompressed_len = dbrecovery::innodb::read_be<uint32_t>(
                        page_data.data(), offset1 + 25);
                    uint32_t compressed_len = dbrecovery::innodb::read_be<uint32_t>(
                        page_data.data(), offset1 + 29);

                    if (compressed_len > 0 && compressed_len < page_size &&
                        offset1 + 33 + compressed_len <= page_size) {
                        // Decompress using zlib
                        // MySQL SDI uses standard zlib compression (with header), not raw deflate
                        const uint8_t* compressed_data = page_data.data() + offset1 + 33;

                        z_stream strm = {};
                        strm.next_in = const_cast<Bytef*>(compressed_data);
                        strm.avail_in = compressed_len;

                        std::vector<uint8_t> decompressed(uncompressed_len > 0 ? uncompressed_len : compressed_len * 4);
                        strm.next_out = decompressed.data();
                        strm.avail_out = decompressed.size();

                        // Use default window bits (15) for standard zlib format
                        // This handles data compressed with zlib.compress() in Python
                        int ret = inflateInit(&strm);  // Standard zlib (not raw deflate)
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
                                std::string json_str(reinterpret_cast<const char*>(decompressed.data()),
                                                      decompressed.size());
                                sdi_payloads.push_back({offset1, json_str});
                            }
                            inflateEnd(&strm);
                        }
                    }
                }
            }

            // Approach 2: Simple FIL header + fixed offset (for uncompressed SDI 0x45BE)
            if (page_type == dbrecovery::innodb::FIL_PAGE_SDI) {
                const uint8_t* sdi_payload = page_data.data() + 38;
                if (page_data.size() >= 38 + 20) {
                    auto json_opt = dbrecovery::innodb::SDIManager::extract_sdi_json(
                        sdi_payload, page_data.size() - 38);
                    if (json_opt.has_value()) {
                        sdi_payloads.push_back({38, json_opt.value()});
                    }
                }
            }

            // Try to parse each SDI payload
            for (const auto& [offset, json_str] : sdi_payloads) {
                try {
                    auto json = nlohmann::json::parse(json_str);

                    // MySQL 8.0+ SDI format: {"dd_object": {...}}
                    dbrecovery::catalog::TableSchema schema;

                    // Try different JSON formats
                    if (json.contains("dd_object")) {
                        // Standard MySQL 8.0+ SDI format
                        if (schema.from_json(json)) {
                            std::cout << "[INFO] Successfully parsed schema from SDI (offset " << offset << "): "
                                      << schema.table_name << " (" << schema.columns.size() << " columns)\n";
                            return schema;
                        }
                    } else if (json.contains("dictionary")) {
                        // Alternative format
                        auto dict = json["dictionary"];
                        if (dict.contains("table") && schema.from_json(dict["table"])) {
                            std::cout << "[INFO] Successfully parsed schema from SDI dictionary: "
                                      << schema.table_name << "\n";
                            return schema;
                        }
                    } else {
                        // Try direct parsing
                        if (schema.from_json(json)) {
                            std::cout << "[INFO] Successfully parsed schema from SDI: "
                                      << schema.table_name << "\n";
                            return schema;
                        }
                    }
                } catch (const nlohmann::json::parse_error& e) {
                    std::cerr << "[WARN] JSON parse error at offset " << offset << ": " << e.what() << "\n";
                }
            }
        }
    }

    std::cerr << "[WARN] No valid SDI schema found in " << ibd_path << "\n";
    return std::nullopt;
}

// 辅助函数：创建默认表结构
dbrecovery::catalog::TableSchema create_default_schema() {
    dbrecovery::catalog::TableSchema schema;
    schema.table_name = "recovered_table";
    schema.row_format = 2;  // DYNAMIC
    schema.page_size = 16384;

    // 添加默认列
    dbrecovery::catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = dbrecovery::innodb::mysql_types::LONGLONG;
    col1.is_nullable = false;
    col1.column_length = 8;
    schema.columns[1] = col1;

    dbrecovery::catalog::ColumnInfo col2;
    col2.name = "name";
    col2.mysql_type_id = dbrecovery::innodb::mysql_types::VARCHAR;
    col2.is_nullable = true;
    col2.column_length = 255;
    schema.columns[2] = col2;

    dbrecovery::catalog::ColumnInfo col3;
    col3.name = "email";
    col3.mysql_type_id = dbrecovery::innodb::mysql_types::VARCHAR;
    col3.is_nullable = true;
    col3.column_length = 255;
    schema.columns[3] = col3;

    dbrecovery::catalog::ColumnInfo col4;
    col4.name = "created_at";
    col4.mysql_type_id = dbrecovery::innodb::mysql_types::DATETIME;
    col4.is_nullable = true;
    schema.columns[4] = col4;

    return schema;
}

int main(int argc, char** argv) {
    dbrecovery::tools::RecoveryCLI cli(argc, argv);
    return cli.run(argc, argv);
}
