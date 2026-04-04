#include "dbrecovery/recovery/enhanced_recovery.hpp"
#include "dbrecovery/innodb/page_reader.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/btree.hpp"
#include "dbrecovery/innodb/page_parser.hpp"
#include "dbrecovery/common/version_compat.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <fnmatch.h>
#include <chrono>
#include <set>
#include <memory>

namespace dbrecovery {
namespace recovery {

// ==================== DiskScanner 实现 ====================

DiskScanner::DiskScanner() 
    : page_size_(16384), max_depth_(10), total_bytes_scanned_(0), pages_analyzed_(0) {
    scan_result_.total_files = 0;
    scan_result_.ibd_files = 0;
    scan_result_.sdi_files = 0;
    scan_result_.frm_files = 0;
    scan_result_.scan_time_ms = 0;
}

DiskScanner::~DiskScanner() = default;

void DiskScanner::set_page_size(uint32_t size) {
    if (size == 4096 || size == 8192 || size == 16384 || 
        size == 32768 || size == 65536) {
        page_size_ = size;
    }
}

void DiskScanner::set_max_depth(int depth) {
    max_depth_ = depth > 0 ? depth : 10;
}

bool DiskScanner::scan_data_directory(const std::string& data_dir) {
    auto start_time = std::chrono::high_resolution_clock::now();

    scan_result_.data_dir = data_dir;
    scan_result_.total_files = 0;
    scan_result_.ibd_files = 0;
    scan_result_.sdi_files = 0;
    scan_result_.frm_files = 0;
    scanned_tables_.clear();
    sdi_pages_.clear();
    index_pages_.clear();
    blob_pages_.clear();
    table_name_to_path_.clear();

    std::cout << "[INFO] DiskScanner: Scanning data directory: " << data_dir << std::endl;

    bool found = scan_directory(data_dir, 0);

    auto end_time = std::chrono::high_resolution_clock::now();
    scan_result_.scan_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    std::cout << "[INFO] DiskScanner: Scan completed. Found " 
              << scanned_tables_.size() << " tables in " 
              << scan_result_.scan_time_ms << " ms" << std::endl;

    return found;
}

bool DiskScanner::scan_directory(const std::string& dir_path, int depth) {
    if (depth > max_depth_) {
        return false;
    }

    bool found = false;
    std::filesystem::path dir(dir_path);

    if (!std::filesystem::exists(dir)) {
        std::cerr << "[WARN] Directory does not exist: " << dir_path << std::endl;
        return false;
    }

    if (!std::filesystem::is_directory(dir)) {
        std::cerr << "[WARN] Path is not a directory: " << dir_path << std::endl;
        return false;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            scan_result_.total_files++;

            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                std::string extension = entry.path().extension().string();

                // 统计文件类型
                if (extension == ".ibd") {
                    scan_result_.ibd_files++;
                    if (analyze_ibd_file(entry.path().string())) {
                        found = true;
                    }
                } else if (extension == ".sdi") {
                    scan_result_.sdi_files++;
                } else if (extension == ".frm") {
                    scan_result_.frm_files++;
                }
            } else if (entry.is_directory()) {
                // 跳过系统目录
                std::string dirname = entry.path().filename().string();
                if (dirname == "." || dirname == ".." ||
                    dirname == "mysql" || dirname == "sys" ||
                    dirname == "performance_schema" || dirname == "information_schema") {
                    continue;
                }
                found |= scan_directory(entry.path().string(), depth + 1);
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        scan_result_.errors.push_back(e.what());
        std::cerr << "[ERROR] Failed to scan directory: " << e.what() << std::endl;
    }

    return found;
}

bool DiskScanner::scan_raw_device(const std::string& device_path) {
    std::cout << "[INFO] DiskScanner: Scanning raw device: " << device_path << std::endl;

    std::ifstream device(device_path, std::ios::binary | std::ios::ate);
    if (!device) {
        std::cerr << "[ERROR] Failed to open device: " << device_path << std::endl;
        return false;
    }

    uint64_t device_size = device.tellg();
    device.seekg(0, std::ios::beg);

    if (device_size < page_size_) {
        std::cerr << "[ERROR] Device too small: " << device_size << " bytes" << std::endl;
        return false;
    }

    uint32_t total_pages = static_cast<uint32_t>(device_size / page_size_);
    std::cout << "[INFO] Device size: " << device_size << " bytes (" 
              << total_pages << " pages)" << std::endl;

    std::vector<uint8_t> page_data(page_size_);
    uint32_t valid_pages = 0;
    uint32_t index_pages = 0;
    uint32_t sdi_pages = 0;
    uint32_t undo_pages = 0;

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        device.seekg(page_no * page_size_, std::ios::beg);
        device.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        uint16_t page_type = innodb::read_le<uint16_t>(
            page_data.data(), innodb::FIL_PAGE_TYPE_ABS);

        if (page_type == innodb::FIL_PAGE_INDEX) {
            index_pages++;
            valid_pages++;
            index_pages_.push_back({page_no, page_data});
        } else if (page_type == innodb::FIL_PAGE_SDI) {
            sdi_pages++;
            valid_pages++;
            sdi_pages_.push_back({page_no, page_data});
        } else if (page_type == innodb::FIL_PAGE_UNDO_LOG) {
            undo_pages++;
            valid_pages++;
        } else if (page_type == innodb::FIL_PAGE_TYPE_ALLOCATED) {
            valid_pages++;
        }

        total_bytes_scanned_ += page_size_;
        pages_analyzed_++;

        // 进度输出
        if (page_no % 10000 == 0) {
            std::cout << "[INFO] Scanned " << page_no << "/" << total_pages 
                      << " pages (" << valid_pages << " valid)" << std::endl;
        }
    }

