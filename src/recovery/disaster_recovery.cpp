#include "dbrecovery/recovery/disaster_recovery.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/redo_log_forward.hpp"
#include "dbrecovery/utils/innodb_checksum.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <set>
#include <memory>
#include <mutex>
#include <future>
#include <queue>
#include <condition_variable>

namespace dbrecovery {
namespace recovery {

// ==================== DisasterRecovery 实现 ====================

DisasterRecovery::DisasterRecovery(uint32_t page_size)
    : page_size_(page_size), multithreaded_(true), thread_count_(4),
      blob_recovery_(true), redo_log_recovery_(false) {
    std::memset(&stats_, 0, sizeof(stats_));
}

DisasterRecovery::~DisasterRecovery() = default;

void DisasterRecovery::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

void DisasterRecovery::set_output_directory(const std::string& dir) {
    output_dir_ = dir;
}

void DisasterRecovery::set_export_csv(bool enabled) {
    export_csv_ = enabled;
}

void DisasterRecovery::set_export_json(bool enabled) {
    export_json_ = enabled;
}

void DisasterRecovery::set_export_sql(bool enabled) {
    export_sql_ = enabled;
}

void DisasterRecovery::set_multithreaded(bool enabled) {
    multithreaded_ = enabled;
}

void DisasterRecovery::set_thread_count(uint32_t threads) {
    thread_count_ = threads > 0 ? threads : 4;
}

void DisasterRecovery::set_blob_recovery(bool enabled) {
    blob_recovery_ = enabled;
}

void DisasterRecovery::set_redo_log_recovery(bool enabled) {
    redo_log_recovery_ = enabled;
}

void DisasterRecovery::set_redo_log_path(const std::string& path) {
    redo_log_path_ = path;
}

void DisasterRecovery::clear() {
    scanned_tables_.clear();
    recovered_tables_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

bool DisasterRecovery::scan_data_directory() {
    auto start_time = std::chrono::high_resolution_clock::now();

    std::filesystem::path directory(data_dir_);

    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        std::cerr << "[ERROR] Data directory does not exist: " << data_dir_ << std::endl;
        return false;
    }

    std::cout << "[INFO] DisasterRecovery: Scanning data directory: " << data_dir_ << std::endl;

    bool found = scan_directory(data_dir_, 0);

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.scan_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    std::cout << "[INFO] DisasterRecovery: Scan completed. Found "
              << scanned_tables_.size() << " tables in "
              << stats_.scan_time_ms << " ms" << std::endl;

    return found;
}

bool DisasterRecovery::scan_directory(const std::string& dir_path, int depth) {
    bool found = false;
    std::filesystem::path dir(dir_path);

    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        return false;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            stats_.total_files_scanned++;

            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                std::string extension = entry.path().extension().string();

                // 统计文件类型
                if (extension == ".ibd") {
                    stats_.ibd_files_found++;
                    stats_.total_bytes_scanned += entry.file_size();

                    ScannedTable table;
                    table.ibd_path = entry.path().string();
                    table.total_pages = static_cast<uint32_t>(entry.file_size() / page_size_);

                    // 分析文件
                    if (analyze_ibd_file(entry.path().string(), table)) {
                        // 从路径推断数据库名和表名
                        std::filesystem::path p(entry.path().string());
                        std::string filename = p.filename().string();
                        if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
                            filename = filename.substr(0, filename.size() - 4);
                        }
                        std::string db_name = p.parent_path().filename().string();

                        table.database_name = db_name;
                        table.table_name = filename;

                        scanned_tables_.push_back(table);
                        found = true;
                    }
                } else if (extension == ".sdi") {
                    stats_.sdi_files_found++;
                }
            } else if (entry.is_directory() && depth < 10) {
                // 递归扫描子目录
                if (scan_directory(entry.path().string(), depth + 1)) {
                    found = true;
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[WARN] Filesystem error: " << e.what() << std::endl;
    }

    return found;
}

bool DisasterRecovery::analyze_ibd_file(const std::string& path, ScannedTable& table) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        return false;
    }

    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);
    std::vector<uint8_t> page_data(page_size_);

    uint32_t valid_pages = 0;
    uint32_t index_pages = 0;
    uint32_t sdi_pages = 0;
    uint32_t undo_pages = 0;
    uint32_t blob_pages = 0;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 16);

        // 检查页类型
        if (page_type == dbrecovery::innodb::FIL_PAGE_INDEX) {
            valid_pages++;
            index_pages++;

            // 检查页健康
            if (dbrecovery::utils::InnoDBChecksum::validate_crc32(page_data.data(), page_size_)) {
                stats_.pages_with_data++;
            }
        } else if (page_type == dbrecovery::innodb::FIL_PAGE_SDI) {
            valid_pages++;
            sdi_pages++;
            stats_.pages_with_data++;
        } else if (page_type == dbrecovery::innodb::FIL_PAGE_UNDO_LOG) {
            valid_pages++;
            undo_pages++;
        } else if (page_type == dbrecovery::innodb::FIL_PAGE_TYPE_BLOB) {
            valid_pages++;
            blob_pages++;
        }
    }

    table.valid_pages = valid_pages;
    table.index_pages = index_pages;
    table.sdi_pages = sdi_pages;
    table.undo_pages = undo_pages;
    table.blob_pages = blob_pages;
    table.estimated_row_count = valid_pages * 100;  // 粗略估计

    // 尝试从 SDI 解析表结构
    auto schema_opt = parse_schema_from_sdi(path);
    if (schema_opt.has_value()) {
        table.schema = schema_opt.value();
        table.has_sdi = true;
    } else {
        table.has_sdi = false;
    }

    stats_.pages_analyzed += total_pages;

    return valid_pages > 0;
}

