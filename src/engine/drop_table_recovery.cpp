/**
 * @file drop_table_recovery.cpp
 * @brief DROP TABLE Recovery Implementation
 */

#include "dbrecovery/engine/drop_table_recovery.hpp"
#include "dbrecovery/engine/signature_scraper.hpp"
#include "dbrecovery/engine/system_table_sdi.hpp"
#include "dbrecovery/utils/innodb_checksum.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <set>
#include <nlohmann/json.hpp>

namespace dbrecovery {
namespace engine {

// Import RedundantHeader for convenience
using innodb::RedundantHeader;

// ==================== Helper Functions ====================

namespace {

// Read big-endian uint32
uint32_t read_be_u32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

// Read big-endian uint64
uint64_t read_be_u64(const uint8_t* data) {
    return (static_cast<uint64_t>(data[0]) << 56) |
           (static_cast<uint64_t>(data[1]) << 48) |
           (static_cast<uint64_t>(data[2]) << 40) |
           (static_cast<uint64_t>(data[3]) << 32) |
           (static_cast<uint64_t>(data[4]) << 24) |
           (static_cast<uint64_t>(data[5]) << 16) |
           (static_cast<uint64_t>(data[6]) << 8) |
           static_cast<uint64_t>(data[7]);
}

// Check if page is an INDEX page
bool is_index_page_type(const uint8_t* data) {
    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    return page_type == 0x45BF;
}

// Extract index_id from INDEX page
uint64_t extract_index_id_from_page(const uint8_t* data) {
    return read_be_u64(data + 66);
}

} // anonymous namespace

// ==================== System Tablespace Scanner ====================

SystemTablespaceScanner::SystemTablespaceScanner(const std::string& path, uint32_t page_size)
    : path_(path), page_size_(page_size) {
}

std::string SystemTablespaceScanner::detect_mysql_version() {
    if (!mysql_version_.empty()) {
        return mysql_version_;
    }

    std::string filename = path_;
    size_t last_slash = filename.find_last_of('/');
    if (last_slash != std::string::npos) {
        filename = filename.substr(last_slash + 1);
    }

    if (filename == "mysql.ibd") {
        mysql_version_ = "8.0";
    } else if (filename == "ibdata1" || filename.find("ibdata") != std::string::npos) {
        mysql_version_ = "5.7";
    } else {
        mysql_version_ = "UNKNOWN";
    }

    return mysql_version_;
}

bool SystemTablespaceScanner::is_mysql80_format() {
    return detect_mysql_version() == "8.0";
}

std::vector<SysTableEntry> SystemTablespaceScanner::get_all_tables() {
    if (tables_.empty()) {
        scan_deleted_tables();
    }
    return tables_;
}

std::vector<SysColumnEntry> SystemTablespaceScanner::get_columns(uint64_t table_id) {
    if (columns_.empty()) {
        scan_deleted_tables();
    }

    std::vector<SysColumnEntry> result;
    for (const auto& col : columns_) {
        if (col.table_id == table_id) {
            result.push_back(col);
        }
    }

    std::sort(result.begin(), result.end(), [](const SysColumnEntry& a, const SysColumnEntry& b) {
        return a.pos < b.pos;
    });

    return result;
}

std::vector<SysIndexEntry> SystemTablespaceScanner::get_indexes(uint64_t table_id) {
    if (indexes_.empty()) {
        scan_deleted_tables();
    }

    std::vector<SysIndexEntry> result;
    for (const auto& idx : indexes_) {
        if (idx.table_id == table_id) {
            result.push_back(idx);
        }
    }

    return result;
}

std::vector<RecoveredTableMetadata> SystemTablespaceScanner::scan_deleted_tables() {
    std::vector<RecoveredTableMetadata> recovered_tables;

    // Open the system tablespace file
    std::ifstream file(path_, std::ios::binary);
    if (!file) {
        error_ = "Cannot open system tablespace: " + path_;
        return recovered_tables;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    uint64_t num_pages = file_size / page_size_;
    if (num_pages == 0) {
        error_ = "File too small to be a valid tablespace";
        return recovered_tables;
    }

    // Allocate buffer for reading pages
    std::vector<uint8_t> page_buffer(page_size_);

    if (is_mysql80_format()) {
        // MySQL 8.0+: Parse SDI pages from mysql.ibd to get DD table schemas
        // Then scan INDEX pages for deleted records in DD tables

        // Declare shared maps at this scope so they're accessible in all branches
        std::map<uint32_t, std::string> space_id_to_table;     // Map space_id -> table name
        std::map<uint32_t, uint32_t> space_id_to_table_id;     // Map space_id -> table_id
        std::map<std::string, uint64_t> table_to_index_id;
        std::map<std::string, uint32_t> table_to_space_id;

        // Step 1: Parse SDI pages to get DD table index IDs
        // In MySQL 8.0, SDI pages can be anywhere in the file, not just pages 3-4
        std::map<std::string, uint64_t> dd_table_index_ids;
        std::map<uint64_t, std::string> index_id_to_table_name;

        // Parse SDI pages to extract DD table metadata
        // Scan all pages looking for SDI_ZIP type (0x45BD)
        for (uint64_t page_no = 0; page_no < num_pages; ++page_no) {
            file.seekg(page_no * page_size_, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);
            if (!file) break;

            // Check for SDI page type
            uint16_t page_type = (static_cast<uint16_t>(page_buffer[24]) << 8) | page_buffer[25];

            if (page_type == 0x45BD) {  // FIL_PAGE_SDI_ZIP (compressed SDI) — shared with BLOB
                // 0x45BD is ambiguous: FIL_PAGE_SDI_ZIP == FIL_PAGE_TYPE_BLOB.
                // Only the SDI signature at offset 64-71 establishes SDI identity.
                // BLOB pages lack this signature and must not be parsed as SDI.
                constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
                if (page_size_ < 72 || std::memcmp(page_buffer.data() + 64, SDI_SIG, 8) != 0) {
                    continue;
                }
                // Use SDIManager::extract_zip_record() which reads BE lengths,
                // constructs the standard LE SDI header, and decompresses through
                // the bounded manager (16 MiB / 20:1 limits).
                for (uint32_t record_offset : {127u, 403u, 679u}) {
                    auto result = dbrecovery::innodb::SDIManager::extract_zip_record(
                        page_buffer.data(), page_size_, record_offset);
                    if (!result.success()) continue;

                    try {
                        auto json = nlohmann::json::parse(result.json);

                        // Check if this is a DD table (mysql.tables, mysql.columns, etc.)
                        if (json.contains("dd_object")) {
                            auto& dd_obj = json["dd_object"];
                            std::string schema_ref = dd_obj.value("schema_ref", "");
                            std::string table_name = dd_obj.value("name", "");

                            if (schema_ref == "mysql" && dd_obj.contains("indexes")) {
                                // Get the PRIMARY index ID from se_private_data
                                for (auto& idx : dd_obj["indexes"]) {
                                    if (idx.value("name", "") == "PRIMARY" && idx.contains("se_private_data")) {
                                        std::string private_data = idx["se_private_data"].get<std::string>();
                                        // Parse id=xxx from se_private_data
                                        size_t id_pos = private_data.find("id=");
                                        if (id_pos != std::string::npos) {
                                            uint64_t idx_id = std::stoull(private_data.substr(id_pos + 3));
                                            dd_table_index_ids[table_name] = idx_id;
                                            index_id_to_table_name[idx_id] = table_name;
                                        }
                                    }
                                }
                            }
                        }
                    } catch (const nlohmann::json::parse_error&) {
                        // Ignore parse errors
                    }
                }
            }
        }

        if (dbrecovery::innodb::g_verbose) 
        std::cerr << "[DEBUG] Found " << dd_table_index_ids.size() << " DD table index IDs" << std::endl;
        for (const auto& [name, id] : dd_table_index_ids) {
            if (dbrecovery::innodb::g_verbose)             std::cerr << "[DEBUG]   " << name << " -> index_id " << id << std::endl;
        }

        // Step 2: If SDI parsing didn't find DD tables, try direct string search
        // This is needed for MySQL 8.0+ where DD tables are stored differently
        if (dd_table_index_ids.empty()) {
            if (dbrecovery::innodb::g_verbose)             std::cerr << "[DEBUG] No DD table index IDs found via SDI, trying direct string search..." << std::endl;

            // Scan all INDEX pages for table metadata
            // We're looking for:
            // 1. mysql.tables DD records: "schema/table" with "id=N" (space_id)
            // 2. mysql.indexes DD records: "PRIMARY" with "flags=0;id=N;root=M;space_id=X;table_id=Y"

            for (uint64_t page_no = 0; page_no < num_pages; ++page_no) {
                file.seekg(page_no * page_size_, std::ios::beg);
                file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);
                if (!file) break;

                if (!is_index_page_type(page_buffer.data())) continue;

                // Convert page buffer to string for searching
                std::string page_content(reinterpret_cast<const char*>(page_buffer.data()), page_size_);

                // Look for se_private_data pattern: "flags=0;id=N;root=M;space_id=X;table_id=Y"
                // This is in mysql.indexes DD records
                size_t pos = 0;
                while ((pos = page_content.find("flags=0;id=", pos)) != std::string::npos) {
                    // Parse the id value
                    size_t id_start = pos + 11;  // Skip "flags=0;id="
                    std::string id_str;
                    while (id_start < page_content.size() && isdigit(page_content[id_start])) {
                        id_str += page_content[id_start++];
                    }

                    // Look for space_id in the same record
                    size_t space_id_pos = page_content.find("space_id=", pos);
                    if (space_id_pos != std::string::npos && space_id_pos < pos + 100) {
                        size_t sp_id_start = space_id_pos + 9;  // Skip "space_id="
                        std::string sp_id_str;
                        while (sp_id_start < page_content.size() && isdigit(page_content[sp_id_start])) {
                            sp_id_str += page_content[sp_id_start++];
                        }

                        // Also parse table_id from se_private_data
                        uint32_t table_id = 0;
                        size_t table_id_pos = page_content.find("table_id=", pos);
                        if (table_id_pos != std::string::npos && table_id_pos < pos + 100) {
                            size_t tid_start = table_id_pos + 9;  // Skip "table_id="
                            std::string tid_str;
                            while (tid_start < page_content.size() && isdigit(page_content[tid_start])) {
                                tid_str += page_content[tid_start++];
                            }
                            if (!tid_str.empty()) {
                                table_id = std::stoul(tid_str);
                            }
                        }

                        if (!id_str.empty() && !sp_id_str.empty()) {
                            uint64_t idx_id = std::stoull(id_str);
                            uint32_t sp_id = std::stoul(sp_id_str);

                            // Check if this is a PRIMARY index (look for "PRIMARY" before the flags)
                            size_t primary_pos = page_content.rfind("PRIMARY", pos);
                            if (primary_pos != std::string::npos && primary_pos > pos - 50) {
                                // This is a PRIMARY index record
                                if (dbrecovery::innodb::g_verbose)                                 std::cerr << "[DEBUG] Found PRIMARY index: id=" << idx_id
                                          << ", space_id=" << sp_id << ", table_id=" << table_id << std::endl;

                                // Map space_id to index_id
                                if (sp_id > 3) {  // Skip system tablespace IDs
                                    // We'll map this to a table name later using space_id
                                    space_id_to_table[sp_id] = "";  // Placeholder
                                    table_to_index_id[std::to_string(sp_id)] = idx_id;
                                    if (table_id > 0) {
                                        space_id_to_table_id[sp_id] = table_id;
                                    }
                                }
                            }
                        }
                    }
                    pos++;
                }

                // Dynamically search for all user tables (not hardcoded list)
                // Look for pattern: schema/table_name followed by "id=N" for space_id
                // MySQL 8.0 DD format: "./schema/table_name.ibd id=N;"
                size_t tbl_pos = 0;
                while ((tbl_pos = page_content.find(".ibd", tbl_pos)) != std::string::npos) {
                    // Look backward for "/" to find schema/table pattern
                    if (tbl_pos > 50) {
                        size_t slash_pos = page_content.rfind('/', tbl_pos);
                        if (slash_pos != std::string::npos && slash_pos > tbl_pos - 100) {
                            // Find the start of table name (look for "/" before schema name)
                            size_t prev_slash = page_content.rfind('/', slash_pos - 1);
                            size_t name_start = (prev_slash != std::string::npos && prev_slash > slash_pos - 50)
                                ? prev_slash + 1 : slash_pos - 50;

                            // Extract schema/table name
                            std::string potential_table = page_content.substr(name_start, tbl_pos - name_start);

                            // Validate it looks like a table name (has "/" and no special chars)
                            size_t table_slash = potential_table.find('/');
                            if (table_slash != std::string::npos &&
                                potential_table.find("mysql/") == std::string::npos &&
                                potential_table.find("sys/") == std::string::npos &&
                                potential_table.find("performance_schema/") == std::string::npos &&
                                potential_table.find("information_schema/") == std::string::npos) {

                                // Look for "id=N" after ".ibd"
                                size_t id_search_start = tbl_pos + 4;
                                size_t id_search_end = std::min(id_search_start + 100, page_content.size());
                                std::string after_ibd = page_content.substr(id_search_start, id_search_end - id_search_start);

                                size_t id_pos = after_ibd.find("id=");
                                if (id_pos != std::string::npos) {
                                    size_t id_start = id_pos + 3;
                                    std::string id_str;
                                    while (id_start < after_ibd.size() && isdigit(after_ibd[id_start])) {
                                        id_str += after_ibd[id_start++];
                                    }
                                    if (!id_str.empty()) {
                                        uint32_t space_id = std::stoul(id_str);
                                        table_to_space_id[potential_table] = space_id;
                                        space_id_to_table[space_id] = potential_table;
                                        if (dbrecovery::innodb::g_verbose)                                         std::cerr << "[DEBUG] Found table: " << potential_table
                                                  << " -> space_id=" << space_id << std::endl;
                                    }
                                }
                            }
                        }
                    }
                    tbl_pos++;
                }
            }

            if (dbrecovery::innodb::g_verbose) 
            std::cerr << "[DEBUG] Found " << table_to_index_id.size() << " PRIMARY indexes" << std::endl;
            if (dbrecovery::innodb::g_verbose)             std::cerr << "[DEBUG] Found " << table_to_space_id.size() << " tables with space_id" << std::endl;

            // Create recovered table entries by matching space_id
            std::set<std::string> found_tables;
            for (const auto& [name, sp_id] : table_to_space_id) {
                found_tables.insert(name);
            }

            for (const auto& name : found_tables) {
                size_t slash_pos = name.find('/');
                std::string schema_name = name.substr(0, slash_pos);
                std::string table_name = name.substr(slash_pos + 1);

                RecoveredTableMetadata metadata;
                metadata.table_id = 0;
                metadata.schema_name = schema_name;
                metadata.table_name = table_name;

                // Get space_id if found
                if (table_to_space_id.count(name)) {
                    metadata.space_id = table_to_space_id[name];

                    // Look up index_id by space_id
                    std::string sp_id_key = std::to_string(metadata.space_id);
                    if (table_to_index_id.count(sp_id_key)) {
                        metadata.primary_index_id = table_to_index_id[sp_id_key];
                        if (dbrecovery::innodb::g_verbose)                         std::cerr << "[DEBUG] Mapped " << name << " (space_id=" << metadata.space_id
                                  << ") to index_id=" << metadata.primary_index_id << std::endl;
                    }
                }

                metadata.recovered_from_deleted = true;
                metadata.confidence = 0.9;

                recovered_tables.push_back(metadata);
            }

            // Step 2.5: Parse column records from mysql.columns DD records
            // Build dynamic known_table_ids from space_id_to_table and space_id_to_table_id
            std::map<uint32_t, std::string> known_table_ids;
            for (const auto& [sp_id, table_name] : space_id_to_table) {
                if (!table_name.empty()) {
                    auto it = space_id_to_table_id.find(sp_id);
                    if (it != space_id_to_table_id.end()) {
                        known_table_ids[it->second] = table_name;
                        if (dbrecovery::innodb::g_verbose)                         std::cerr << "[DEBUG] Mapped table_id=" << it->second
                                  << " -> " << table_name << " (space_id=" << sp_id << ")" << std::endl;
                    }
                }
            }

            // We need to scan pages for column definitions: column_name...table_id=N...type_info
            if (dbrecovery::innodb::g_verbose)         std::cerr << "[DEBUG] Parsing column records from mysql.columns..." << std::endl;

        // Map table_id -> columns
        std::map<uint32_t, std::vector<std::tuple<std::string, std::string, int, int>>> table_columns;

        // We know table_ids from space_id mapping
        // space_id 4 -> disaster_test/critical_data
        // space_id 5 -> disaster_test/recovered_data
        // space_id 6 -> rmrf_test/important_records
        // space_id 7 -> drop_test/users
        // table_id for these should be around 1070-1073 based on se_private_data

        // Search for column records with known table_ids
        for (const auto& [table_id, table_name] : known_table_ids) {
            std::string table_id_str = "table_id=" + std::to_string(table_id) + ";";

            for (uint64_t page_no = 0; page_no < num_pages; ++page_no) {
                file.seekg(page_no * page_size_, std::ios::beg);
                file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);
                if (!file) break;

                std::string page_content(reinterpret_cast<const char*>(page_buffer.data()), page_size_);

                // Search for table_id pattern
                size_t pos = 0;
                while ((pos = page_content.find(table_id_str, pos)) != std::string::npos) {
                    // Found column record, look backward for column name
                    // Column name appears before the table_id, like "id\x00\x00\x00\x01..."

                    // Look for column type after table_id
                    // Format: table_id=N; \x01 type_name \x80 ...
                    size_t type_start = pos + table_id_str.length();
                    if (page_content[type_start] == '\x01') {
                        type_start++;  // Skip the \x01 marker
                        std::string type_str;
                        while (type_start < page_content.size() &&
                               page_content[type_start] != '\x80' &&
                               isprint(page_content[type_start])) {
                            type_str += page_content[type_start++];
                        }

                        // Look backward for column name (appears before a byte pattern)
                        // Search for the pattern: column_name followed by \x00\x00\x00 ordinal
                        size_t search_start = (pos > 100) ? pos - 100 : 0;
                        std::string before = page_content.substr(search_start, pos - search_start);

                        // Find the column name - it's before the last \x00\x00\x00 pattern
                        // The format is: column_name\x00\x00\x00 ordinal_position...
                        size_t name_end = before.find('\x00');
                        if (name_end != std::string::npos && name_end > 0) {
                            // Look backward from name_end
                            size_t name_start = name_end;
                            while (name_start > 0 && before[name_start - 1] != '\x00' &&
                                   isprint(before[name_start - 1])) {
                                name_start--;
                            }

                            std::string col_name = before.substr(name_start, name_end - name_start);
                            if (!col_name.empty() && col_name.length() > 0 && col_name.length() < 50 &&
                                col_name.find("PRIMARY") == std::string::npos &&
                                col_name.find("DB_") == std::string::npos) {

                                // Parse type information
                                // varchar(N), int, timestamp, etc.
                                int col_type = 0;  // InnoDB type
                                int col_len = 0;

                                if (type_str.find("int") != std::string::npos) {
                                    col_type = 4;  // InnoDB LONG type
                                    col_len = 4;
                                    if (type_str.find("bigint") != std::string::npos) {
                                        col_len = 8;
                                    } else if (type_str.find("tinyint") != std::string::npos) {
                                        col_len = 1;
                                    } else if (type_str.find("smallint") != std::string::npos) {
                                        col_len = 2;
                                    } else if (type_str.find("mediumint") != std::string::npos) {
                                        col_len = 3;
                                    }
                                } else if (type_str.find("varchar") != std::string::npos) {
                                    col_type = 16;  // InnoDB VARCHAR type
                                    // Extract length from varchar(N)
                                    size_t len_pos = type_str.find('(');
                                    if (len_pos != std::string::npos) {
                                        std::string len_str = type_str.substr(len_pos + 1);
                                        col_len = std::stoi(len_str);
                                    }
                                } else if (type_str.find("char") != std::string::npos && type_str.find("varchar") == std::string::npos) {
                                    col_type = 16;  // CHAR also InnoDB VARCHAR type
                                    size_t len_pos = type_str.find('(');
                                    if (len_pos != std::string::npos) {
                                        std::string len_str = type_str.substr(len_pos + 1);
                                        col_len = std::stoi(len_str);
                                    }
                                } else if (type_str.find("timestamp") != std::string::npos) {
                                    col_type = 18;  // InnoDB TIMESTAMP2 type
                                    col_len = 4;
                                } else if (type_str.find("datetime") != std::string::npos) {
                                    col_type = 19;  // InnoDB DATETIME2 type
                                    col_len = 5;
                                } else if (type_str.find("date") != std::string::npos) {
                                    col_type = 15;  // InnoDB DATE type
                                    col_len = 3;
                                } else if (type_str.find("decimal") != std::string::npos) {
                                    col_type = 21;  // InnoDB DECIMAL type
                                    col_len = 8;  // Default for DECIMAL(10,2)
                                } else if (type_str.find("text") != std::string::npos) {
                                    col_type = 27;  // InnoDB BLOB type (TEXT=BLOB)
                                    col_len = 10;  // Default
                                } else if (type_str.find("blob") != std::string::npos) {
                                    col_type = 27;  // InnoDB BLOB type
                                    col_len = 10;
                                } else if (type_str.find("json") != std::string::npos) {
                                    col_type = 31;  // InnoDB JSON type
                                    col_len = 10;
                                } else if (type_str.find("enum") != std::string::npos) {
                                    col_type = 5;  // InnoDB ENUM type
                                    col_len = 2;
                                } else if (type_str.find("set") != std::string::npos) {
                                    col_type = 5;  // InnoDB SET type
                                    col_len = 8;
                                } else if (type_str.find("float") != std::string::npos) {
                                    col_type = 3;  // InnoDB FLOAT type
                                    col_len = 4;
                                } else if (type_str.find("double") != std::string::npos) {
                                    col_type = 6;  // InnoDB DOUBLE type
                                    col_len = 8;
                                }

                                if (dbrecovery::innodb::g_verbose) 
                                std::cerr << "[DEBUG] Found column: " << col_name
                                          << " type=" << type_str << " (InnoDB type=" << col_type
                                          << ", len=" << col_len << ")" << std::endl;

                                table_columns[table_id].push_back(std::make_tuple(col_name, type_str, col_type, col_len));
                            }
                        }
                    }
                    pos++;
                }
            }
        }

        if (dbrecovery::innodb::g_verbose) 
        std::cerr << "[DEBUG] Found columns for " << table_columns.size() << " tables" << std::endl;
        for (const auto& [tid, cols] : table_columns) {
            if (dbrecovery::innodb::g_verbose)             std::cerr << "[DEBUG]   Table " << tid << ": ";
            for (const auto& [name, type_str, type, len] : cols) {
                std::cerr << name << "(" << type_str << ") ";
            }
            std::cerr << std::endl;
        }

        // Build schema for recovered tables
        for (auto& table : recovered_tables) {
            // Find table_id for this table
            uint32_t table_id = 0;
            for (const auto& [tid, tname] : known_table_ids) {
                if (tname == table.schema_name + "/" + table.table_name) {
                    table_id = tid;
                    break;
                }
            }

            if (table_id > 0 && table_columns.count(table_id)) {
                // Build schema from columns
                catalog::TableSchema schema;
                schema.table_name = table.table_name;

                uint32_t ordinal = 1;
                for (const auto& [col_name, type_str, col_type, col_len] : table_columns[table_id]) {
                    catalog::ColumnInfo col;
                    col.name = col_name;
                    col.mysql_type_id = col_type;
                    col.column_length = col_len;
                    col.ordinal = ordinal;
                    col.physical_pos = ordinal;  // Same as ordinal for DD-discovered columns
                    col.is_nullable = true;  // Default nullable (safer for parsing)
                    col.is_signed = true;  // Default signed
                    col.charset_id = 45;  // utf8mb4_general_ci default
                    col.collation_id = 45;
                    col.numeric_precision = (col_type == 4) ? 10 : 0;  // INT default
                    col.numeric_scale = 0;
                    col.datetime_precision = 0;

                    schema.columns[ordinal++] = col;
                }

                table.schema = schema;
                table.table_id = table_id;  // Set the actual table_id
                if (dbrecovery::innodb::g_verbose)                 std::cerr << "[DEBUG] Built schema for " << table.table_name
                          << " with " << schema.columns.size() << " columns" << std::endl;
            }
        }

        // Step 3: Scan all INDEX pages for deleted records in DD tables
        // DD tables in MySQL 8.0: tables, columns, indexes, index_column_usage, etc.
        static const std::set<std::string> DD_TABLES = {
            "tables", "columns", "indexes", "index_column_usage",
            "foreign_keys", "foreign_key_column_usage", "check_constraints",
            "table_partitions", "schemata", "tablespaces"
        };

        // Map index_id -> pages
        std::map<uint64_t, std::vector<std::vector<uint8_t>>> dd_index_pages;

        for (uint64_t page_no = 0; page_no < num_pages; ++page_no) {
            file.seekg(page_no * page_size_, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);
            if (!file) break;

            if (!is_index_page_type(page_buffer.data())) continue;

            uint64_t idx_id = extract_index_id_from_page(page_buffer.data());

            // Check if this is a DD table index
            if (index_id_to_table_name.count(idx_id)) {
                dd_index_pages[idx_id].push_back(page_buffer);
            }
        }

        // Step 3: Parse deleted records from DD tables
        // Key fields: tables.id -> table_id, tables.name -> table_name
        std::map<uint64_t, std::map<std::string, std::string>> dd_tables_data;
        std::map<uint64_t, std::vector<std::map<std::string, std::string>>> dd_columns_data;
        std::map<uint64_t, std::vector<std::map<std::string, std::string>>> dd_indexes_data;

        // Parse mysql.tables pages for deleted records
        if (dd_table_index_ids.count("tables")) {
            uint64_t tables_idx_id = dd_table_index_ids["tables"];
            for (const auto& page_data : dd_index_pages[tables_idx_id]) {
                parse_dd_records_deleted(page_data.data(), "tables", dd_tables_data);
            }
        }

        // Parse mysql.columns pages
        if (dd_table_index_ids.count("columns")) {
            uint64_t columns_idx_id = dd_table_index_ids["columns"];
            for (const auto& page_data : dd_index_pages[columns_idx_id]) {
                parse_dd_records_deleted_columns(page_data.data(), dd_columns_data);
            }
        }

        // Parse mysql.indexes pages
        if (dd_table_index_ids.count("indexes")) {
            uint64_t indexes_idx_id = dd_table_index_ids["indexes"];
            for (const auto& page_data : dd_index_pages[indexes_idx_id]) {
                parse_dd_records_deleted_indexes(page_data.data(), dd_indexes_data);
            }
        }

        // Step 4: Build recovered table metadata
        for (const auto& [table_id, table_data] : dd_tables_data) {
            // Get name from map
            std::string name;
            if (table_data.count("name")) name = table_data.at("name");
            if (name.empty()) continue;

            // Parse schema/table format
            size_t slash_pos = name.find('/');
            if (slash_pos == std::string::npos) continue;

            std::string schema_name = name.substr(0, slash_pos);
            std::string table_name = name.substr(slash_pos + 1);

            // Skip system schemas
            if (schema_name == "mysql" || schema_name == "sys" ||
                schema_name == "information_schema" || schema_name == "performance_schema") {
                continue;
            }

            RecoveredTableMetadata metadata;
            metadata.table_id = table_id;
            metadata.schema_name = schema_name;
            metadata.table_name = table_name;
            metadata.recovered_from_deleted = true;
            metadata.confidence = 0.9;

            // Get columns
            if (dd_columns_data.count(table_id)) {
                for (const auto& col_data : dd_columns_data[table_id]) {
                    SysColumnEntry col;
                    col.table_id = table_id;
                    if (col_data.count("name")) col.name = col_data.at("name");
                    if (col_data.count("ordinal_position")) col.pos = std::stoul(col_data.at("ordinal_position"));
                    if (col_data.count("type")) col.mtype = std::stoul(col_data.at("type"));
                    if (col_data.count("char_length")) col.len = std::stoul(col_data.at("char_length"));
                    col.is_nullable = col_data.count("is_nullable") && col_data.at("is_nullable") != "0";
                    col.is_unsigned = col_data.count("is_unsigned") && col_data.at("is_unsigned") != "0";
                    metadata.columns.push_back(col);
                }

                // Sort by position
                std::sort(metadata.columns.begin(), metadata.columns.end(),
                    [](const SysColumnEntry& a, const SysColumnEntry& b) { return a.pos < b.pos; });
            }

            // Build schema
            metadata.schema = build_schema_from_dd(table_name, metadata.columns);

            // Get indexes and find primary key
            if (dd_indexes_data.count(table_id)) {
                for (const auto& idx_data : dd_indexes_data[table_id]) {
                    SysIndexEntry idx;
                    idx.table_id = table_id;
                    if (idx_data.count("name")) idx.name = idx_data.at("name");
                    if (idx_data.count("id")) idx.index_id = std::stoull(idx_data.at("id"));
                    if (idx_data.count("type")) idx.type = std::stoul(idx_data.at("type"));

                    if (idx.name == "PRIMARY") {
                        metadata.primary_index_id = idx.index_id;

                        // Parse index_id from se_private_data
                        if (idx_data.count("se_private_data")) {
                            std::string private_data = idx_data.at("se_private_data");
                            size_t id_pos = private_data.find("id=");
                            if (id_pos != std::string::npos) {
                                metadata.primary_index_id = std::stoull(private_data.substr(id_pos + 3));
                            }
                        }
                    }
                    metadata.indexes.push_back(idx);
                }
            }

            recovered_tables.push_back(metadata);
        }
    } else {
        // MySQL 5.7: Parse SYS_TABLES
        // Map to store found pages by index_id
        std::map<uint64_t, std::vector<std::vector<uint8_t>>> pages_by_index_id;

        // Scan all pages looking for INDEX pages with system table index IDs
        for (uint64_t page_no = 0; page_no < num_pages; ++page_no) {
            file.seekg(page_no * page_size_, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);

            if (!file) break;

            // Check if this is an INDEX page
            if (!is_index_page_type(page_buffer.data())) continue;

            // Extract index_id
            uint64_t idx_id = extract_index_id_from_page(page_buffer.data());

            // Check if it's one of the system table index IDs
            bool is_sys_table = (idx_id == sys_index_ids::SYS_TABLES ||
                               idx_id == sys_index_ids::SYS_COLUMNS ||
                               idx_id == sys_index_ids::SYS_INDEXES ||
                               idx_id == sys_index_ids::SYS_FIELDS);

            if (is_sys_table) {
                pages_by_index_id[idx_id].push_back(page_buffer);
            }
        }

        // Parse records from system tables
        if (pages_by_index_id.count(sys_index_ids::SYS_TABLES)) {
            for (const auto& page_data : pages_by_index_id[sys_index_ids::SYS_TABLES]) {
                parse_sys_tables_page(page_data.data());
            }
        }

        if (pages_by_index_id.count(sys_index_ids::SYS_COLUMNS)) {
            for (const auto& page_data : pages_by_index_id[sys_index_ids::SYS_COLUMNS]) {
                parse_sys_columns_page(page_data.data());
            }
        }

        if (pages_by_index_id.count(sys_index_ids::SYS_INDEXES)) {
            for (const auto& page_data : pages_by_index_id[sys_index_ids::SYS_INDEXES]) {
                parse_sys_indexes_page(page_data.data());
            }
        }
    }

    // Build recovered table metadata
    for (const auto& table : tables_) {
        RecoveredTableMetadata metadata;
        metadata.table_id = table.table_id;
        metadata.schema_name = table.schema_name;
        metadata.table_name = table.table_name;
        metadata.space_id = table.space_id;
        metadata.recovered_from_deleted = true;
        metadata.confidence = 0.9;

        // Get columns
        metadata.columns = get_columns(table.table_id);

        // Build schema
        metadata.schema = build_schema(table, metadata.columns);

        // Get indexes
        metadata.indexes = get_indexes(table.table_id);

        // Find primary key index_id
        for (const auto& idx : metadata.indexes) {
            if (idx.name == "PRIMARY") {
                metadata.primary_index_id = idx.index_id;
                metadata.root_page_no = idx.page_no;
                break;
            }
        }

        recovered_tables.push_back(metadata);
    }

    }  // close is_mysql80_format() block