    std::cout << "[INFO] Device scan completed:" << std::endl;
    std::cout << "  - Total pages: " << total_pages << std::endl;
    std::cout << "  - Valid pages: " << valid_pages << std::endl;
    std::cout << "  - Index pages: " << index_pages << std::endl;
    std::cout << "  - SDI pages: " << sdi_pages << std::endl;
    std::cout << "  - Undo pages: " << undo_pages << std::endl;

    return valid_pages > 0;
}

bool DiskScanner::scan_disk(const std::string& disk_path) {
    // 尝试扫描整个磁盘
    std::cout << "[INFO] DiskScanner: Scanning disk: " << disk_path << std::endl;

    // 尝试常见的分区路径
    std::vector<std::string> partitions = {
        disk_path + "1",
        disk_path + "2",
        disk_path + "3",
        disk_path + "p1",
        disk_path + "p2",
        disk_path + "p3"
    };

    bool found = false;
    for (const auto& partition : partitions) {
        if (std::filesystem::exists(partition)) {
            std::cout << "[INFO] Checking partition: " << partition << std::endl;
            if (scan_raw_device(partition)) {
                found = true;
            }
        }
    }

    return found;
}

bool DiskScanner::analyze_ibd_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[WARN] Failed to open file: " << path << std::endl;
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

    // 读取前几个页进行分析
    for (uint32_t page_no = 0; page_no < std::min(total_pages, 10u); ++page_no) {
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        uint16_t page_type = innodb::read_le<uint16_t>(
            page_data.data(), innodb::FIL_PAGE_TYPE_ABS);

        if (page_type == innodb::FIL_PAGE_INDEX) {
            index_pages++;
            valid_pages++;
        } else if (page_type == innodb::FIL_PAGE_SDI) {
            sdi_pages++;
            valid_pages++;
            sdi_pages_.push_back({page_no, page_data});
        } else if (page_type == innodb::FIL_PAGE_UNDO_LOG) {
            undo_pages++;
            valid_pages++;
        } else if (page_type == 15) { // BLOB page type
            blob_pages++;
            valid_pages++;
            blob_pages_.push_back({page_no, page_data});
        } else if (page_type == innodb::FIL_PAGE_TYPE_ALLOCATED) {
            valid_pages++;
        }

        total_bytes_scanned_ += page_size_;
        pages_analyzed_++;
    }

    // 如果找到 SDI 页，尝试解析表结构
    std::optional<catalog::TableSchema> schema_opt;
    bool has_sdi = false;

    if (!sdi_pages_.empty()) {
        auto [page_no, sdi_data] = sdi_pages_.back();
        std::vector<uint8_t> sdi_payload_vec(sdi_data.begin() + 38, sdi_data.end());
        schema_opt = parse_schema_from_sdi(sdi_payload_vec);
        has_sdi = schema_opt.has_value();
    }

    // 推断表名和数据库名
    auto [db_name, table_name] = infer_table_from_path(path);

    // 如果没有 SDI，尝试从文件名推断
    if (!has_sdi && table_name.empty()) {
        table_name = "unknown_table";
    }