std::optional<catalog::TableSchema> DisasterRecovery::parse_schema_from_sdi(const std::string& ibd_path) {
    dbrecovery::innodb::InnoDBFileReader reader(ibd_path);
    if (!reader.is_valid()) {
        return std::nullopt;
    }

    uint32_t total_pages = reader.get_total_pages();

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        auto page_data = reader.read_page_raw(page_no);
        if (page_data.empty()) continue;

        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 16);

        if (page_type == dbrecovery::innodb::FIL_PAGE_SDI) {
            const uint8_t* sdi_payload = page_data.data() + 38;
            if (page_data.size() < 38 + 20) continue;

            auto json_opt = dbrecovery::innodb::SDIManager::extract_sdi_json(
                sdi_payload, page_data.size() - 38);

            if (json_opt.has_value()) {
                try {
                    auto json = nlohmann::json::parse(json_opt.value());
                    catalog::TableSchema schema;

                    if (schema.from_json(json)) {
                        return schema;
                    }
                } catch (const nlohmann::json::parse_error& e) {
                    // SDI JSON 解析失败是正常的（文件可能已损坏）
                }
            }
        }
    }

    return std::nullopt;
}

bool DisasterRecovery::recover_single_table(const ScannedTable& table, RecoveredTable& recovered) {
    auto start_time = std::chrono::high_resolution_clock::now();

    recovered.database_name = table.database_name;
    recovered.table_name = table.table_name;
    recovered.ibd_path = table.ibd_path;
    recovered.table_id = table.table_id;
    recovered.schema = table.schema;
    recovered.total_pages = table.total_pages;
    recovered.valid_pages = table.valid_pages;
    recovered.success = true;

    try {
        // 使用 B+Tree 解析器恢复记录
        std::ifstream file(table.ibd_path, std::ios::binary | std::ios::ate);

        if (!file) {
            recovered.success = false;
            recovered.error_message = "Failed to open file";
            return false;
        }

        uint64_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> page_data(page_size_);

        for (uint32_t page_no = 0; page_no < table.total_pages; ++page_no) {
            file.seekg(page_no * page_size_, std::ios::beg);
            file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

            if (page_data.size() < 38) continue;

            uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 16);

            if (page_type == dbrecovery::innodb::FIL_PAGE_INDEX) {
                // 解析页
                dbrecovery::innodb::BPlusTreeParser page_parser(page_data);
                if (page_parser.parse()) {
                    auto records = page_parser.parse_records(table.schema);
                    recovered.records.insert(recovered.records.end(),
                                           records.begin(), records.end());
                }
            }
        }

        recovered.record_count = recovered.records.size();
        recovered.data_size_bytes = file_size;

        // 导出数据
        if (export_csv_) {
            recovered.output_csv = output_dir_ + "/" + recovered.database_name +
                                  "_" + recovered.table_name + ".csv";
            export_to_csv(recovered.records, recovered.schema, recovered.output_csv);
        }

        if (export_json_) {
            recovered.output_json = output_dir_ + "/" + recovered.database_name +
                                   "_" + recovered.table_name + ".json";
            export_to_json(recovered.records, recovered.schema, recovered.output_json);
        }

        if (export_sql_) {
            recovered.output_sql = output_dir_ + "/" + recovered.database_name +
                                  "_" + recovered.table_name + ".sql";
            export_to_sql(recovered.records, recovered.schema, recovered.output_sql);
        }

        stats_.tables_recovered++;
        stats_.total_records_recovered += recovered.record_count;
        stats_.total_data_size_bytes += recovered.data_size_bytes;

    } catch (const std::exception& e) {
        recovered.success = false;
        recovered.error_message = e.what();
        stats_.tables_failed++;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms += std::chrono::duration<double, std::milli>(
        end_time - start_time).count();

    return recovered.success;
}