    return recovered_tables;
}

void SystemTablespaceScanner::parse_sys_tables_page(const uint8_t* data) {
    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    if (page_type != 0x45BF) return;

    // Parse page header to get slot directory info
    uint16_t n_dir_slots = (static_cast<uint16_t>(data[38]) << 8) | data[39];
    if (n_dir_slots == 0 || n_dir_slots > 1000) return;

    // Slot directory is at the end of the page
    size_t slot_dir_offset = page_size_ - n_dir_slots * 2;

    // Iterate through slots (skip first 2: infimum and supremum)
    for (size_t slot = 2; slot < n_dir_slots; slot++) {
        size_t slot_pos = slot_dir_offset + slot * 2;
        if (slot_pos + 2 > page_size_) break;

        // Read record offset from slot (big-endian)
        uint16_t record_offset = (static_cast<uint16_t>(data[slot_pos]) << 8) | data[slot_pos + 1];
        if (record_offset == 0 || record_offset >= page_size_ - 50) continue;

        // Parse REDUNDANT record header at record_offset
        auto header_opt = RedundantHeader::parse(data + record_offset, page_size_ - record_offset);
        if (!header_opt.has_value()) continue;

        RedundantHeader header = header_opt.value();

        // Skip deleted records for normal tables (we want existing tables)
        if (header.is_deleted()) continue;

        // Get record data start (after 6-byte header for REDUNDANT)
        size_t pos = record_offset + 6;
        if (pos >= page_size_ - 20) continue;

        // SYS_TABLES row format (REDUNDANT):
        // Field 1: NAME (length-encoded string)
        // Field 2: ID (8 bytes)
        // Field 3: N_COLS (4 bytes)
        // Field 4: TYPE (4 bytes)
        // Field 5: MIX_ID (8 bytes)
        // Field 6: MIX_LEN (4 bytes)
        // Field 7: CLUSTER_NAME (length-encoded string)
        // Field 8: SPACE (4 bytes)

        // Read name length (first byte is length for REDUNDANT)
        uint8_t name_len = data[pos++];
        if (name_len == 0 || name_len > 200 || pos + name_len > page_size_ - 50) continue;

        std::string name(reinterpret_cast<const char*>(data + pos), name_len);
        pos += name_len;

        // Check if name looks like a table name (schema/table format)
        size_t slash_pos = name.find('/');
        if (slash_pos == std::string::npos || slash_pos == 0 || slash_pos >= name.length() - 1) {
            continue;
        }

        SysTableEntry table;
        table.deleted = false;
        table.dropped = false;
        table.schema_name = name.substr(0, slash_pos);
        table.table_name = name.substr(slash_pos + 1);

        // Skip system tables
        if (table.schema_name == "SYS" || table.schema_name == "mysql" ||
            table.schema_name.empty() || table.table_name.empty()) {
            continue;
        }

        // Skip FTS tables
        if (table.table_name.substr(0, 4) == "FTS_") {
            continue;
        }

        // Parse id (8 bytes, big-endian)
        if (pos + 8 > page_size_) continue;
        table.table_id = read_be_u64(data + pos);
        pos += 8;

        // Parse n_cols (4 bytes)
        if (pos + 4 > page_size_) continue;
        table.n_cols = read_be_u32(data + pos);
        pos += 4;

        // Parse type (4 bytes)
        if (pos + 4 > page_size_) continue;
        table.type = read_be_u32(data + pos);
        pos += 4;

        // Determine row format from type flags
        table.row_format = table.type;

        // Skip mix_id, mix_len
        pos += 8 + 4;

        // Skip cluster_name
        if (pos >= page_size_) continue;
        uint8_t cluster_len = data[pos++];
        pos += cluster_len;

        // Parse space (4 bytes)
        if (pos + 4 > page_size_) continue;
        table.space_id = read_be_u32(data + pos);

        // Validate the entry
        if (table.table_id > 0 && table.table_id < 1000000000 &&
            table.n_cols > 0 && table.n_cols < 1000 &&
            table.space_id < 1000000000) {
            tables_.push_back(table);
        }
    }
}

void SystemTablespaceScanner::parse_sys_columns_page(const uint8_t* data) {
    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    if (page_type != 0x45BF) return;

    // Parse page header to get slot directory info
    uint16_t n_dir_slots = (static_cast<uint16_t>(data[38]) << 8) | data[39];
    if (n_dir_slots == 0 || n_dir_slots > 1000) return;

    // Slot directory is at the end of the page
    size_t slot_dir_offset = page_size_ - n_dir_slots * 2;

    // Iterate through slots (skip first 2: infimum and supremum)
    for (size_t slot = 2; slot < n_dir_slots; slot++) {
        size_t slot_pos = slot_dir_offset + slot * 2;
        if (slot_pos + 2 > page_size_) break;

        // Read record offset from slot (big-endian)
        uint16_t record_offset = (static_cast<uint16_t>(data[slot_pos]) << 8) | data[slot_pos + 1];
        if (record_offset == 0 || record_offset >= page_size_ - 50) continue;

        // Parse REDUNDANT record header at record_offset
        auto header_opt = RedundantHeader::parse(data + record_offset, page_size_ - record_offset);
        if (!header_opt.has_value()) continue;

        RedundantHeader header = header_opt.value();

        // Skip deleted records for normal columns
        if (header.is_deleted()) continue;

        // Get record data start (after 6-byte header for REDUNDANT)
        size_t pos = record_offset + 6;
        if (pos >= page_size_ - 20) continue;

        SysColumnEntry col;

        // Parse table_id (8 bytes)
        if (pos + 8 > page_size_) continue;
        col.table_id = read_be_u64(data + pos);
        pos += 8;

        if (col.table_id == 0 || col.table_id > 1000000000) continue;

        // Parse column position (4 bytes)
        if (pos + 4 > page_size_) continue;
        col.pos = read_be_u32(data + pos);
        pos += 4;

        // Skip virtual columns (pos >> 16 != 0)
        if (col.pos >> 16) continue;

        // Parse column name (length-encoded)
        if (pos >= page_size_) continue;
        uint8_t name_len = data[pos++];
        if (name_len == 0 || name_len > 255 || pos + name_len > page_size_) continue;

        col.name = std::string(reinterpret_cast<const char*>(data + pos), name_len);
        pos += name_len;

        // Skip system columns
        if (col.name.empty() || col.name == "DB_TRX_ID" ||
            col.name == "DB_ROLL_PTR" || col.name == "DB_ROW_ID") {
            continue;
        }

        // Parse mtype (4 bytes)
        if (pos + 4 > page_size_) continue;
        col.mtype = read_be_u32(data + pos);
        pos += 4;

        // Parse prtype (4 bytes)
        if (pos + 4 > page_size_) continue;
        col.prtype = read_be_u32(data + pos);
        pos += 4;

        // Parse prtype flags
        ParsedPrType parsed = parse_prtype(col.prtype);
        col.is_nullable = parsed.is_nullable;
        col.is_unsigned = parsed.is_unsigned;

        // Parse len (4 bytes)
        if (pos + 4 > page_size_) continue;
        col.len = read_be_u32(data + pos);

        // Validate column
        if (col.pos < 1000 && !col.name.empty()) {
            columns_.push_back(col);
        }
    }
}

void SystemTablespaceScanner::parse_sys_indexes_page(const uint8_t* data) {
    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    if (page_type != 0x45BF) return;

    // Parse page header to get slot directory info
    uint16_t n_dir_slots = (static_cast<uint16_t>(data[38]) << 8) | data[39];
    if (n_dir_slots == 0 || n_dir_slots > 1000) return;

    // Slot directory is at the end of the page
    size_t slot_dir_offset = page_size_ - n_dir_slots * 2;

    // Iterate through slots (skip first 2: infimum and supremum)
    for (size_t slot = 2; slot < n_dir_slots; slot++) {
        size_t slot_pos = slot_dir_offset + slot * 2;
        if (slot_pos + 2 > page_size_) break;

        // Read record offset from slot (big-endian)
        uint16_t record_offset = (static_cast<uint16_t>(data[slot_pos]) << 8) | data[slot_pos + 1];
        if (record_offset == 0 || record_offset >= page_size_ - 50) continue;

        // Parse REDUNDANT record header at record_offset
        auto header_opt = RedundantHeader::parse(data + record_offset, page_size_ - record_offset);
        if (!header_opt.has_value()) continue;

        RedundantHeader header = header_opt.value();

        // Skip deleted records for normal indexes
        if (header.is_deleted()) continue;

        // Get record data start (after 6-byte header for REDUNDANT)
        size_t pos = record_offset + 6;
        if (pos >= page_size_ - 20) continue;

        SysIndexEntry idx;

        // Parse table_id (8 bytes)
        if (pos + 8 > page_size_) continue;
        idx.table_id = read_be_u64(data + pos);
        pos += 8;

        if (idx.table_id == 0 || idx.table_id > 1000000000) continue;

        // Parse index_id (8 bytes)
        if (pos + 8 > page_size_) continue;
        idx.index_id = read_be_u64(data + pos);
        pos += 8;

        // Parse name (length-encoded)
        if (pos >= page_size_) continue;
        uint8_t name_len = data[pos++];
        if (name_len == 0 || name_len > 255 || pos + name_len > page_size_) continue;

        idx.name = std::string(reinterpret_cast<const char*>(data + pos), name_len);
        pos += name_len;

        // Skip n_fields (4 bytes)
        pos += 4;

        // Parse type (4 bytes)
        if (pos + 4 > page_size_) continue;
        idx.type = read_be_u32(data + pos);
        pos += 4;

        // Parse space (4 bytes)
        if (pos + 4 > page_size_) continue;
        idx.space_id = read_be_u32(data + pos);
        pos += 4;

        // Parse page_no (4 bytes)
        if (pos + 4 > page_size_) continue;
        idx.page_no = read_be_u32(data + pos);

        // Validate
        if (idx.index_id > 0 && !idx.name.empty()) {
            indexes_.push_back(idx);
        }
    }
}

catalog::TableSchema SystemTablespaceScanner::build_schema(
    const SysTableEntry& table, const std::vector<SysColumnEntry>& cols) {

    catalog::TableSchema schema;
    schema.table_name = table.table_name;

    for (const auto& col : cols) {
        catalog::ColumnInfo column;
        column.name = col.name;
        column.mysql_type_id = col.mtype;
        column.is_nullable = col.is_nullable;
        column.column_length = col.len;
        schema.columns[schema.columns.size()] = column;
    }

    return schema;
}

// ==================== Orphaned Page Scanner ====================

OrphanedPageScanner::OrphanedPageScanner(RawDiskIOEngine& engine)
    : engine_(engine) {
}

std::vector<OrphanedPageScanner::OrphanedPage> OrphanedPageScanner::scan_by_index_id(
    uint64_t index_id, uint32_t page_size) {

    std::vector<OrphanedPage> pages;

    engine::SignatureBasedScraper scraper(engine_);
    engine::SignatureBasedScraper::ScanConfig config;
    config.min_page_size = page_size;
    config.max_page_size = page_size;
    config.validate_checksums = true;
    config.filter_to_index_pages = true;

    std::vector<engine::FoundPage> found_pages;
    scraper.scan(config, [&found_pages](const engine::FoundPage& p) {
        found_pages.push_back(p);
    });

    for (const auto& fp : found_pages) {
        if (fp.data.size() >= 74) {
            uint64_t idx_id = (static_cast<uint64_t>(fp.data[66]) << 56) |
                              (static_cast<uint64_t>(fp.data[67]) << 48) |
                              (static_cast<uint64_t>(fp.data[68]) << 40) |
                              (static_cast<uint64_t>(fp.data[69]) << 32) |
                              (static_cast<uint64_t>(fp.data[70]) << 24) |
                              (static_cast<uint64_t>(fp.data[71]) << 16) |
                              (static_cast<uint64_t>(fp.data[72]) << 8)  |
                              static_cast<uint64_t>(fp.data[73]);

            if (idx_id == index_id) {
                OrphanedPage op;
                op.disk_offset = fp.disk_offset;
                op.page_number = fp.page_number;
                op.index_id = idx_id;
                op.space_id = fp.space_id;
                op.page_size = page_size;
                op.lsn = fp.lsn;
                op.record_count = fp.n_records;
                op.data = fp.data;
                pages.push_back(op);
            }
        }
    }

    return pages;
}

std::map<uint64_t, std::vector<OrphanedPageScanner::OrphanedPage>>
OrphanedPageScanner::scan_by_index_ids(const std::vector<uint64_t>& index_ids, uint32_t page_size) {

    std::map<uint64_t, std::vector<OrphanedPage>> result;

    for (uint64_t id : index_ids) {
        result[id] = std::vector<OrphanedPage>();
    }

    engine::SignatureBasedScraper scraper(engine_);
    engine::SignatureBasedScraper::ScanConfig config;
    config.min_page_size = page_size;
    config.max_page_size = page_size;
    config.validate_checksums = true;
    config.filter_to_index_pages = true;

    std::vector<engine::FoundPage> found_pages;
    scraper.scan(config, [&found_pages](const engine::FoundPage& p) {
        found_pages.push_back(p);
    });

    for (const auto& fp : found_pages) {
        if (fp.data.size() >= 74) {
            uint64_t idx_id = (static_cast<uint64_t>(fp.data[66]) << 56) |
                              (static_cast<uint64_t>(fp.data[67]) << 48) |
                              (static_cast<uint64_t>(fp.data[68]) << 40) |
                              (static_cast<uint64_t>(fp.data[69]) << 32) |
                              (static_cast<uint64_t>(fp.data[70]) << 24) |
                              (static_cast<uint64_t>(fp.data[71]) << 16) |
                              (static_cast<uint64_t>(fp.data[72]) << 8)  |
                              static_cast<uint64_t>(fp.data[73]);

            if (std::find(index_ids.begin(), index_ids.end(), idx_id) != index_ids.end()) {
                OrphanedPage op;
                op.disk_offset = fp.disk_offset;
                op.page_number = fp.page_number;
                op.index_id = idx_id;
                op.space_id = fp.space_id;
                op.page_size = page_size;
                op.lsn = fp.lsn;
                op.record_count = fp.n_records;
                op.data = fp.data;
                result[idx_id].push_back(op);
            }
        }
    }

    return result;
}

std::vector<OrphanedPageScanner::OrphanedPage> OrphanedPageScanner::scan_all(
    uint32_t page_size, std::function<void(double)> callback) {

    std::vector<OrphanedPage> pages;

    engine::SignatureBasedScraper scraper(engine_);
    scraper.set_progress_callback(callback);

    engine::SignatureBasedScraper::ScanConfig config;
    config.min_page_size = page_size;
    config.max_page_size = page_size;
    config.validate_checksums = true;
    config.filter_to_index_pages = true;

    std::vector<engine::FoundPage> found_pages;
    scraper.scan(config, [&found_pages](const engine::FoundPage& p) {
        found_pages.push_back(p);
    });

    for (const auto& fp : found_pages) {
        OrphanedPage op;
        op.disk_offset = fp.disk_offset;
        op.page_number = fp.page_number;
        op.space_id = fp.space_id;
        op.page_size = page_size;
        op.lsn = fp.lsn;
        op.record_count = fp.n_records;
        op.data = fp.data;

        if (fp.data.size() >= 74) {
            op.index_id = (static_cast<uint64_t>(fp.data[66]) << 56) |
                          (static_cast<uint64_t>(fp.data[67]) << 48) |
                          (static_cast<uint64_t>(fp.data[68]) << 40) |
                          (static_cast<uint64_t>(fp.data[69]) << 32) |
                          (static_cast<uint64_t>(fp.data[70]) << 24) |
                          (static_cast<uint64_t>(fp.data[71]) << 16) |
                          (static_cast<uint64_t>(fp.data[72]) << 8)  |
                          static_cast<uint64_t>(fp.data[73]);
        }

        pages.push_back(op);
    }

    return pages;
}

bool OrphanedPageScanner::is_index_page(const uint8_t* data, uint32_t page_size) {
    if (!data) return false;
    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    return page_type == 0x45BF;
}

uint64_t OrphanedPageScanner::extract_index_id(const uint8_t* data, uint32_t page_size) {
    if (!data || page_size < 74) return 0;
    return (static_cast<uint64_t>(data[66]) << 56) |
           (static_cast<uint64_t>(data[67]) << 48) |
           (static_cast<uint64_t>(data[68]) << 40) |
           (static_cast<uint64_t>(data[69]) << 32) |
           (static_cast<uint64_t>(data[70]) << 24) |
           (static_cast<uint64_t>(data[71]) << 16) |
           (static_cast<uint64_t>(data[72]) << 8)  |
           static_cast<uint64_t>(data[73]);
}

bool OrphanedPageScanner::verify_checksum(const uint8_t* data, uint32_t page_size) {
    return utils::InnoDBChecksum::validate_crc32(data, page_size);
}

// ==================== DROP TABLE Recovery ====================

DropTableRecovery::DropTableRecovery() {
}

void DropTableRecovery::set_config(const Config& config) {
    config_ = config;
}

DropTableRecovery::Result DropTableRecovery::recover(
    std::function<void(double, const std::string&)> progress) {

    Result result;
    result.success = false;
    result.tables_found = 0;
    result.tables_recovered = 0;
    result.total_records = 0;
    result.scan_time_seconds = 0;
    result.recovery_time_seconds = 0;

    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        if (progress) progress(0.0, "Starting DROP TABLE recovery...");

        // Determine system tablespace path
        std::string system_tablespace_path = config_.system_tablespace;
        if (system_tablespace_path.empty() && !config_.data_directory.empty()) {
            // Auto-detect system tablespace from data directory
            std::filesystem::path data_dir(config_.data_directory);

            // Try MySQL 8.0+ mysql.ibd first
            std::filesystem::path mysql_ibd = data_dir / "mysql.ibd";
            if (std::filesystem::exists(mysql_ibd)) {
                system_tablespace_path = mysql_ibd.string();
            } else {
                // Try MySQL 5.x ibdata1
                std::filesystem::path ibdata1 = data_dir / "ibdata1";
                if (std::filesystem::exists(ibdata1)) {
                    system_tablespace_path = ibdata1.string();
                }
            }
        }

        // Step 1: Scan system tablespace for deleted table metadata
        if (!system_tablespace_path.empty()) {
            if (progress) progress(0.1, "Scanning system tablespace for deleted tables...");

            sys_scanner_ = std::make_unique<SystemTablespaceScanner>(
                system_tablespace_path, config_.page_size);

            auto recovered_tables = sys_scanner_->scan_deleted_tables();
            result.tables_found = recovered_tables.size();
            result.recovered_tables = recovered_tables;

            if (progress) {
                std::ostringstream oss;
                oss << "Found " << recovered_tables.size() << " deleted table(s) in system tablespace";
                progress(0.2, oss.str());
            }
        }

        // Step 2: Scan for orphaned pages
        // If we have recovered table metadata OR a schema file, proceed with scanning
        bool has_schema = !config_.schema_file.empty();
        if (!result.recovered_tables.empty() || has_schema) {

            // Load schema from file if provided
            catalog::TableSchema loaded_schema;
            if (has_schema) {
                try {
                    std::ifstream sf(config_.schema_file);
                    if (sf) {
                        nlohmann::json schema_json = nlohmann::json::parse(sf);
                        int ordinal = 0;
                        for (const auto& col_json : schema_json) {
                            catalog::ColumnInfo col;
                            col.name = col_json.value("name", "");
                            col.mysql_type_id = col_json.value("mysql_type_id", 0);
                            col.is_nullable = col_json.value("is_nullable", true);
                            col.is_signed = col_json.value("is_signed", true);
                            col.column_length = col_json.value("column_length", 0);
                            col.ordinal = col_json.value("ordinal", ordinal);
                            col.physical_pos = col_json.value("physical_pos", ordinal);
                            col.charset_id = col_json.value("charset_id", 45);
                            col.collation_id = col_json.value("collation_id", 45);
                            col.numeric_precision = col_json.value("numeric_precision", 0);
                            col.numeric_scale = col_json.value("numeric_scale", 0);
                            col.datetime_precision = col_json.value("datetime_precision", 0);
                            col.enum_values = col_json.value("enum_values", std::vector<std::string>{});
                            col.set_values = col_json.value("set_values", std::vector<std::string>{});
                            loaded_schema.columns[ordinal] = col;
                            ordinal++;
                        }
                        std::cout << "[INFO] Loaded schema from " << config_.schema_file
                                  << " with " << loaded_schema.columns.size() << " columns\n";
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[WARN] Failed to load schema file: " << e.what() << "\n";
                    has_schema = false;
                }
            }

            // Apply loaded schema to recovered tables
            if (has_schema && !loaded_schema.columns.empty()) {
                if (result.recovered_tables.empty()) {
                    RecoveredTableMetadata meta;
                    meta.table_name = "recovered";
                    meta.schema = loaded_schema;
                    result.recovered_tables.push_back(meta);
                } else {
                    for (auto& table : result.recovered_tables) {
                        loaded_schema.table_name = table.table_name;
                        table.schema = loaded_schema;
                    }
                }
            }

            // Collect all primary index IDs to scan for
            std::vector<uint64_t> primary_index_ids;
            for (const auto& table : result.recovered_tables) {
                if (table.primary_index_id > 0) {
                    primary_index_ids.push_back(table.primary_index_id);
                }
            }

            if (!primary_index_ids.empty() && !config_.device_path.empty()) {
                // Deep scan raw device for orphaned pages
                if (progress) progress(0.3, "Scanning raw device for orphaned pages...");

                RawDiskIOEngine engine;
                if (engine.open(config_.device_path)) {
                    page_scanner_ = std::make_unique<OrphanedPageScanner>(engine);

                    auto orphaned_pages = page_scanner_->scan_by_index_ids(
                        primary_index_ids, config_.page_size);

                    if (progress) {
                        uint64_t total_pages = 0;
                        for (const auto& [id, pages] : orphaned_pages) {
                            total_pages += pages.size();
                        }
                        std::ostringstream oss;
                        oss << "Found " << total_pages << " orphaned INDEX page(s)";
                        progress(0.5, oss.str());
                    }

                    // Step 3: Extract records from orphaned pages
                    if (progress) progress(0.6, "Extracting records from orphaned pages...");

                    for (auto& table : result.recovered_tables) {
                        if (table.primary_index_id == 0) continue;

                        auto it = orphaned_pages.find(table.primary_index_id);
                        if (it == orphaned_pages.end() || it->second.empty()) continue;

                        auto records = extract_records(it->second, table.schema);
                        if (!records.empty()) {
                            result.records_per_table[table.table_id] = records.size();
                            result.total_records += records.size();

                            // Export recovered data
                            if (export_data(table.table_name, table.schema, records)) {
                                result.tables_recovered++;
                            }
                        }
                    }
                }
            } else if (!config_.data_directory.empty()) {
                // Scan data directory for existing .ibd files (for tables that haven't been
                // physically deleted yet, or for recovering from backup IBD files)
                // Note: For actually dropped tables, use --device-path for disk carving
                if (progress) progress(0.3, "Scanning data directory for .ibd files...");

                auto ibd_files = find_ibd_files();

                if (progress) {
                    std::ostringstream oss;
                    oss << "Found " << ibd_files.size() << " .ibd file(s) to scan";
                    progress(0.4, oss.str());
                }

                // Scan each .ibd file for INDEX pages matching known index IDs
                for (const auto& ibd_path : ibd_files) {
                    // Use InnoDBFileReader for proper page-boundary reading
                    innodb::InnoDBFileReader reader(ibd_path);
                    if (!reader.is_valid()) {
                        if (dbrecovery::innodb::g_verbose)                         std::cerr << "[DEBUG] Cannot open " << ibd_path << " as valid IBD file\n";
                        continue;
                    }

                    uint32_t total_pages = reader.get_total_pages();
                    if (dbrecovery::innodb::g_verbose)                     std::cerr << "[DEBUG] Reading " << ibd_path << ": "
                              << total_pages << " pages, page_size=" << reader.get_page_size() << std::endl;

                    // Read all pages and collect INDEX pages
                    std::vector<OrphanedPageScanner::OrphanedPage> index_pages;
                    for (uint32_t pg = 0; pg < total_pages; pg++) {
                        auto page_data = reader.read_page_raw(pg);
                        if (page_data.empty() || page_data.size() < 50) continue;

                        const uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(
                            page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
                        if (page_type == dbrecovery::innodb::FIL_PAGE_INDEX) {
                            OrphanedPageScanner::OrphanedPage op;
                            op.page_number = pg;
                            op.page_size = reader.get_page_size();
                            op.disk_offset = static_cast<uint64_t>(pg) * reader.get_page_size();
                            op.data = std::move(page_data);
                            op.index_id = (static_cast<uint64_t>(op.data[66]) << 56) |
                                         (static_cast<uint64_t>(op.data[67]) << 48) |
                                         (static_cast<uint64_t>(op.data[68]) << 40) |
                                         (static_cast<uint64_t>(op.data[69]) << 32) |
                                         (static_cast<uint64_t>(op.data[70]) << 24) |
                                         (static_cast<uint64_t>(op.data[71]) << 16) |
                                         (static_cast<uint64_t>(op.data[72]) << 8)  |
                                         static_cast<uint64_t>(op.data[73]);
                            index_pages.push_back(std::move(op));
                        }
                    }

                    if (dbrecovery::innodb::g_verbose)                     std::cerr << "[DEBUG] Found " << index_pages.size()
                              << " INDEX pages in " << ibd_path << std::endl;

                    // Extract records using schema
                    for (auto& table : result.recovered_tables) {
                        if (table.schema.columns.empty()) continue;

                        auto records = extract_records(index_pages, table.schema);
                        if (!records.empty()) {
                            result.records_per_table[table.table_id] += records.size();
                            result.total_records += records.size();
                            export_data(table.table_name, table.schema, records);
                        }
                    }
                }

                // Count tables with recovered records
                for (const auto& [table_id, count] : result.records_per_table) {
                    if (count > 0) result.tables_recovered++;
                }
            }
        }

        if (progress) progress(1.0, "Recovery complete");
        result.success = true;

    } catch (const std::exception& e) {
        result.errors.push_back(e.what());
        result.success = false;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.recovery_time_seconds = std::chrono::duration<double>(end_time - start_time).count();

    return result;
}

DropTableRecovery::Result DropTableRecovery::recover_table(
    const std::string& schema_name, const std::string& table_name) {
    return recover();
}

DropTableRecovery::Result DropTableRecovery::recover_by_table_id(uint64_t table_id) {
    Result result;
    result.success = false;
    return result;
}

std::vector<OrphanedPageScanner::OrphanedPage> DropTableRecovery::find_table_pages(
    const RecoveredTableMetadata& table) {

    if (!page_scanner_ || table.primary_index_id == 0) {
        return {};
    }

    return page_scanner_->scan_by_index_id(table.primary_index_id, config_.page_size);
}

std::vector<std::map<std::string, std::string>> DropTableRecovery::extract_records(
    const std::vector<OrphanedPageScanner::OrphanedPage>& pages,
    const catalog::TableSchema& schema) {

    std::vector<std::map<std::string, std::string>> all_records;

    if (schema.columns.empty()) {
        std::cout << "[DEBUG DROP] schema.columns is EMPTY! schema.table_name=" << schema.table_name << std::endl;
        return all_records;
    }

    // Build column definitions for InnoDBIndexParser
    std::vector<innodb::IndexColumnDef> col_defs;
    uint32_t n_pk = 0;
    std::set<std::string> pk_col_names;

    // Find PK columns from indexes
    for (const auto& idx_info : schema.indexes) {
        if (idx_info.is_primary) {
            for (const auto& col_name : idx_info.columns) {
                pk_col_names.insert(col_name);
            }
        }
    }

    for (const auto& [idx, col] : schema.columns) {
        innodb::IndexColumnDef def;
        def.name = col.name;
        def.col_type = col.mysql_type_id;
        def.is_nullable = col.is_nullable;
        def.is_unsigned = !col.is_signed;
        def.col_length = col.column_length;
        def.precision = col.numeric_precision;
        def.scale = col.numeric_scale;
        def.datetime_precision = col.datetime_precision;
        def.enum_values = col.enum_values;
        def.set_values = col.set_values;
        def.char_set_id = col.charset_id;
        def.mb_maxlen = 4;  // utf8mb4 default
        if (col.charset_id == 63) {
            def.mb_maxlen = 1;  // binary charset
        } else if (col.charset_id == 8 || col.charset_id == 83) {
            def.mb_maxlen = 1;  // latin1, utf8
        }

        // CRITICAL: Set physical_pos and ordinal_position for correct InnoDB storage order
        def.ordinal_position = col.ordinal;
        def.physical_pos = col.physical_pos;

        // Set is_big for variable-length columns
        if (def.is_variable_length()) {
            def.is_big = (def.col_length * def.mb_maxlen > 255);
        }

        // Set is_binary based on charset
        def.is_binary = (col.charset_id == 63 || col.collation_id == 63) && (def.col_type != 31);

        // Check if PK column
        if (pk_col_names.count(col.name) > 0) {
            n_pk++;
        }

        col_defs.push_back(def);
    }

    if (n_pk == 0 && !col_defs.empty()) {
        n_pk = 1;  // Default: first column is PK
    }

    // Create parser with schema
    innodb::InnoDBIndexParser parser;
    parser.set_columns(col_defs, false, n_pk);
    parser.set_recover_deleted(false);  // Only recover non-deleted records
    parser.set_bad_page_mode(0);  // Normal mode for properly structured pages

    // Process each orphaned page
    for (const auto& page : pages) {
        if (page.data.empty() || page.data.size() < 100) continue;

        // Verify this is an INDEX page (type 0x45BF)
        uint16_t page_type = (static_cast<uint16_t>(page.data[24]) << 8) | page.data[25];
        if (page_type != 0x45BF) continue;

        // Check if this is a leaf page (level 0)
        // Page header: offset 26-27 = level (big-endian)
        uint16_t level = (static_cast<uint16_t>(page.data[26]) << 8) | page.data[27];
        if (level != 0) continue;  // Skip non-leaf pages

        try {
            // Use InnoDBIndexParser to extract records
            auto records = parser.parse_page(page.data.data(), page.page_size, page.page_number);

            if (!records.empty() && all_records.empty()) {
                std::cout << "[DEBUG DROP] First record on page " << page.page_number
                          << " fields=" << records[0].fields.size()
                          << " cols=" << col_defs.size() << std::endl;
                for (size_t i = 0; i < std::min(records[0].fields.size(), col_defs.size()); i++) {
                    std::cout << "[DEBUG DROP]   field[" << i << "] name=" << col_defs[i].name
                              << " value='" << records[0].fields[i].str_value.substr(0, 40) << "'"
                              << " size=" << records[0].fields[i].str_value.size() << std::endl;
                }
            }

            for (const auto& rec : records) {
                std::map<std::string, std::string> record_map;
                for (size_t i = 0; i < rec.fields.size() && i < col_defs.size(); i++) {
                    record_map[col_defs[i].name] = rec.fields[i].str_value;
                }
                all_records.push_back(record_map);
            }
        } catch (...) {
            // Continue with next page if parsing fails
        }
    }

    return all_records;
}

bool DropTableRecovery::export_data(
    const std::string& table_name,
    const catalog::TableSchema& schema,
    const std::vector<std::map<std::string, std::string>>& records) {

    if (records.empty()) return false;

    std::string base_path = config_.output_directory + "/" + table_name;
    std::filesystem::create_directories(config_.output_directory);

    if (config_.export_csv) {
        std::ofstream csv_file(base_path + ".csv");
        if (csv_file) {
            bool first = true;
            for (const auto& [idx, col] : schema.columns) {
                if (!first) csv_file << ",";
                first = false;
                csv_file << col.name;
            }
            csv_file << "\n";

            for (const auto& rec : records) {
                first = true;
                for (const auto& [idx, col] : schema.columns) {
                    if (!first) csv_file << ",";
                    first = false;
                    auto it = rec.find(col.name);
                    if (it != rec.end()) {
                        csv_file << "\"" << it->second << "\"";
                    }
                }
                csv_file << "\n";
            }
        }
    }

    if (config_.export_sql) {
        std::ofstream sql_file(base_path + ".sql");
        if (sql_file) {
            sql_file << "-- Recovered data from table: " << table_name << "\n";
            sql_file << "-- Records: " << records.size() << "\n\n";

            for (const auto& rec : records) {
                sql_file << "INSERT INTO " << table_name << " (";
                bool first = true;
                for (const auto& [idx, col] : schema.columns) {
                    if (!first) sql_file << ", ";
                    first = false;
                    sql_file << col.name;
                }
                sql_file << ") VALUES (";
                first = true;
                for (const auto& [idx, col] : schema.columns) {
                    if (!first) sql_file << ", ";
                    first = false;
                    auto it = rec.find(col.name);
                    if (it != rec.end()) {
                        sql_file << "'" << it->second << "'";
                    } else {
                        sql_file << "NULL";
                    }
                }
                sql_file << ");\n";
            }
        }
    }

    return true;
}

std::vector<std::string> DropTableRecovery::find_ibd_files() {
    std::vector<std::string> ibd_files;

    if (config_.data_directory.empty()) return ibd_files;

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(config_.data_directory)) {
            if (entry.path().extension() == ".ibd") {
                ibd_files.push_back(entry.path().string());
            }
        }
    } catch (...) {}

    return ibd_files;
}

bool DropTableRecovery::try_recover_from_ibd(
    const std::string& ibd_path,
    const RecoveredTableMetadata& table,
    std::vector<std::map<std::string, std::string>>& records) {

    // Check if IBD file exists
    if (!std::filesystem::exists(ibd_path)) {
        return false;
    }

    // Get file size
    uint64_t file_size = std::filesystem::file_size(ibd_path);
    if (file_size < 16384) {  // Minimum one page
        return false;
    }

    // Determine page size from file
    uint32_t page_size = 16384;  // Default 16KB

    // Read first page to detect page size
    std::ifstream file(ibd_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    std::vector<uint8_t> first_page(page_size);
    file.read(reinterpret_cast<char*>(first_page.data()), page_size);

    // Check for valid InnoDB page
    uint16_t page_type = (static_cast<uint16_t>(first_page[24]) << 8) | first_page[25];

    // Use IndexPageParser to recover records
    innodb::InnoDBIndexParser parser;

    // Read and parse all pages
    file.seekg(0);
    uint64_t total_pages = file_size / page_size;

    for (uint64_t page_no = 0; page_no < total_pages; page_no++) {
        std::vector<uint8_t> page_data(page_size);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size);

        if (!file) break;

        // Parse page records using schema-aware parsing
        auto page_records = parser.parse_page(page_data.data(), page_size, page_no);
        for (const auto& rec : page_records) {
            // Convert IndexRecord to map format
            std::map<std::string, std::string> record;
            for (size_t i = 0; i < rec.fields.size() && i < table.columns.size(); i++) {
                record[table.columns[i].name] = rec.fields[i].to_string();
            }
            records.push_back(record);
        }
    }

    file.close();
    return !records.empty();
}

// ==================== Deleted Record Parsing Helpers ====================

void SystemTablespaceScanner::parse_sys_tables_page_deleted(const uint8_t* data) {
    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    if (page_type != 0x45BF) return;

    // Get deleted record offset from page directory (offset 44-46, big-endian)
    // This is the "free" list head - first deleted record
    uint16_t deleted_offset = (static_cast<uint16_t>(data[44]) << 8) | data[45];

    if (deleted_offset == 0 || deleted_offset >= page_size_) return;

    // Follow the deleted record chain
    size_t current_offset = deleted_offset;
    int max_iterations = 1000;  // Safety limit
    int iterations = 0;

    while (current_offset > 100 && current_offset < page_size_ - 50 && iterations < max_iterations) {
        iterations++;

        // Try to parse record at this offset
        // Record format: [record header] [trx_id] [roll_ptr] [field data...]
        size_t pos = current_offset;

        // Skip record header (variable size, typically 5-6 bytes)
        // Check for valid record start
        if (pos + 20 > page_size_) break;

        // Parse record info bytes
        uint8_t info_bits = data[pos];

        // Check if this looks like a valid record
        // Try to extract table name (first field in SYS_TABLES)
        // In DYNAMIC format, variable-length fields are stored with length prefix

        // Skip the record header and system columns (DB_TRX_ID, DB_ROLL_PTR)
        // For SYS_TABLES, the user columns are: NAME, ID, N_COLS, TYPE, MIX_ID, MIX_LEN, CLUSTER_NAME, SPACE

        // Try heuristic: look for schema/table pattern
        for (size_t probe = pos + 5; probe < std::min<size_t>(pos + 100, page_size_ - 50); probe++) {
            uint8_t len = data[probe];
            if (len == 0 || len > 200 || probe + len > page_size_ - 50) continue;

            std::string name(reinterpret_cast<const char*>(data + probe + 1), len);

            // Check for schema/table format
            size_t slash_pos = name.find('/');
            if (slash_pos != std::string::npos && slash_pos > 0 && slash_pos < name.length() - 1) {
                SysTableEntry table;
                table.deleted = true;
                table.dropped = true;
                table.schema_name = name.substr(0, slash_pos);
                table.table_name = name.substr(slash_pos + 1);

                // Skip system tables
                if (table.schema_name != "SYS" && table.schema_name != "mysql" &&
                    !table.schema_name.empty() && !table.table_name.empty() &&
                    table.table_name.substr(0, 4) != "FTS_") {

                    // Try to parse remaining fields
                    size_t field_pos = probe + 1 + len;

                    // ID (8 bytes, big-endian)
                    if (field_pos + 8 <= page_size_) {
                        table.table_id = read_be_u64(data + field_pos);
                        field_pos += 8;
                    }

                    // N_COLS (4 bytes)
                    if (field_pos + 4 <= page_size_) {
                        table.n_cols = read_be_u32(data + field_pos);
                    }

                    if (table.table_id > 0 && table.table_id < 1000000000) {
                        tables_.push_back(table);
                    }
                }
            }
        }

        // Move to next deleted record (offset stored in record header)
        // In InnoDB, deleted records form a linked list via "next" pointer in record header
        // The next offset is typically at offset+2 from current position (2 bytes, big-endian)
        if (pos + 2 < page_size_) {
            uint16_t next_offset = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
            if (next_offset == 0 || next_offset >= current_offset) break;  // End of chain or invalid
            current_offset = next_offset;
        } else {
            break;
        }
    }
}

void SystemTablespaceScanner::parse_sys_columns_page_deleted(const uint8_t* data) {
    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    if (page_type != 0x45BF) return;

    // Get deleted record offset
    uint16_t deleted_offset = (static_cast<uint16_t>(data[44]) << 8) | data[45];

    if (deleted_offset == 0 || deleted_offset >= page_size_) return;

    // Follow the deleted record chain
    size_t current_offset = deleted_offset;
    int iterations = 0;

    while (current_offset > 100 && current_offset < page_size_ - 50 && iterations < 1000) {
        iterations++;

        // Try to parse column record
        // SYS_COLUMNS format: TABLE_ID, POS, NAME, MTYPE, PRTYPE, LEN

        for (size_t probe = current_offset; probe < std::min<size_t>(current_offset + 100, page_size_ - 30); probe++) {
            // Look for table_id pattern (8 bytes big-endian)
            if (probe + 20 > page_size_) break;

            uint64_t potential_table_id = read_be_u64(data + probe);

            // Validate table_id range (user tables typically have IDs > 1)
            if (potential_table_id > 1 && potential_table_id < 1000000000) {
                // Try to parse column position
                uint32_t pos_val = read_be_u32(data + probe + 8);

                // Skip virtual columns (pos >> 16 != 0)
                if ((pos_val >> 16) == 0 && pos_val < 1000) {
                    // Try to find column name (variable length string)
                    for (size_t name_probe = probe + 12; name_probe < probe + 40 && name_probe < page_size_ - 20; name_probe++) {
                        uint8_t name_len = data[name_probe];
                        if (name_len > 0 && name_len < 100 && name_probe + name_len < page_size_) {
                            std::string col_name(reinterpret_cast<const char*>(data + name_probe + 1), name_len);

                            // Skip system columns
                            if (col_name != "DB_TRX_ID" && col_name != "DB_ROLL_PTR" &&
                                col_name != "DB_ROW_ID" && !col_name.empty()) {

                                SysColumnEntry col;
                                col.table_id = potential_table_id;
                                col.pos = pos_val;
                                col.name = col_name;

                                // Parse mtype, prtype, len
                                size_t field_pos = name_probe + 1 + name_len;
                                if (field_pos + 12 <= page_size_) {
                                    col.mtype = read_be_u32(data + field_pos);
                                    col.prtype = read_be_u32(data + field_pos + 4);
                                    col.len = read_be_u32(data + field_pos + 8);

                                    // Parse prtype
                                    ParsedPrType parsed = parse_prtype(col.prtype);
                                    col.is_nullable = parsed.is_nullable;
                                    col.is_unsigned = parsed.is_unsigned;

                                    columns_.push_back(col);
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        // Move to next deleted record
        if (current_offset + 2 < page_size_) {
            uint16_t next_offset = (static_cast<uint16_t>(data[current_offset]) << 8) | data[current_offset + 1];
            if (next_offset == 0 || next_offset >= current_offset) break;
            current_offset = next_offset;
        } else {
            break;
        }
    }
}

void SystemTablespaceScanner::parse_sys_indexes_page_deleted(const uint8_t* data) {
    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    if (page_type != 0x45BF) return;

    uint16_t deleted_offset = (static_cast<uint16_t>(data[44]) << 8) | data[45];

    if (deleted_offset == 0 || deleted_offset >= page_size_) return;

    size_t current_offset = deleted_offset;
    int iterations = 0;

    while (current_offset > 100 && current_offset < page_size_ - 50 && iterations < 1000) {
        iterations++;

        // SYS_INDEXES format: TABLE_ID, ID, NAME, N_FIELDS, TYPE, SPACE, PAGE_NO

        for (size_t probe = current_offset; probe < std::min<size_t>(current_offset + 100, page_size_ - 30); probe++) {
            if (probe + 30 > page_size_) break;

            uint64_t potential_table_id = read_be_u64(data + probe);

            if (potential_table_id > 1 && potential_table_id < 1000000000) {
                uint64_t potential_index_id = read_be_u64(data + probe + 8);

                if (potential_index_id > 0 && potential_index_id < 1000000000) {
                    // Try to find index name
                    for (size_t name_probe = probe + 16; name_probe < probe + 50 && name_probe < page_size_ - 20; name_probe++) {
                        uint8_t name_len = data[name_probe];
                        if (name_len > 0 && name_len < 100 && name_probe + name_len < page_size_) {
                            std::string idx_name(reinterpret_cast<const char*>(data + name_probe + 1), name_len);

                            if (!idx_name.empty()) {
                                SysIndexEntry idx;
                                idx.table_id = potential_table_id;
                                idx.index_id = potential_index_id;
                                idx.name = idx_name;
                                idx.deleted = true;

                                // Parse remaining fields
                                size_t field_pos = name_probe + 1 + name_len;
                                if (field_pos + 16 <= page_size_) {
                                    // Skip N_FIELDS (4 bytes)
                                    idx.type = read_be_u32(data + field_pos + 4);
                                    idx.space_id = read_be_u32(data + field_pos + 8);
                                    idx.page_no = read_be_u32(data + field_pos + 12);
                                }

                                indexes_.push_back(idx);
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (current_offset + 2 < page_size_) {
            uint16_t next_offset = (static_cast<uint16_t>(data[current_offset]) << 8) | data[current_offset + 1];
            if (next_offset == 0 || next_offset >= current_offset) break;
            current_offset = next_offset;
        } else {
            break;
        }
    }
}

void SystemTablespaceScanner::parse_dd_records_deleted(
    const uint8_t* data,
    const std::string& table_name,
    std::map<uint64_t, std::map<std::string, std::string>>& records) {

    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    if (page_type != 0x45BF) return;

    uint16_t deleted_offset = (static_cast<uint16_t>(data[44]) << 8) | data[45];

    if (deleted_offset == 0 || deleted_offset >= page_size_) return;

    // For mysql.tables table, parse id and name fields
    // The format is different from 5.7 SYS_TABLES
    // We need to parse based on the DD table schema

    // Simplified approach: scan for id and name patterns
    size_t current_offset = deleted_offset;
    int iterations = 0;

    while (current_offset > 100 && current_offset < page_size_ - 50 && iterations < 1000) {
        iterations++;

        if (table_name == "tables") {
            // Look for id (first column, primary key)
            // In DD format, id is typically at the beginning
            for (size_t probe = current_offset; probe < std::min<size_t>(current_offset + 200, page_size_ - 50); probe++) {
                // Look for a valid table_id followed by table name pattern
                if (probe + 20 > page_size_) break;

                // Try to find table name (schema/table format)
                for (size_t name_start = probe; name_start < probe + 100 && name_start < page_size_ - 30; name_start++) {
                    uint8_t len = data[name_start];
                    if (len > 0 && len < 200 && name_start + len < page_size_) {
                        std::string name(reinterpret_cast<const char*>(data + name_start + 1), len);
                        size_t slash_pos = name.find('/');

                        if (slash_pos != std::string::npos && slash_pos > 0 && slash_pos < name.length() - 1) {
                            std::string schema = name.substr(0, slash_pos);
                            std::string table = name.substr(slash_pos + 1);

                            // Validate this looks like a real table name
                            if (schema != "mysql" && schema != "sys" && schema != "information_schema" &&
                                schema != "performance_schema" && !schema.empty() && !table.empty()) {

                                // Try to find id (should be before or after name)
                                // Look for 8-byte value that could be table_id
                                for (size_t id_probe = probe; id_probe < name_start; id_probe++) {
                                    if (id_probe + 8 > page_size_) break;
                                    uint64_t potential_id = read_be_u64(data + id_probe);

                                    if (potential_id > 1 && potential_id < 1000000000) {
                                        records[potential_id]["name"] = name;
                                        records[potential_id]["schema"] = schema;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (current_offset + 2 < page_size_) {
            uint16_t next_offset = (static_cast<uint16_t>(data[current_offset]) << 8) | data[current_offset + 1];
            if (next_offset == 0 || next_offset >= current_offset) break;
            current_offset = next_offset;
        } else {
            break;
        }
    }
}

void SystemTablespaceScanner::parse_dd_records_deleted_columns(
    const uint8_t* data,
    std::map<uint64_t, std::vector<std::map<std::string, std::string>>>& columns_data) {

    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    if (page_type != 0x45BF) return;

    uint16_t deleted_offset = (static_cast<uint16_t>(data[44]) << 8) | data[45];
    if (deleted_offset == 0 || deleted_offset >= page_size_) return;

    size_t current_offset = deleted_offset;
    int iterations = 0;

    while (current_offset > 100 && current_offset < page_size_ - 50 && iterations < 1000) {
        iterations++;

        // Parse mysql.columns records
        // Key fields: table_id, name, ordinal_position, type, char_length, etc.

        for (size_t probe = current_offset; probe < std::min<size_t>(current_offset + 200, page_size_ - 50); probe++) {
            if (probe + 20 > page_size_) break;

            // Look for table_id
            uint64_t potential_table_id = read_be_u64(data + probe);

            if (potential_table_id > 1 && potential_table_id < 1000000000) {
                // Try to find column name
                for (size_t name_probe = probe + 8; name_probe < probe + 100 && name_probe < page_size_ - 20; name_probe++) {
                    uint8_t name_len = data[name_probe];
                    if (name_len > 0 && name_len < 100 && name_probe + name_len < page_size_) {
                        std::string col_name(reinterpret_cast<const char*>(data + name_probe + 1), name_len);

                        if (!col_name.empty() && col_name != "DB_TRX_ID" && col_name != "DB_ROLL_PTR") {
                            std::map<std::string, std::string> col_data;
                            col_data["name"] = col_name;

                            // Try to parse additional fields
                            size_t field_pos = name_probe + 1 + name_len;

                            // ordinal_position might be stored
                            // For simplicity, assign based on position in array
                            columns_data[potential_table_id].push_back(col_data);
                            break;
                        }
                    }
                }
            }
        }

        if (current_offset + 2 < page_size_) {
            uint16_t next_offset = (static_cast<uint16_t>(data[current_offset]) << 8) | data[current_offset + 1];
            if (next_offset == 0 || next_offset >= current_offset) break;
            current_offset = next_offset;
        } else {
            break;
        }
    }
}

void SystemTablespaceScanner::parse_dd_records_deleted_indexes(
    const uint8_t* data,
    std::map<uint64_t, std::vector<std::map<std::string, std::string>>>& indexes_data) {

    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];
    if (page_type != 0x45BF) return;

    uint16_t deleted_offset = (static_cast<uint16_t>(data[44]) << 8) | data[45];
    if (deleted_offset == 0 || deleted_offset >= page_size_) return;

    size_t current_offset = deleted_offset;
    int iterations = 0;

    while (current_offset > 100 && current_offset < page_size_ - 50 && iterations < 1000) {
        iterations++;

        // Parse mysql.indexes records
        // Key fields: table_id, id, name, type, se_private_data

        for (size_t probe = current_offset; probe < std::min<size_t>(current_offset + 200, page_size_ - 50); probe++) {
            if (probe + 30 > page_size_) break;

            uint64_t potential_table_id = read_be_u64(data + probe);

            if (potential_table_id > 1 && potential_table_id < 1000000000) {
                uint64_t potential_index_id = read_be_u64(data + probe + 8);

                if (potential_index_id > 0) {
                    // Try to find index name
                    for (size_t name_probe = probe + 16; name_probe < probe + 80 && name_probe < page_size_ - 20; name_probe++) {
                        uint8_t name_len = data[name_probe];
                        if (name_len > 0 && name_len < 100 && name_probe + name_len < page_size_) {
                            std::string idx_name(reinterpret_cast<const char*>(data + name_probe + 1), name_len);

                            if (!idx_name.empty()) {
                                std::map<std::string, std::string> idx_data;
                                idx_data["name"] = idx_name;
                                idx_data["id"] = std::to_string(potential_index_id);

                                indexes_data[potential_table_id].push_back(idx_data);
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (current_offset + 2 < page_size_) {
            uint16_t next_offset = (static_cast<uint16_t>(data[current_offset]) << 8) | data[current_offset + 1];
            if (next_offset == 0 || next_offset >= current_offset) break;
            current_offset = next_offset;
        } else {
            break;
        }
    }
}

catalog::TableSchema SystemTablespaceScanner::build_schema_from_dd(
    const std::string& table_name,
    const std::vector<SysColumnEntry>& cols) {

    catalog::TableSchema schema;
    schema.table_name = table_name;

    int ordinal = 0;
    for (const auto& col : cols) {
        catalog::ColumnInfo column;
        column.name = col.name;
        column.mysql_type_id = col.mtype;
        column.is_nullable = col.is_nullable;
        column.column_length = col.len;
        schema.columns[ordinal++] = column;
    }

    return schema;
}

} // namespace engine
} // namespace dbrecovery