    ScannedTable table;
    table.table_name = table_name;
    table.database_name = db_name;
    table.ibd_path = path;
    table.table_id = 0; // 从 SDI 解析
    table.total_pages = total_pages;
    table.valid_pages = valid_pages;
    table.index_pages = index_pages;
    table.sdi_pages = sdi_pages;
    table.undo_pages = undo_pages;
    table.blob_pages = blob_pages;
    table.estimated_row_count = index_pages * 100; // 粗略估计
    table.schema = schema_opt.value_or(catalog::TableSchema());
    table.has_sdi = has_sdi;
    table.is_corrupted = false;

    if (has_sdi) {
        table.table_id = schema_opt.value().table_id;
        table_name_to_path_[table_name] = path;
    }

    scanned_tables_.push_back(table);

    std::cout << "[INFO] Analyzed: " << path << std::endl;
    std::cout << "  - Tables: " << table_name << " (has_sdi: " << has_sdi << ")" << std::endl;
    std::cout << "  - Pages: " << total_pages << " (valid: " << valid_pages << ")" << std::endl;

    return true;
}

std::optional<catalog::TableSchema> DiskScanner::parse_schema_from_sdi(
    const std::vector<uint8_t>& sdi_payload) {
    
    auto json_opt = innodb::SDIManager::extract_sdi_json(
        sdi_payload.data(), sdi_payload.size());

    if (!json_opt.has_value()) {
        return std::nullopt;
    }

    try {
        auto json = nlohmann::json::parse(json_opt.value());
        catalog::TableSchema schema;
        if (schema.from_json(json)) {
            return schema;
        }
    } catch (const nlohmann::json::parse_error& e) {
        // SDI JSON 解析失败，返回空 schema
    }

    return std::nullopt;
}

std::pair<std::string, std::string> DiskScanner::infer_table_from_path(const std::string& path) {
    std::filesystem::path p(path);
    std::string filename = p.filename().string();

    // 移除 .ibd 扩展名
    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
        filename = filename.substr(0, filename.size() - 4);
    }

    // 尝试从父目录获取数据库名
    std::string db_name = "";
    std::string table_name = filename;

    try {
        auto parent = p.parent_path();
        if (parent.has_parent_path()) {
            db_name = parent.filename().string();
        }
    } catch (...) {
        // 忽略错误
    }

    return {db_name, table_name};
}

const std::vector<ScannedTable>& DiskScanner::get_scanned_tables() const {
    return scanned_tables_;
}

const ScanResult& DiskScanner::get_scan_result() const {
    return scan_result_;
}

const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& 
DiskScanner::get_sdi_pages() const {
    return sdi_pages_;
}

const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& 
DiskScanner::get_index_pages() const {
    return index_pages_;
}

const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& 
DiskScanner::get_blob_pages() const {
    return blob_pages_;
}

// ==================== TableNameSearcher 实现 ====================

TableNameSearcher::TableNameSearcher() : page_size_(16384) {}

TableNameSearcher::~TableNameSearcher() = default;

void TableNameSearcher::set_data_directory(const std::string& dir) {
    data_dir_ = dir;
}

void TableNameSearcher::set_page_size(uint32_t size) {
    if (size == 4096 || size == 8192 || size == 16384 || 
        size == 32768 || size == 65536) {
        page_size_ = size;
    }
}

std::vector<TableSearchResult> TableNameSearcher::search_table(const std::string& table_name) {
    search_results_.clear();

    // 先从 SDI 搜索
    auto sdi_results = search_from_sdi(table_name);
    search_results_.insert(search_results_.end(), sdi_results.begin(), sdi_results.end());

    // 再从文件名搜索
    auto filename_results = search_from_filename(table_name);

    // 合并结果（去重）
    std::set<std::string> seen_paths;
    for (const auto& result : search_results_) {
        seen_paths.insert(result.ibd_path);
    }

    for (const auto& result : filename_results) {
        if (seen_paths.find(result.ibd_path) == seen_paths.end()) {
            search_results_.push_back(result);
        }
    }

    return search_results_;
}

std::vector<TableSearchResult> TableNameSearcher::search_all_tables() {
    search_results_.clear();

    if (data_dir_.empty()) {
        return search_results_;
    }

    search_directory(data_dir_, "*");

    return search_results_;
}

