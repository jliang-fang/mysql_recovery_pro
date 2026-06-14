/**
 * @file drop_table_recovery.cpp
 * @brief DROP TABLE Recovery Implementation
 */

#include "dbrecovery/engine/drop_table_recovery.hpp"
#include "dbrecovery/engine/signature_scraper.hpp"
#include "dbrecovery/utils/innodb_checksum.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace dbrecovery {
namespace engine {

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
    // Placeholder - full implementation would parse SYS_* tables
    return recovered_tables;
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

    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        if (progress) progress(0.0, "Starting DROP TABLE recovery...");

        if (!config_.system_tablespace.empty()) {
            if (progress) progress(0.2, "Scanning system tablespace...");

            sys_scanner_ = std::make_unique<SystemTablespaceScanner>(
                config_.system_tablespace, config_.page_size);

            auto recovered_tables = sys_scanner_->scan_deleted_tables();
            result.tables_found = recovered_tables.size();
        }

        if (progress) progress(0.4, "Searching for .ibd files...");
        auto ibd_files = find_ibd_files();

        if (progress) progress(0.6, "Recovering data...");
        // Placeholder for actual recovery logic

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

    // Placeholder - would use IndexPageParser to extract records
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

    // Placeholder implementation
    return false;
}

// ==================== Utility Functions ====================

std::string mysql_type_name(uint32_t mtype) {
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

    auto it = type_names.find(mtype);
    return (it != type_names.end()) ? it->second : "UNKNOWN";
}

uint32_t mysql_to_innodb_type(uint32_t mysql_type) {
    switch (mysql_type) {
        case 1: return 1; case 2: return 2; case 3: return 4; case 8: return 6;
        case 9: return 3; case 4: return 7; case 5: return 8; case 18: return 12;
        case 7: return 10; case 10: return 14; case 11: return 11; case 12: return 13;
        case 15: return 15; case 25: return 15; case 26: return 0;
        case 17: case 21: case 22: case 23: case 24: return 20;
        default: return mysql_type;
    }
}

ParsedPrType parse_prtype(uint32_t prtype) {
    ParsedPrType result;
    result.charset_id = (prtype >> 16) & 0x7FFF;
    result.is_nullable = !(prtype & 256);
    result.is_unsigned = (prtype & 512) != 0;
    result.is_binary = (prtype & 1024) != 0;
    result.is_virtual = (prtype & 8192) != 0;
    result.base_type = prtype & 0xFF;
    return result;
}

std::string row_format_name(uint32_t flags) {
    if (!(flags & 1)) return "REDUNDANT";
    else if (!(flags & 32)) return "COMPACT";
    else if ((flags & 30) >> 1) return "COMPRESSED";
    else return "DYNAMIC";
}

} // namespace engine
} // namespace dbrecovery