bool DisasterRecovery::export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                                      const catalog::TableSchema& schema, const std::string& path) {
    std::ofstream file(path);
    if (!file) {
        return false;
    }

    // 写入表头
    std::vector<std::string> column_names;
    for (const auto& pair : schema.columns) {
        column_names.push_back(pair.second.name);
    }

    for (size_t i = 0; i < column_names.size(); ++i) {
        if (i > 0) file << ",";
        file << column_names[i];
    }
    file << "\n";

    // 写入数据
    for (const auto& record : records) {
        for (size_t i = 0; i < column_names.size(); ++i) {
            if (i > 0) file << ",";
            auto it = record.find(column_names[i]);
            if (it != record.end()) {
                file << it->second;
            }
        }
        file << "\n";
    }

    return true;
}

bool DisasterRecovery::export_to_json(const std::vector<std::map<std::string, std::string>>& records,
                                       const catalog::TableSchema& schema, const std::string& path) {
    std::ofstream file(path);
    if (!file) {
        return false;
    }

    nlohmann::json json;
    json["database"] = schema.table_name;
    json["table"] = schema.table_name;
    json["record_count"] = records.size();

    nlohmann::json data_array = nlohmann::json::array();
    for (const auto& record : records) {
        nlohmann::json row;
        for (const auto& [key, value] : record) {
            row[key] = value;
        }
        data_array.push_back(row);
    }
    json["data"] = data_array;

    file << json.dump(2);
    return true;
}

bool DisasterRecovery::export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                                      const catalog::TableSchema& schema, const std::string& path) {
    std::ofstream file(path);
    if (!file) {
        return false;
    }

    // 写入 CREATE TABLE 语句
    file << "-- Recovered table: " << schema.table_name << "\n";
    file << "-- Database: " << schema.table_name << "\n\n";

    file << "CREATE TABLE `" << schema.table_name << "` (\n";

    size_t col_idx = 0;
    for (const auto& pair : schema.columns) {
        const auto& col = pair.second;
        if (col_idx > 0) file << ",\n";

        file << "  `" << col.name << "` ";

        // 类型映射
        switch (col.mysql_type_id) {
            case dbrecovery::innodb::mysql_types::LONGLONG:
                file << "BIGINT";
                break;
            case dbrecovery::innodb::mysql_types::LONG:
                file << "INT";
                break;
            case dbrecovery::innodb::mysql_types::VARCHAR:
                file << "VARCHAR(" << col.column_length << ")";
                break;
            case dbrecovery::innodb::mysql_types::DATETIME:
                file << "DATETIME";
                break;
            case dbrecovery::innodb::mysql_types::DATE:
                file << "DATE";
                break;
            default:
                file << "VARCHAR(255)";
                break;
        }

        if (!col.is_nullable) {
            file << " NOT NULL";
        }
        col_idx++;
    }

    file << "\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\n\n";

    // 写入 INSERT 语句
    for (const auto& record : records) {
        std::vector<std::string> values;
        for (const auto& pair : schema.columns) {
            auto it = record.find(pair.second.name);
            if (it != record.end()) {
                std::string value = it->second;
                // 转义单引号
                for (size_t i = 0; i < value.size(); ++i) {
                    if (value[i] == '\'') {
                        value.insert(i, "'");
                        ++i;
                    }
                }
                values.push_back("'" + value + "'");
            } else {
                values.push_back("NULL");
            }
        }
        file << "INSERT INTO `" << schema.table_name << "` VALUES (" << values[0];
        for (size_t i = 1; i < values.size(); ++i) {
            file << ", " << values[i];
        }
        file << ");\n";
    }

    return true;
}