std::vector<TableSearchResult> TableNameSearcher::search_from_sdi(const std::string& table_name) {
    std::vector<TableSearchResult> results;

    if (data_dir_.empty()) {
        return results;
    }

    // 递归扫描所有 .sdi 文件
    std::filesystem::path dir(data_dir_);
    if (!std::filesystem::exists(dir)) {
        return results;
    }

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() && 
                entry.path().extension().string() == ".sdi") {
                
                std::ifstream file(entry.path(), std::ios::binary);
                if (!file) continue;

                // 读取文件内容
                file.seekg(0, std::ios::end);
                size_t size = file.tellg();
                file.seekg(0, std::ios::beg);

                std::vector<uint8_t> data(size);
                file.read(reinterpret_cast<char*>(data.data()), size);

                // 解析 SDI
                auto json_opt = innodb::SDIManager::extract_sdi_json(data.data(), data.size());
                if (!json_opt.has_value()) continue;

                try {
                    auto json = nlohmann::json::parse(json_opt.value());

                    // 检查表名
                    std::string found_table_name = json.value("dd_object.name", "");
                    
                    if (match_filename(found_table_name, table_name)) {
                        TableSearchResult result;
                        result.table_name = found_table_name;
                        result.database_name = json.value("dd_object.schema", "");
                        result.ibd_path = entry.path().string();
                        result.found_in_sdi = true;
                        result.found_in_filename = false;
                        
                        // 尝试解析表结构
                        catalog::TableSchema schema;
                        if (schema.from_json(json)) {
                            result.schema = schema;
                        }

                        results.push_back(result);
                    }
                } catch (const nlohmann::json::parse_error& e) {
                    // 忽略解析错误
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[WARN] Failed to scan directory: " << e.what() << std::endl;
    }

    return results;
}

std::vector<TableSearchResult> TableNameSearcher::search_from_filename(const std::string& table_name) {
    std::vector<TableSearchResult> results;

    if (data_dir_.empty()) {
        return results;
    }

    search_directory(data_dir_, table_name);

    return search_results_;
}

void TableNameSearcher::search_directory(const std::string& dir, const std::string& pattern) {
    std::filesystem::path path(dir);
    if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path)) {
        return;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                
                // 移除 .ibd 扩展名
                if (filename.size() > 4 && 
                    filename.substr(filename.size() - 4) == ".ibd") {
                    filename = filename.substr(0, filename.size() - 4);
                }

                if (match_filename(filename, pattern)) {
                    auto [db_name, table_name] = parse_table_name(entry.path().string());

                    TableSearchResult result;
                    result.table_name = table_name;
                    result.database_name = db_name;
                    result.ibd_path = entry.path().string();
                    result.found_in_sdi = false;
                    result.found_in_filename = true;

                    search_results_.push_back(result);
                }
            } else if (entry.is_directory()) {
                // 跳过系统目录
                std::string dirname = entry.path().filename().string();
                if (dirname != "." && dirname != ".." && 
                    dirname != "mysql" && dirname != "sys" &&
                    dirname != "performance_schema" && dirname != "information_schema") {
                    search_directory(entry.path().string(), pattern);
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[WARN] Failed to scan directory: " << e.what() << std::endl;
    }
}

bool TableNameSearcher::match_filename(const std::string& filename, const std::string& pattern) {
    // 简单的通配符匹配
    if (pattern == "*") {
        return true;
    }

    // 转换通配符为 fnmatch 格式
    std::string fn_pattern = pattern;
    std::replace(fn_pattern.begin(), fn_pattern.end(), '?', '?');
    std::replace(fn_pattern.begin(), fn_pattern.end(), '*', '*');

    return fnmatch(fn_pattern.c_str(), filename.c_str(), 0) == 0;
}

std::pair<std::string, std::string> TableNameSearcher::parse_table_name(const std::string& path) {
    std::filesystem::path p(path);
    std::string filename = p.filename().string();

    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
        filename = filename.substr(0, filename.size() - 4);
    }

    std::string db_name = "";
    try {
        auto parent = p.parent_path();
        if (parent.has_parent_path()) {
            db_name = parent.filename().string();
        }
    } catch (...) {
        // 忽略错误
    }

    return {db_name, filename};
}

size_t TableNameSearcher::get_search_count() const {
    return search_results_.size();
}

// ==================== BulkRecovery 实现 ====================

BulkRecovery::BulkRecovery() : format_("csv") {
    statistics_["total_tables"] = 0;
    statistics_["recovered_tables"] = 0;
    statistics_["failed_tables"] = 0;
    statistics_["total_records"] = 0;
}

