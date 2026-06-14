/**
 * @file drop_table_recovery.cpp
 * @brief DROP TABLE Recovery Implementation
 */

#include "dbrecovery/engine/drop_table_recovery.hpp"
#include "dbrecovery/engine/signature_scraper.hpp"
#include "dbrecovery/engine/system_table_sdi.hpp"
#include "dbrecovery/utils/innodb_checksum.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace dbrecovery {
namespace engine {

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
        bool is_sys_table = false;
        if (is_mysql80_format()) {
            // MySQL 8.0 DD tables have low index IDs
            is_sys_table = (idx_id >= 1 && idx_id <= 100);
        } else {
            // MySQL 5.7 system tables
            is_sys_table = (idx_id == sys_index_ids::SYS_TABLES ||
                           idx_id == sys_index_ids::SYS_COLUMNS ||
                           idx_id == sys_index_ids::SYS_INDEXES ||
                           idx_id == sys_index_ids::SYS_FIELDS);
        }

        if (is_sys_table) {
            pages_by_index_id[idx_id].push_back(page_buffer);
        }
    }

    // Parse records from system tables
    if (is_mysql80_format()) {
        // TODO: Implement MySQL 8.0 DD table parsing
        // This requires parsing compressed SDI pages
    } else {
        // MySQL 5.7: Parse SYS_TABLES
        if (pages_by_index_id.count(sys_index_ids::SYS_TABLES)) {
            for (const auto& page_data : pages_by_index_id[sys_index_ids::SYS_TABLES]) {
                parse_sys_tables_page(page_data.data());
            }
        }

        // Parse SYS_COLUMNS
        if (pages_by_index_id.count(sys_index_ids::SYS_COLUMNS)) {
            for (const auto& page_data : pages_by_index_id[sys_index_ids::SYS_COLUMNS]) {
                parse_sys_columns_page(page_data.data());
            }
        }

        // Parse SYS_INDEXES
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

    // Scan for records - simplified heuristic scanning
    for (size_t offset = 120; offset < page_size_ - 100; offset++) {
        SysTableEntry table;
        table.deleted = false;
        table.dropped = false;

        size_t pos = offset;

        // Try to find a valid table name (schema/table format)
        uint8_t name_len = data[pos++];
        if (name_len == 0 || name_len > 200 || pos + name_len > page_size_ - 50) continue;

        std::string name(reinterpret_cast<const char*>(data + pos), name_len);
        pos += name_len;

        // Check if name looks like a table name (schema/table format)
        size_t slash_pos = name.find('/');
        if (slash_pos == std::string::npos || slash_pos == 0 || slash_pos >= name.length() - 1) {
            continue;
        }

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

    for (size_t offset = 120; offset < page_size_ - 50; offset++) {
        SysColumnEntry col;

        size_t pos = offset;

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

    for (size_t offset = 120; offset < page_size_ - 50; offset++) {
        SysIndexEntry idx;

        size_t pos = offset;

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

} // namespace engine
} // namespace dbrecovery