void DisasterRecovery::recover_multithreaded() {
    if (scanned_tables_.empty()) return;

    std::vector<std::future<void>> futures;
    uint32_t tables_per_thread = (scanned_tables_.size() + thread_count_ - 1) / thread_count_;

    for (uint32_t i = 0; i < thread_count_; ++i) {
        uint32_t start_idx = i * tables_per_thread;
        uint32_t end_idx = std::min(start_idx + tables_per_thread, (uint32_t)scanned_tables_.size());

        if (start_idx >= scanned_tables_.size()) break;

        futures.push_back(std::async(std::launch::async, [this, start_idx, end_idx]() {
            for (uint32_t idx = start_idx; idx < end_idx; ++idx) {
                RecoveredTable recovered;
                recover_single_table(scanned_tables_[idx], recovered);
                recovered_tables_.push_back(recovered);
            }
        }));
    }

    for (auto& f : futures) {
        f.get();
    }
}

void DisasterRecovery::recover_singlethreaded() {
    for (const auto& table : scanned_tables_) {
        RecoveredTable recovered;
        recover_single_table(table, recovered);
        recovered_tables_.push_back(recovered);
    }
}

std::vector<RecoveredTable> DisasterRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 扫描数据目录
    if (!scan_data_directory()) {
        std::cerr << "[WARN] No tables found in data directory" << std::endl;
        return recovered_tables_;
    }

    // 恢复所有表
    if (multithreaded_) {
        recover_multithreaded();
    } else {
        recover_singlethreaded();
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(
        total_end - total_start).count();

    return recovered_tables_;
}

bool DisasterRecovery::recover_from_ibd_file(const std::string& ibd_path, RecoveredTable& table) {
    ScannedTable scanned;
    scanned.ibd_path = ibd_path;

    if (!analyze_ibd_file(ibd_path, scanned)) {
        return false;
    }

    // 从路径推断数据库名和表名
    std::filesystem::path p(ibd_path);
    std::string filename = p.filename().string();
    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
        filename = filename.substr(0, filename.size() - 4);
    }
    std::string db_name = p.parent_path().filename().string();

    scanned.database_name = db_name;
    scanned.table_name = filename;

    return recover_single_table(scanned, table);
}

// ==================== CompleteRecovery 实现 ====================

CompleteRecovery::CompleteRecovery() {
    std::memset(&stats_, 0, sizeof(stats_));
}

CompleteRecovery::~CompleteRecovery() = default;

void CompleteRecovery::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

void CompleteRecovery::set_output_directory(const std::string& dir) {
    output_dir_ = dir;
}

void CompleteRecovery::set_scan_all_devices(bool enabled) {
    scan_all_devices_ = enabled;
}

void CompleteRecovery::set_recover_all(bool enabled) {
    recover_all_ = enabled;
}

void CompleteRecovery::set_table_filter(const std::string& pattern) {
    table_filter_ = pattern;
}

uint32_t CompleteRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    // 扫描数据目录
    if (!scan_data_directory()) {
        std::cerr << "[WARN] No tables found in data directory" << std::endl;
        return 0;
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(
        total_end - total_start).count();

    return static_cast<uint32_t>(recovered_tables_.size());
}