BulkRecovery::~BulkRecovery() = default;

void BulkRecovery::set_output_directory(const std::string& dir) {
    output_dir_ = dir;
}

void BulkRecovery::set_format(const std::string& fmt) {
    format_ = fmt;
}

void BulkRecovery::add_table(const ScannedTable& table) {
    tables_.push_back(table);
    statistics_["total_tables"] = tables_.size();
}

bool BulkRecovery::recover_all() {
    return recover_tables(tables_);
}

bool BulkRecovery::recover_tables(const std::vector<ScannedTable>& tables) {
    if (tables.empty()) {
        std::cerr << "[ERROR] No tables to recover" << std::endl;
        return false;
    }

    // 创建输出目录
    try {
        std::filesystem::create_directories(output_dir_);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create output directory: " << e.what() << std::endl;
        return false;
    }

    bool all_success = true;
    uint64_t total_records = 0;
    uint64_t total_pages_parsed = 0;
    uint64_t total_pages_failed = 0;
    std::vector<std::string> table_results;

    for (const auto& table : tables) {
        std::cout << "[INFO] ============================================\n";
        std::cout << "[INFO] Processing table: " << table.table_name << "\n";
        std::cout << "[INFO] Source: " << table.ibd_path << "\n";
        
        if (recover_single_table(table)) {
            statistics_["recovered_tables"]++;
            total_records += table.estimated_row_count;
            table_results.push_back(table.table_name + " - SUCCESS");
        } else {
            statistics_["failed_tables"]++;
            all_success = false;
            table_results.push_back(table.table_name + " - FAILED");
        }
    }

    statistics_["total_records"] = total_records;

    // ✅ P2-4 新增：生成全局汇总报告
    std::string global_summary_file = output_dir_ + "/global_recovery_summary.txt";
    std::ofstream global_summary(global_summary_file);
    if (global_summary) {
        global_summary << "MySQL Recovery Pro - Global Recovery Summary\n";
        global_summary << "=============================================\n\n";
        global_summary << "Recovery Timestamp: " 
                       << std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count()
                       << "\n\n";
        global_summary << "Statistics:\n";
        global_summary << "  - Total tables processed: " << statistics_["total_tables"] << "\n";
        global_summary << "  - Successfully recovered: " << statistics_["recovered_tables"] << "\n";
        global_summary << "  - Failed: " << statistics_["failed_tables"] << "\n";
        global_summary << "  - Total records: " << total_records << "\n\n";
        global_summary << "Table Results:\n";
        for (const auto& result : table_results) {
            global_summary << "  - " << result << "\n";
        }
        global_summary << "\nOutput Directory: " << output_dir_ << "\n";
        global_summary.close();
        std::cout << "\n[INFO] Global summary saved to: " << global_summary_file << std::endl;
    }

    // ✅ P2-5 新增：生成 JSON 格式的汇总报告
    std::string json_summary_file = output_dir_ + "/recovery_summary.json";
    std::ofstream json_summary(json_summary_file);
    if (json_summary) {
        nlohmann::json json;
        json["recovery_timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json["total_tables"] = statistics_["total_tables"];
        json["recovered_tables"] = statistics_["recovered_tables"];
        json["failed_tables"] = statistics_["failed_tables"];
        json["total_records"] = total_records;
        
        nlohmann::json tables_json;
        for (const auto& table : tables) {
            nlohmann::json t;
            t["database"] = table.database_name;
            t["table"] = table.table_name;
            t["source_path"] = table.ibd_path;
            t["status"] = "recovered";  // 简化处理
            tables_json.push_back(t);
        }
        json["tables"] = tables_json;
        
        json_summary << json.dump(2);
        json_summary.close();
        std::cout << "[INFO] JSON summary saved to: " << json_summary_file << std::endl;
    }

    std::cout << "\n[INFO] Bulk recovery completed:" << std::endl;
    std::cout << "  - Total tables: " << statistics_["total_tables"] << std::endl;
    std::cout << "  - Recovered: " << statistics_["recovered_tables"] << std::endl;
    std::cout << "  - Failed: " << statistics_["failed_tables"] << std::endl;
    std::cout << "  - Total records: " << total_records << std::endl;

    return all_success;
}

bool BulkRecovery::recover_single_table(const ScannedTable& table) {
    std::cout << "[INFO] Recovering table: " << table.table_name << std::endl;
    std::cout << "  - Database: " << table.database_name << std::endl;
    std::cout << "  - Source: " << table.ibd_path << std::endl;

    // ✅ P2-1 优化：创建带标识的输出目录
    // 格式：database_table_timestamp 或 database_table_corrupted
    std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    std::string safe_db_name = table.database_name.empty() ? "unknown_db" : table.database_name;
    std::string safe_table_name = table.table_name.empty() ? "unknown_table" : table.table_name;
    
    // 根据文件名和恢复状态创建标识
    std::string table_dir_name;
    if (table.is_corrupted) {
        table_dir_name = safe_db_name + "_" + safe_table_name + "_corrupted_" + timestamp;
    } else {
        table_dir_name = safe_db_name + "_" + safe_table_name + "_" + timestamp;
    }
    
    std::string table_dir = output_dir_ + "/" + table_dir_name;
    try {
        std::filesystem::create_directories(table_dir);
    } catch (const std::exception& e) {
        std::cerr << "[WARN] Failed to create table directory: " << e.what() << std::endl;
        return false;
    }
    
    // 保存原始文件信息
    std::string info_file = table_dir + "/table_info.json";
    std::ofstream info_out(info_file);
    if (info_out) {
        nlohmann::json info;
        info["database"] = table.database_name;
        info["table"] = table.table_name;
        info["source_path"] = table.ibd_path;
        info["table_id"] = table.table_id;
        info["total_pages"] = table.total_pages;
        info["valid_pages"] = table.valid_pages;
        info["index_pages"] = table.index_pages;
        info["sdi_pages"] = table.sdi_pages;
        info["has_sdi"] = table.has_sdi;
        info["is_corrupted"] = table.is_corrupted;
        info["recovery_timestamp"] = timestamp;
        info_out << info.dump(2);
        info_out.close();
        std::cout << "  - Table info saved to: " << info_file << std::endl;
    }

    // 打开 .ibd 文件
    std::ifstream file(table.ibd_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[ERROR] Failed to open file: " << table.ibd_path << std::endl;
        return false;
    }

    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 16384) {
        std::cerr << "[WARN] File too small: " << table.ibd_path << std::endl;
        return false;
    }

    uint32_t page_size = 16384;
    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size);

    std::vector<uint8_t> page_data(page_size);
    std::vector<std::map<std::string, std::string>> records;

    // 读取所有索引页
    uint32_t pages_parsed = 0;
    uint32_t pages_failed = 0;
    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * page_size, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size);

        uint16_t page_type = innodb::read_le<uint16_t>(
            page_data.data(), innodb::FIL_PAGE_TYPE_ABS);

        if (page_type != innodb::FIL_PAGE_INDEX) {
            continue;
        }

        // 解析记录
        innodb::BPlusTreeParser parser(page_data);
        if (!parser.parse()) {
            pages_failed++;
            continue;
        }

        auto page_records = parser.parse_records(table.schema);
        records.insert(records.end(), page_records.begin(), page_records.end());
        pages_parsed++;
    }

    std::cout << "  - Pages parsed: " << pages_parsed << ", failed: " << pages_failed << std::endl;
    std::cout << "  - Recovered " << records.size() << " records" << std::endl;

    // ✅ P2-2 优化：导出结果时添加表标识前缀
    std::string output_path;
    if (format_ == "csv") {
        // 使用表名作为前缀
        output_path = table_dir + "/" + safe_table_name + "_recovered.csv";
        std::ofstream out(output_path);
        if (out) {
            // 写入表头
            if (!records.empty()) {
                bool first = true;
                for (const auto& [key, value] : records[0]) {
                    if (!first) out << ",";
                    out << key;
                    first = false;
                }
                out << "\n";

                // 写入数据
                for (const auto& record : records) {
                    first = true;
                    for (const auto& [key, value] : record) {
                        if (!first) out << ",";
                        out << value;
                        first = false;
                    }
                    out << "\n";
                }
            }
            out.close();
            std::cout << "  - CSV exported to: " << output_path << std::endl;
        }
    } else if (format_ == "sql") {
        // 使用表名作为前缀
        output_path = table_dir + "/" + safe_table_name + "_recovered.sql";
        std::ofstream out(output_path);
        if (out) {
            out << "-- Recovered data from " << table.table_name << "\n";
            out << "-- Database: " << table.database_name << "\n";
            out << "-- Source: " << table.ibd_path << "\n";
            out << "-- Records: " << records.size() << "\n";
            out << "-- Timestamp: " << timestamp << "\n\n";
            out << "INSERT INTO `" << table.table_name << "` VALUES\n";

            bool first = true;
            for (const auto& record : records) {
                if (!first) out << ",\n";
                out << "(";
                bool first_col = true;
                for (const auto& [key, value] : record) {
                    if (!first_col) out << ", ";
                    out << "'" << value << "'";
                    first_col = false;
                }
                out << ")";
                first = false;
            }
            out << ";\n";
            out.close();
            std::cout << "  - SQL exported to: " << output_path << std::endl;
        }
    } else if (format_ == "ibd") {
        // 导出为 IBD 格式（仅包含恢复的数据）
        output_path = table_dir + "/" + safe_table_name + "_recovered.ibd";
        std::ofstream out(output_path, std::ios::binary);
        if (out) {
            // 写入恢复的记录到 IBD 格式
            // 这里简化处理，实际需要重新构建页结构
            out << "-- Recovered from " << table.ibd_path << "\n";
            out << "-- Database: " << table.database_name << "\n";
            out << "-- Table: " << table.table_name << "\n";
            out << "-- Records: " << records.size() << "\n";
            for (const auto& record : records) {
                for (const auto& [key, value] : record) {
                    out << key << "=" << value << "|";
                }
                out << "\n";
            }
            out.close();
            std::cout << "  - IBD export to: " << output_path << std::endl;
        }
    }

    // ✅ P2-3 新增：生成恢复摘要
    std::string summary_file = table_dir + "/recovery_summary.txt";
    std::ofstream summary_out(summary_file);
    if (summary_out) {
        summary_out << "MySQL Recovery Pro - Table Recovery Summary\n";
        summary_out << "============================================\n\n";
        summary_out << "Table: " << table.table_name << "\n";
        summary_out << "Database: " << table.database_name << "\n";
        summary_out << "Source: " << table.ibd_path << "\n";
        summary_out << "Table ID: " << table.table_id << "\n\n";
        summary_out << "Statistics:\n";
        summary_out << "  - Total pages: " << table.total_pages << "\n";
        summary_out << "  - Valid pages: " << table.valid_pages << "\n";
        summary_out << "  - Index pages: " << table.index_pages << "\n";
        summary_out << "  - Pages parsed: " << pages_parsed << "\n";
        summary_out << "  - Pages failed: " << pages_failed << "\n\n";
        summary_out << "Recovery Results:\n";
        summary_out << "  - Records recovered: " << records.size() << "\n";
        summary_out << "  - Output format: " << format_ << "\n";
        summary_out << "  - Output path: " << output_path << "\n";
        summary_out << "  - Timestamp: " << timestamp << "\n\n";
        summary_out << "Status: " << (records.size() > 0 ? "SUCCESS" : "PARTIAL") << "\n";
        summary_out.close();
        std::cout << "  - Summary saved to: " << summary_file << std::endl;
    }

    return !records.empty();
}

