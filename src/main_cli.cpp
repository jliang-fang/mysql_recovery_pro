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

// 辅助函数：从 SDI 页解析表结构
std::optional<dbrecovery::catalog::TableSchema> parse_schema_from_sdi(
    const std::string& ibd_path, uint64_t table_id) {

    dbrecovery::innodb::InnoDBFileReader reader(ibd_path);
    if (!reader.is_valid()) {
        std::cerr << "[ERROR] Failed to open file for SDI parsing: " << ibd_path << "\n";
        return std::nullopt;
    }

    uint32_t total_pages = reader.get_total_pages();

    // 遍历所有页查找 SDI 页
    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        auto page_data = reader.read_page_raw(page_no);
        if (page_data.empty()) continue;

        // 检查页类型是否为 SDI (17854 = 0x45BE)
        // InnoDB FIL header fields are stored in BIG-ENDIAN format
        uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(
            page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

        if (page_type == dbrecovery::innodb::FIL_PAGE_SDI) {
            // 提取 SDI payload
            const uint8_t* sdi_payload = page_data.data() + 38;
            if (page_data.size() < 38 + 20) continue;

            // 解析 SDI
            auto json_opt = dbrecovery::innodb::SDIManager::extract_sdi_json(
                sdi_payload, page_data.size() - 38);

            if (json_opt.has_value()) {
                try {
                    auto json = nlohmann::json::parse(json_opt.value());
                    dbrecovery::catalog::TableSchema schema;

                    if (schema.from_json(json)) {
                        std::cout << "[INFO] Successfully parsed schema from SDI: "
                                  << schema.table_name << "\n";
                        return schema;
                    }
                } catch (const nlohmann::json::parse_error& e) {
                    // SDI JSON 解析失败，继续处理其他页
                }
            }
        }
    }

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