void CompleteRecovery::clear() {
    recovered_tables_.clear();
    errors_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

bool CompleteRecovery::scan_data_directory() {
    std::filesystem::path directory(data_dir_);

    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        return false;
    }

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();

                if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
                    // 尝试从文件名推断表结构
                    auto schema_opt = infer_schema_from_filename(entry.path().string());

                    if (schema_opt.has_value()) {
                        // 恢复表
                        if (recover_single_table(entry.path().string(), schema_opt.value())) {
                            stats_.tables_recovered++;
                        } else {
                            errors_.push_back("Failed to recover: " + entry.path().string());
                            stats_.tables_failed++;
                        }
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        errors_.push_back("Filesystem error: " + std::string(e.what()));
    }

    return stats_.tables_recovered > 0;
}

std::optional<catalog::TableSchema> CompleteRecovery::infer_schema_from_filename(const std::string& path) {
    std::filesystem::path p(path);
    std::string filename = p.filename().string();

    // 移除 .ibd 扩展名
    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
        filename = filename.substr(0, filename.size() - 4);
    }

    catalog::TableSchema schema;
    schema.table_name = filename;
    schema.row_format = 2;  // DYNAMIC
    schema.page_size = 16384;

    // 尝试从文件名推断列名（假设表名是复数形式）
    // 例如: users -> id, name, email, created_at
    if (filename.size() > 3 && filename.substr(filename.size() - 3) == "ies") {
        // users -> user
        std::string singular = filename.substr(0, filename.size() - 3) + "y";
        schema.table_name = singular;
    } else if (filename.size() > 1 && filename.back() == 's') {
        // tables -> table
        schema.table_name = filename.substr(0, filename.size() - 1);
    }

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
    col3.name = "created_at";
    col3.mysql_type_id = dbrecovery::innodb::mysql_types::DATETIME;
    col3.is_nullable = true;
    schema.columns[3] = col3;

    return schema;
}

bool CompleteRecovery::recover_single_table(const std::string& ibd_path,
                                            const catalog::TableSchema& schema) {
    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }

    uint64_t file_size = file.tellg();
    uint32_t total_pages = static_cast<uint32_t>(file_size / 16384);

    RecoveredTable recovered;
    recovered.ibd_path = ibd_path;
    recovered.schema = schema;
    recovered.total_pages = total_pages;

    std::vector<uint8_t> page_data(16384);

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * 16384, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

        if (page_data.size() < 38) continue;

        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 16);

        if (page_type == dbrecovery::innodb::FIL_PAGE_INDEX) {
            dbrecovery::innodb::BPlusTreeParser parser(page_data);
            if (parser.parse()) {
                auto records = parser.parse_records(schema);
                recovered.records.insert(recovered.records.end(),
                                       records.begin(), records.end());
            }
        }
    }

    recovered.record_count = recovered.records.size();
    recovered.data_size_bytes = file_size;
    recovered.success = true;

    // 导出数据
    std::string db_name = "recovered";
    std::string tbl_name = schema.table_name;

    export_recovered_data(db_name, tbl_name, recovered.records, schema);

    recovered_tables_.push_back(recovered);
    stats_.total_records_recovered += recovered.record_count;

    return true;
}

bool CompleteRecovery::export_recovered_data(const std::string& database,
                                             const std::string& table,
                                             const std::vector<std::map<std::string, std::string>>& records,
                                             const catalog::TableSchema& schema) {
    // 导出为 CSV
    std::string csv_path = output_dir_ + "/" + database + "_" + table + ".csv";
    std::ofstream csv_file(csv_path);
    if (csv_file) {
        csv_file << "id,name,created_at\n";
        for (const auto& record : records) {
            csv_file << record.at("id") << "," << record.at("name") << "," << record.at("created_at") << "\n";
        }
        csv_file.close();
    }

    // 导出为 JSON
    std::string json_path = output_dir_ + "/" + database + "_" + table + ".json";
    std::ofstream json_file(json_path);
    if (json_file) {
        nlohmann::json json;
        json["database"] = database;
        json["table"] = table;
        json["record_count"] = records.size();

        nlohmann::json data_array = nlohmann::json::array();
        for (const auto& record : records) {
            nlohmann::json row;
            for (const auto& [key, value] : record) {
                row[key] = value;
            }
            data_array.push_back(row);
        }
        json["data"] = data_array;

        json_file << json.dump(2);
        json_file.close();
    }

    return true;
}