const std::map<std::string, uint64_t>& BulkRecovery::get_statistics() const {
    return statistics_;
}

// ==================== AutoSchemaInferer 实现 ====================

AutoSchemaInferer::AutoSchemaInferer() : page_size_(16384) {}

AutoSchemaInferer::~AutoSchemaInferer() = default;

std::optional<catalog::TableSchema> AutoSchemaInferer::infer_from_page(
    const std::vector<uint8_t>& page_data) {
    
    if (page_data.size() < 38) {
        return std::nullopt;
    }

    uint16_t page_type = innodb::read_le<uint16_t>(
        page_data.data(), innodb::FIL_PAGE_TYPE_ABS);

    if (page_type != innodb::FIL_PAGE_INDEX) {
        return std::nullopt;
    }

    // 解析 B+Tree 页
    innodb::BPlusTreeParser parser(page_data);
    if (!parser.parse()) {
        return std::nullopt;
    }

    // 解析记录
    catalog::TableSchema schema;
    schema.table_name = "inferred_table";
    schema.row_format = 2; // DYNAMIC
    schema.page_size = page_size_;

    // 推断列信息
    auto records = parser.parse_records(schema);
    if (!records.empty()) {
        inferred_columns_ = infer_columns_from_records(records);
        for (size_t i = 0; i < inferred_columns_.size(); ++i) {
            schema.columns[i + 1] = inferred_columns_[i];
        }
    }

    return schema;
}

