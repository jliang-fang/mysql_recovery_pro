/**
 * @file drop_table_recovery.cpp
 * @brief DROP TABLE Recovery Implementation
 */

#include "dbrecovery/engine/drop_table_recovery.hpp"
#include "dbrecovery/engine/signature_scraper.hpp"
#include "dbrecovery/engine/system_table_sdi.hpp"
#include "dbrecovery/utils/innodb_checksum.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <set>
#include <nlohmann/json.hpp>
#include <zlib.h>

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
        mysql_version_ = "5.7";
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

        // Step 1: Parse SDI pages (pages 3 and 4) to get DD table index IDs
        std::map<std::string, uint64_t> dd_table_index_ids;
        std::map<uint64_t, std::string> index_id_to_table_name;

        // Parse SDI pages to extract DD table metadata
        for (uint64_t page_no = 0; page_no < std::min(num_pages, static_cast<uint64_t>(10)); ++page_no) {
            file.seekg(page_no * page_size_, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);
            if (!file) break;

            // Check for SDI page type
            uint16_t page_type = (static_cast<uint16_t>(page_buffer[24]) << 8) | page_buffer[25];

            if (page_type == 0x45BD) {  // FIL_PAGE_SDI_ZIP (compressed SDI)
                // Use ibd2sql approach: slot-based offset calculation
                int16_t slot_offset = static_cast<int16_t>(
                    (static_cast<uint16_t>(page_buffer[97]) << 8) |
                    static_cast<uint16_t>(page_buffer[98]));
                uint32_t record_offset = static_cast<uint32_t>(slot_offset + 99);

                if (record_offset > 0 && record_offset < page_size_ - 50) {
                    // Read lengths at offset+25 and offset+29 (big-endian)
                    uint32_t uncompressed_len = read_be_u32(page_buffer.data() + record_offset + 25);
                    uint32_t compressed_len = read_be_u32(page_buffer.data() + record_offset + 29);

                    if (compressed_len > 0 && compressed_len < page_size_ &&
                        record_offset + 33 + compressed_len <= page_size_) {

                        const uint8_t* compressed_data = page_buffer.data() + record_offset + 33;

                        // Decompress using standard zlib
                        z_stream strm = {};
                        strm.next_in = const_cast<Bytef*>(compressed_data);
                        strm.avail_in = compressed_len;

                        std::vector<uint8_t> decompressed(uncompressed_len > 0 ? uncompressed_len : compressed_len * 4);
                        strm.next_out = decompressed.data();
                        strm.avail_out = decompressed.size();

                        int ret = inflateInit(&strm);  // Standard zlib
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

                                try {
                                    auto json = nlohmann::json::parse(json_str);

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
                            inflateEnd(&strm);
                        }
                    }
                }
            }
        }

        // Step 2: Scan all INDEX pages for deleted records in DD tables
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

        if (!config_.system_tablespace.empty()) {
            if (progress) progress(0.2, "Scanning system tablespace...");

            sys_scanner_ = std::make_unique<SystemTablespaceScanner>(
                config_.system_tablespace, config_.page_size);

            auto recovered_tables = sys_scanner_->scan_deleted_tables();
            result.tables_found = recovered_tables.size();
            result.recovered_tables = recovered_tables;
        }

        if (progress) progress(0.4, "Searching for .ibd files...");
        auto ibd_files = find_ibd_files();

        if (progress) progress(0.6, "Recovering data...");
        // TODO: Implement actual data recovery from orphaned pages

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

    // TODO: Implement using PageParserV3 to extract records
    return {};
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

    // TODO: Implement IBD file recovery
    return false;
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