// ==================== FastRecovery 实现 ====================

FastRecovery::FastRecovery() {
    std::memset(&stats_, 0, sizeof(stats_));
}

FastRecovery::~FastRecovery() = default;

void FastRecovery::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

void FastRecovery::set_output_directory(const std::string& dir) {
    output_dir_ = dir;
}

void FastRecovery::set_skip_validation(bool enabled) {
    skip_validation_ = enabled;
}

void FastRecovery::set_records_only(bool enabled) {
    records_only_ = enabled;
}

uint64_t FastRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    if (!scan_and_recover_records()) {
        return 0;
    }

    if (!records_only_) {
        export_all_records();
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(
        total_end - total_start).count();

    return all_records_.size();
}

void FastRecovery::clear() {
    all_records_.clear();
    table_records_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

bool FastRecovery::scan_and_recover_records() {
    std::filesystem::path directory(data_dir_);

    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        return false;
    }

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();

                if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
                    stats_.total_files_scanned++;
                    stats_.total_bytes_scanned += entry.file_size();

                    if (recover_records_from_ibd(entry.path().string())) {
                        stats_.ibd_files_found++;
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[WARN] Filesystem error: " << e.what() << std::endl;
    }

    return !all_records_.empty();
}

bool FastRecovery::recover_records_from_ibd(const std::string& ibd_path) {
    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }

    uint64_t file_size = file.tellg();
    uint32_t total_pages = static_cast<uint32_t>(file_size / 16384);

    // 推断表结构
    catalog::TableSchema schema;
    schema.table_name = std::filesystem::path(ibd_path).filename().string().substr(0, 4);
    schema.row_format = 2;
    schema.page_size = 16384;

    dbrecovery::catalog::ColumnInfo col1;
    col1.name = "id";
    col1.mysql_type_id = dbrecovery::innodb::mysql_types::LONGLONG;
    col1.is_nullable = false;
    col1.column_length = 8;
    schema.columns[1] = col1;

    dbrecovery::catalog::ColumnInfo col2;
    col2.name = "data";
    col2.mysql_type_id = dbrecovery::innodb::mysql_types::VARCHAR;
    col2.is_nullable = true;
    col2.column_length = 255;
    schema.columns[2] = col2;

    std::vector<uint8_t> page_data(16384);

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * 16384, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

        if (page_data.size() < 38) continue;

        uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data.data(), 16);

        if (page_type == dbrecovery::innodb::FIL_PAGE_INDEX) {
            auto records = recover_records_from_page(page_data, schema);
            all_records_.insert(all_records_.end(), records.begin(), records.end());
        }
    }

    return true;
}

std::vector<std::map<std::string, std::string>> FastRecovery::recover_records_from_page(
    const std::vector<uint8_t>& page_data, const catalog::TableSchema& schema) {
    std::vector<std::map<std::string, std::string>> records;

    dbrecovery::innodb::BPlusTreeParser parser(page_data);
    if (parser.parse()) {
        records = parser.parse_records(schema);
    }

    return records;
}

bool FastRecovery::export_all_records() {
    // 导出为 JSON
    std::string json_path = output_dir_ + "/all_records.json";
    std::ofstream json_file(json_path);
    if (json_file) {
        nlohmann::json json;
        json["total_records"] = all_records_.size();
        json["records"] = nlohmann::json::array();

        for (const auto& record : all_records_) {
            nlohmann::json row;
            for (const auto& [key, value] : record) {
                row[key] = value;
            }
            json["records"].push_back(row);
        }

        json_file << json.dump(2);
        json_file.close();
    }

    return true;
}

} // namespace recovery
} // namespace dbrecovery