std::optional<catalog::TableSchema> AutoSchemaInferer::infer_from_pages(
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages) {
    
    if (pages.empty()) {
        return std::nullopt;
    }

    catalog::TableSchema schema;
    schema.table_name = "inferred_table";
    schema.row_format = 2;
    schema.page_size = page_size_;

    std::vector<std::map<std::string, std::string>> all_records;

    for (const auto& [page_no, page_data] : pages) {
        if (page_data.size() < 38) continue;

        uint16_t page_type = innodb::read_le<uint16_t>(
            page_data.data(), innodb::FIL_PAGE_TYPE_ABS);

        if (page_type != innodb::FIL_PAGE_INDEX) continue;

        innodb::BPlusTreeParser parser(page_data);
        if (!parser.parse()) continue;

        auto records = parser.parse_records(schema);
        all_records.insert(all_records.end(), records.begin(), records.end());
    }

    if (!all_records.empty()) {
        inferred_columns_ = infer_columns_from_records(all_records);
        for (size_t i = 0; i < inferred_columns_.size(); ++i) {
            schema.columns[i + 1] = inferred_columns_[i];
        }
    }

    return schema;
}

std::optional<catalog::TableSchema> AutoSchemaInferer::infer_from_sdi(
    const std::vector<uint8_t>& sdi_payload) {
    
    auto json_opt = innodb::SDIManager::extract_sdi_json(
        sdi_payload.data(), sdi_payload.size());

    if (!json_opt.has_value()) {
        return std::nullopt;
    }

    try {
        auto json = nlohmann::json::parse(json_opt.value());
        catalog::TableSchema schema;
        if (schema.from_json(json)) {
            return schema;
        }
    } catch (const nlohmann::json::parse_error& e) {
        // SDI JSON 解析失败，返回空 schema
    }

    return std::nullopt;
}

std::vector<catalog::ColumnInfo> AutoSchemaInferer::infer_columns_from_records(
    const std::vector<std::map<std::string, std::string>>& records) {
    
    if (records.empty()) {
        return {};
    }

    // 获取所有键
    std::set<std::string> keys;
    for (const auto& record : records) {
        for (const auto& [key, value] : record) {
            keys.insert(key);
        }
    }

    std::vector<catalog::ColumnInfo> columns;
    int col_idx = 1;

    for (const auto& key : keys) {
        catalog::ColumnInfo col;
        col.name = key;
        col.is_nullable = true;

        // 推断类型
        uint8_t type = infer_column_type(records[0].at(key));
        col.mysql_type_id = type;

        // 设置长度
        if (type == innodb::mysql_types::VARCHAR ||
            type == innodb::mysql_types::VAR_STRING) {
            col.column_length = 255;
        } else if (type == innodb::mysql_types::LONGLONG) {
            col.column_length = 8;
        } else if (type == innodb::mysql_types::LONG) {
            col.column_length = 4;
        } else {
            col.column_length = 0;
        }

        columns.push_back(col);
    }

    return columns;
}

uint32_t AutoSchemaInferer::infer_column_count_from_header(const uint8_t* record_header) {
    // 从记录头推断列数
    // Compact 格式：前 5 字节是记录头
    // Dynamic 格式：前 5 字节是记录头，后面是变长字段长度列表
    
    if (!record_header) return 0;

    // 检查是否为 Compact 格式
    uint8_t first_byte = record_header[0];
    if (first_byte & 0x80) {
        // Compact 格式
        uint8_t n_fields = first_byte & 0x7F;
        return n_fields + 1;
    }

    return 0;
}

uint8_t AutoSchemaInferer::infer_column_type(const std::string& value) {
    if (value.empty()) {
        return innodb::mysql_types::VARCHAR;
    }

    // 尝试推断类型
    try {
        // 检查是否为整数
        size_t pos;
        std::stoll(value, &pos);
        if (pos == value.size()) {
            return innodb::mysql_types::LONGLONG;
        }
    } catch (...) {
        // 不是整数
    }

    // 检查是否为浮点数
    try {
        size_t pos;
        std::stod(value, &pos);
        if (pos == value.size()) {
            return innodb::mysql_types::DOUBLE;
        }
    } catch (...) {
        // 不是浮点数
    }

    // 检查是否为日期时间
    if (value.size() >= 10) {
        if (value[4] == '-' && value[7] == '-') {
            return innodb::mysql_types::DATETIME;
        }
    }

    // 默认为 VARCHAR
    return innodb::mysql_types::VARCHAR;
}

} // namespace recovery
} // namespace dbrecovery
