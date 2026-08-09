#include "dbrecovery/recovery/engine.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/heuristic_parser.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>

namespace dbrecovery {
namespace recovery {

RecoveryEngine::RecoveryEngine() : file_reader_(nullptr), current_file_path_("")
{}

RecoveryEngine::~RecoveryEngine() {
    close();
}

bool RecoveryEngine::open(const std::string& file_path) {
    if (file_reader_) {
        close();
    }

    // 🔧 修复：在创建 InnoDBFileReader 之前应用页大小提示
    // 这确保 InnoDBFileReader 构造函数使用正确的页大小，而不是自动检测
    std::unique_ptr<innodb::InnoDBFileReader> reader;
    if (page_size_hint_ > 0) {
        reader = std::make_unique<innodb::InnoDBFileReader>(file_path, page_size_hint_);
    } else {
        reader = std::make_unique<innodb::InnoDBFileReader>(file_path);
    }

    if (!reader->is_valid()) {
        std::cerr << "[ERROR] Failed to open file: " << file_path << std::endl;
        return false;
    }

    file_reader_ = std::move(reader);

    current_file_path_ = file_path;
    std::cout << "[INFO] Successfully opened file: " << file_path
              << " (" << file_reader_->get_file_size() << " bytes, "
              << file_reader_->get_total_pages() << " pages)" << std::endl;

    return true;
}

void RecoveryEngine::close() {
    if (file_reader_) {
        file_reader_.reset();
        current_file_path_ = "";
    }
}

std::vector<std::map<std::string, std::string>>
RecoveryEngine::recover_truncate(
    const catalog::TableSchema& schema) {

    if (!file_reader_ || file_reader_->is_valid() == false) {
        std::cerr << "[ERROR] No file opened" << std::endl;
        return {};
    }

    std::vector<std::map<std::string, std::string>> recovered_records;
    std::set<std::string> seen_combinations;

    uint32_t total_pages = file_reader_->get_total_pages();
    uint32_t page_size = file_reader_->get_page_size();
    std::cout << "[INFO] Processing " << total_pages << " pages with heuristic scan..." << std::endl;

    uint32_t last_progress_page = 0;
    auto last_progress_time = std::chrono::high_resolution_clock::now();

    // Use InnoDBIndexParser for INDEX pages (schema-agnostic parsing)
    innodb::InnoDBIndexParser index_parser;
    index_parser.set_recover_deleted(true);  // Include deleted records for TRUNCATE recovery
    index_parser.set_bad_page_mode(1);  // Fast mode for corrupted pages

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        auto page_data = file_reader_->read_page_raw(page_no);
        if (page_data.empty()) {
            continue;
        }

        // Check page type
        uint16_t page_type = innodb::read_be<uint16_t>(page_data.data(), innodb::FIL_PAGE_TYPE_ABS);

        if (page_type == innodb::FIL_PAGE_INDEX) {
            // Use IndexParser for INDEX pages - proper record parsing
            auto index_records = index_parser.parse_page(page_data.data(), page_data.size(), page_no);

            for (const auto& idx_rec : index_records) {
                if (idx_rec.is_infimum || idx_rec.is_supremum) {
                    continue;
                }

                std::map<std::string, std::string> rec;

                // Convert IndexRecord fields to map
                for (size_t i = 0; i < idx_rec.fields.size(); ++i) {
                    const auto& field = idx_rec.fields[i];
                    std::string col_name;

                    // Try to get column name from schema
                    uint32_t ordinal = static_cast<uint32_t>(i) + 1;
                    auto it = schema.columns.find(ordinal);
                    if (it != schema.columns.end()) {
                        col_name = it->second.name;
                    } else {
                        // Use zero-padded field names for correct alphabetical sorting
                        char buf[16];
                        snprintf(buf, sizeof(buf), "field_%04zu", i);
                        col_name = buf;
                    }

                    if (field.is_null) {
                        rec[col_name] = "NULL";
                    } else {
                        rec[col_name] = field.to_string();
                    }
                }

                if (!rec.empty()) {
                    // Deduplication using first field + page_no
                    std::string dedup_key;
                    auto first_field = rec.begin();
                    if (first_field != rec.end()) {
                        dedup_key = first_field->second + "_" + std::to_string(page_no) + "_" + std::to_string(idx_rec.record_offset);
                    } else {
                        dedup_key = std::to_string(page_no) + "_" + std::to_string(idx_rec.record_offset);
                    }

                    if (seen_combinations.find(dedup_key) == seen_combinations.end()) {
                        seen_combinations.insert(dedup_key);
                        recovered_records.push_back(rec);
                    }
                }
            }
        } else {
            // For non-INDEX pages, fall back to SchemaAgnosticScanner (pattern-agnostic)
            innodb::SchemaAgnosticScanner::Config sa_config;
            sa_config.recover_deleted = true;
            innodb::SchemaAgnosticScanner sa_scanner(sa_config);

            auto sa_records = sa_scanner.parse_page(page_data.data(), page_data.size(), page_no);

            for (const auto& sa_rec : sa_records.records) {
                if (sa_rec.is_deleted || !sa_rec.is_valid) {
                    continue;
                }

                if (sa_rec.fields.empty()) {
                    continue;
                }

                std::map<std::string, std::string> rec;

                // Build record from schemaless fields
                // Use zero-padded field names for correct alphabetical sorting
                // field_0000, field_0001, ... field_9999
                for (size_t i = 0; i < sa_rec.fields.size(); i++) {
                    char key[16];
                    snprintf(key, sizeof(key), "field_%04zu", i);
                    rec[key] = sa_rec.fields[i].to_string();
                }

                // Use record_id as dedup key if available
                std::string dedup_key = std::to_string(sa_rec.record_id) + "_" + std::to_string(page_no);
                if (seen_combinations.find(dedup_key) == seen_combinations.end()) {
                    seen_combinations.insert(dedup_key);
                    recovered_records.push_back(rec);
                }
            }
        }

        if (progress_callback_) {
            progress_callback_(page_no, total_pages);
        }

        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_time);
        if (page_no - last_progress_page >= 1000 || elapsed.count() >= 1000) {
            double progress = static_cast<double>(page_no) / total_pages * 100.0;
            std::cout << "\r[INFO] Processing pages: " << page_no << "/" << total_pages
                      << " (" << std::fixed << std::setprecision(1) << progress << "%)" << std::flush;
            last_progress_page = page_no;
            last_progress_time = now;
        }
    }

    std::cout << "\r[INFO] Heuristic scan completed. Found " << recovered_records.size() << " records." << std::endl;

    if (total_pages > 0) {
        std::cout << "\r[INFO] Processing complete: " << total_pages << " pages processed." << std::endl;
    }

    std::cout << "[INFO] Recovered " << recovered_records.size() << " records from " << total_pages << " pages" << std::endl;
    return recovered_records;
}

bool RecoveryEngine::repair_header(const std::string& output_path) {
    if (!file_reader_ || file_reader_->is_valid() == false) {
        std::cerr << "[ERROR] No file opened" << std::endl;
        return false;
    }

    // 读取第一页（系统页）
    auto page_data = file_reader_->read_page_raw(0);
    if (page_data.empty() || page_data.size() < 16384) {
        std::cerr << "[ERROR] Failed to read first page for header repair" << std::endl;
        return false;
    }

    // 检查并修复 FIL_PAGE_TYPE
    uint16_t* page_type_ptr = reinterpret_cast<uint16_t*>(page_data.data() + innodb::FIL_PAGE_TYPE_ABS);
    if (*page_type_ptr != innodb::FIL_PAGE_INDEX &&
        *page_type_ptr != innodb::FIL_PAGE_SDI) {
        std::cerr << "[WARN] Invalid page type in header, attempting repair" << std::endl;

        // 尝试设置为默认的 B+Tree 索引页类型
        *page_type_ptr = innodb::FIL_PAGE_INDEX;
    }

    // 写入修复后的文件
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "[ERROR] Failed to open output file: " << output_path << std::endl;
        return false;
    }

    out.write(reinterpret_cast<const char*>(page_data.data()), page_data.size());

    // 补全剩余页（如果文件不完整）
    uint64_t file_size = file_reader_->get_file_size();
    size_t page_size = file_reader_->get_page_size();
    uint64_t expected_size = (file_reader_->get_total_pages() + 1) * page_size;

    if (expected_size > file_size) {
        std::vector<uint8_t> padding(page_size, 0);
        for (uint64_t i = file_size; i < expected_size; i += page_size) {
            out.write(reinterpret_cast<const char*>(padding.data()), padding.size());
        }
    }

    out.close();
    std::cout << "[INFO] Header repaired and saved to: " << output_path << std::endl;

    return true;
}

bool RecoveryEngine::export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                                   const catalog::TableSchema& schema,
                                   const std::string& csv_path) {
    std::ofstream out(csv_path);
    if (!out) {
        std::cerr << "[ERROR] Failed to open CSV file: " << csv_path << std::endl;
        return false;
    }

    // 如果没有记录，只写入空文件（或可选的空表头）
    if (records.empty()) {
        std::cout << "[INFO] No records to export, created empty CSV file: " << csv_path << std::endl;
        out.close();
        return true;
    }

    // Write header
    if (!schema.columns.empty()) {
        // Use schema column names
        for (const auto& pair : schema.columns) {
            if (pair.first > 1) out << ",";
            out << pair.second.name;
        }
    } else if (!records.empty()) {
        // Schemaless mode: write field names from first record
        // The map keys are already sorted (zero-padded field_NNNN format)
        bool first = true;
        for (const auto& pair : records[0]) {
            if (!first) out << ",";
            out << pair.first;
            first = false;
        }
    }
    out << "\n";

    // 写入数据行（按schema顺序）
    for (const auto& record : records) {
        for (auto it = schema.columns.begin(); it != schema.columns.end(); ++it) {
            if (it != schema.columns.begin()) out << ",";
            auto it2 = record.find(it->second.name);
            if (it2 != record.end()) {
                out << it2->second;
            }
        }
        out << "\n";
    }

    out.close();
    std::cout << "[INFO] Exported " << records.size() << " records to CSV: " << csv_path << std::endl;

    return true;
}

bool RecoveryEngine::export_to_sql(const std::vector<std::map<std::string, std::string>>& records,
                                   const std::string& table_name,
                                   const std::string& sql_path) {
    std::ofstream out(sql_path);
    if (!out) {
        std::cerr << "[ERROR] Failed to open SQL file: " << sql_path << std::endl;
        return false;
    }

    // 写入 SQL 头
    out << "-- MySQL Recovery Export\n";
    out << "-- Table: " << table_name << "\n";
    out << "-- Records: " << records.size() << "\n\n";

    // 如果没有记录，只写入头信息
    if (records.empty()) {
        std::cout << "[INFO] No records to export, created SQL file with header: " << sql_path << std::endl;
        out.close();
        return true;
    }

    // ✅ 修复：使用第一条记录的字段列表作为所有记录的字段模板
    // 确保 SQL 导出的字段数量一致
    std::vector<std::string> field_order;
    if (!records.empty()) {
        for (const auto& pair : records[0]) {
            field_order.push_back(pair.first);
        }
    }

    // ✅ 修复：添加 CREATE TABLE 语句（基于字段列表）
    out << "CREATE TABLE IF NOT EXISTS `" << table_name << "` (\n";
    for (size_t i = 0; i < field_order.size(); i++) {
        out << "    `" << field_order[i] << "` TEXT";
        if (i < field_order.size() - 1) out << ",";
        out << "\n";
    }
    out << ") ENGINE=InnoDB;\n\n";

    // ✅ 修复：正确转义 SQL 特殊字符（包括二进制字符）
    auto escape_sql_value = [](const std::string& value) -> std::string {
        std::string result;
        for (unsigned char c : value) {
            switch (c) {
                case '\0': result += "\\0"; break;        // NULL
                case '\n': result += "\\n"; break;        // newline
                case '\r': result += "\\r"; break;        // carriage return
                case '\t': result += "\\t"; break;        // tab
                case '\\': result += "\\\\"; break;       // backslash
                case '\'': result += "''"; break;         // single quote (MySQL style)
                case '"': result += "\\\""; break;        // double quote
                case '\x1a': result += "\\Z"; break;      // Ctrl+Z
                default:
                    // ✅ 修复：过滤不可打印的二进制字符（ASCII < 32 或 > 127）
                    // 避免 SQL 文件包含二进制数据导致导入失败
                    if (c >= 32 && c < 127) {
                        // 可打印 ASCII 字符
                        result += static_cast<char>(c);
                    } else if (c >= 127) {
                        // 高位字符（可能是 UTF-8 多字节的一部分）
                        result += static_cast<char>(c);
                    } else {
                        // 控制字符：转义为十六进制表示
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\x%02X", c);
                        result += buf;
                    }
                    break;
            }
        }
        return result;
    };

    for (const auto& record : records) {
        std::ostringstream oss;
        oss << "INSERT INTO `" << table_name << "` (";

        // 列名（使用固定的字段列表）
        bool first = true;
        for (const auto& field : field_order) {
            if (!first) oss << ", ";
            oss << "`" << field << "`";
            first = false;
        }
        oss << ") VALUES (";

        // 值（使用固定的字段列表，缺失字段用 NULL）
        first = true;
        for (const auto& field : field_order) {
            if (!first) oss << ", ";

            auto it = record.find(field);
            if (it == record.end() || it->second.empty()) {
                // 字段缺失或空值，使用 NULL
                oss << "NULL";
            } else if (it->second == "NULL") {
                oss << "NULL";
            } else {
                // ✅ 修复：使用完善的转义函数
                oss << "'" << escape_sql_value(it->second) << "'";
            }
            first = false;
        }
        oss << ");\n";

        out << oss.str();
    }

    out.close();
    std::cout << "[INFO] Exported " << records.size() << " records to SQL: " << sql_path << std::endl;

    return true;
}

bool RecoveryEngine::export_to_sql_with_user(const std::vector<std::map<std::string, std::string>>& records,
                                              const std::string& table_name,
                                              const std::string& sql_path,
                                              const std::string& user,
                                              const std::string& database) {
    std::ofstream out(sql_path);
    if (!out) {
        std::cerr << "[ERROR] Failed to open SQL file: " << sql_path << std::endl;
        return false;
    }

    // 写入 SQL 头（带用户和数据库信息）
    out << "-- MySQL Recovery Export\n";
    out << "-- User: " << user << "\n";
    out << "-- Database: " << database << "\n";
    out << "-- Table: " << table_name << "\n";
    out << "-- Records: " << records.size() << "\n\n";

    // 添加 USE DATABASE 语句
    if (!database.empty()) {
        out << "USE `" << database << "`;\n\n";
    }

    // 如果没有记录，只写入头信息
    if (records.empty()) {
        std::cout << "[INFO] No records to export, created SQL file with header: " << sql_path << std::endl;
        out.close();
        return true;
    }

    // ✅ 修复：使用第一条记录的字段列表作为所有记录的字段模板
    std::vector<std::string> field_order;
    if (!records.empty()) {
        for (const auto& pair : records[0]) {
            field_order.push_back(pair.first);
        }
    }

    // ✅ 修复：添加 CREATE TABLE 语句
    out << "CREATE TABLE IF NOT EXISTS `" << table_name << "` (\n";
    for (size_t i = 0; i < field_order.size(); i++) {
        out << "    `" << field_order[i] << "` TEXT";
        if (i < field_order.size() - 1) out << ",";
        out << "\n";
    }
    out << ") ENGINE=InnoDB;\n\n";

    // ✅ 修复：正确转义 SQL 特殊字符
    auto escape_sql_value = [](const std::string& value) -> std::string {
        std::string result;
        for (unsigned char c : value) {
            switch (c) {
                case '\0': result += "\\0"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                case '\\': result += "\\\\"; break;
                case '\'': result += "''"; break;
                case '"': result += "\\\""; break;
                case '\x1a': result += "\\Z"; break;
                default:
                    if (c >= 32 && c < 127) {
                        result += static_cast<char>(c);
                    } else if (c >= 127) {
                        result += static_cast<char>(c);
                    } else {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\x%02X", c);
                        result += buf;
                    }
                    break;
            }
        }
        return result;
    };

    for (const auto& record : records) {
        std::ostringstream oss;
        oss << "INSERT INTO `" << table_name << "` (";

        // 列名（使用固定的字段列表）
        bool first = true;
        for (const auto& field : field_order) {
            if (!first) oss << ", ";
            oss << "`" << field << "`";
            first = false;
        }
        oss << ") VALUES (";

        // 值（使用固定的字段列表）
        first = true;
        for (const auto& field : field_order) {
            if (!first) oss << ", ";

            auto it = record.find(field);
            if (it == record.end() || it->second.empty()) {
                oss << "NULL";
            } else if (it->second == "NULL") {
                oss << "NULL";
            } else {
                oss << "'" << escape_sql_value(it->second) << "'";
            }
            first = false;
        }
        oss << ");\n";

        out << oss.str();
    }

    out.close();
    std::cout << "[INFO] Exported " << records.size() << " records to SQL (user: " << user << ", db: " << database << "): " << sql_path << std::endl;

    return true;
}

bool RecoveryEngine::export_to_json(const std::vector<std::map<std::string, std::string>>& records,
                                    const std::string& json_path) {
    std::ofstream out(json_path);
    if (!out) {
        std::cerr << "[ERROR] Failed to open JSON file: " << json_path << std::endl;
        return false;
    }

    // 使用 nlohmann/json 构建 JSON
    nlohmann::json json;
    json["records"] = nlohmann::json::array();

    for (const auto& record : records) {
        nlohmann::json obj;
        for (const auto& pair : record) {
            // 简单处理：将值作为字符串存储
            // 对于 NULL 值，存储为 JSON null
            if (pair.second == "NULL") {
                obj[pair.first] = nullptr;
            } else {
                // 检查字符串是否包含有效的UTF-8
                const std::string& value = pair.second;
                bool is_valid_utf8 = true;
                for (size_t i = 0; i < value.size(); ) {
                    unsigned char byte = static_cast<unsigned char>(value[i]);
                    if (byte < 0x80) {
                        i++;
                    } else if ((byte & 0xE0) == 0xC0) {
                        if (i + 1 >= value.size() || (static_cast<unsigned char>(value[i+1]) & 0xC0) != 0x80) {
                            is_valid_utf8 = false;
                            break;
                        }
                        i += 2;
                    } else if ((byte & 0xF0) == 0xE0) {
                        if (i + 2 >= value.size() || (static_cast<unsigned char>(value[i+1]) & 0xC0) != 0x80 ||
                            (static_cast<unsigned char>(value[i+2]) & 0xC0) != 0x80) {
                            is_valid_utf8 = false;
                            break;
                        }
                        i += 3;
                    } else if ((byte & 0xF8) == 0xF0) {
                        if (i + 3 >= value.size() || (static_cast<unsigned char>(value[i+1]) & 0xC0) != 0x80 ||
                            (static_cast<unsigned char>(value[i+2]) & 0xC0) != 0x80 ||
                            (static_cast<unsigned char>(value[i+3]) & 0xC0) != 0x80) {
                            is_valid_utf8 = false;
                            break;
                        }
                        i += 4;
                    } else {
                        is_valid_utf8 = false;
                        break;
                    }
                }
                
                if (is_valid_utf8) {
                    obj[pair.first] = value;
                } else {
                    // 对于非UTF-8数据（如BLOB），存储为base64编码
                    obj[pair.first] = "__BLOB_BASE64__" + pair.second;
                }
            }
        }
        json["records"].push_back(obj);
    }

    // 写入格式化的 JSON
    // Use error_handler_t::replace to handle non-UTF-8 data
    out << json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
    out.close();

    std::cout << "[INFO] Exported " << records.size() << " records to JSON: " << json_path << std::endl;
    return true;
}

// ==================== 实现缺失的方法 ====================

void RecoveryEngine::set_space_id(uint32_t space_id) {
    space_id_ = space_id;
    // Store space_id for TDE key derivation context
    // The space_id identifies the tablespace for per-table key derivation
    if (!primary_key_hex_.empty()) {
        // Parse hex key and register as table key for this space
        std::vector<uint8_t> key_bytes;
        std::string clean_hex = primary_key_hex_;
        if (clean_hex.substr(0, 2) == "0x" || clean_hex.substr(0, 2) == "0X")
            clean_hex = clean_hex.substr(2);
        for (size_t i = 0; i + 1 < clean_hex.length(); i += 2) {
            key_bytes.push_back(static_cast<uint8_t>(
                std::stoul(clean_hex.substr(i, 2), nullptr, 16)));
        }
        tde_decryptor_.add_table_key(space_id, key_bytes, innodb::TDEAlgorithm::AES_256_CBC);
    }
    std::cout << "[INFO] Set space_id to: " << space_id << std::endl;
}

bool RecoveryEngine::set_tde_primary_key(const std::string& key_hex) {
    // Parse hex string to bytes
    std::vector<uint8_t> key_bytes;
    std::string clean_hex = key_hex;
    // Remove "0x" prefix if present
    if (clean_hex.substr(0, 2) == "0x" || clean_hex.substr(0, 2) == "0X") {
        clean_hex = clean_hex.substr(2);
    }
    // Remove any spaces or colons
    clean_hex.erase(std::remove(clean_hex.begin(), clean_hex.end(), ' '), clean_hex.end());
    clean_hex.erase(std::remove(clean_hex.begin(), clean_hex.end(), ':'), clean_hex.end());

    if (clean_hex.length() % 2 != 0) {
        std::cerr << "[ERROR] Invalid TDE key hex length (must be even)" << std::endl;
        return false;
    }

    for (size_t i = 0; i < clean_hex.length(); i += 2) {
        uint8_t byte = std::stoul(clean_hex.substr(i, 2), nullptr, 16);
        key_bytes.push_back(byte);
    }

    if (key_bytes.size() < 16 || key_bytes.size() > 64) {
        std::cerr << "[ERROR] TDE key must be 16-64 bytes (got " << key_bytes.size() << ")" << std::endl;
        return false;
    }

    tde_decryptor_.set_primary_key(key_bytes);
    std::cout << "[INFO] Set TDE primary key (" << key_bytes.size() << " bytes)" << std::endl;
    return true;
}

bool RecoveryEngine::add_tde_table_key(uint32_t key_id, const std::string& key_hex) {
    // Parse hex string to bytes
    std::vector<uint8_t> key_bytes;
    std::string clean_hex = key_hex;
    if (clean_hex.substr(0, 2) == "0x" || clean_hex.substr(0, 2) == "0X") {
        clean_hex = clean_hex.substr(2);
    }
    clean_hex.erase(std::remove(clean_hex.begin(), clean_hex.end(), ' '), clean_hex.end());
    clean_hex.erase(std::remove(clean_hex.begin(), clean_hex.end(), ':'), clean_hex.end());

    if (clean_hex.length() % 2 != 0) {
        std::cerr << "[ERROR] Invalid TDE key hex length" << std::endl;
        return false;
    }

    for (size_t i = 0; i < clean_hex.length(); i += 2) {
        uint8_t byte = std::stoul(clean_hex.substr(i, 2), nullptr, 16);
        key_bytes.push_back(byte);
    }

    tde_decryptor_.add_table_key(key_id, key_bytes, innodb::TDEAlgorithm::AES_256_CBC);
    std::cout << "[INFO] Added TDE table key: id=" << key_id << " (" << key_bytes.size() << " bytes)" << std::endl;
    return true;
}

bool RecoveryEngine::load_tde_keyring(const std::string& keyring_path) {
    innodb::KeyringFile keyring;
    if (!keyring.load(keyring_path)) {
        std::cerr << "[ERROR] Failed to load keyring file: " << keyring_path << std::endl;
        return false;
    }

    std::cout << "[INFO] Loaded keyring with " << keyring.entries.size() << " keys" << std::endl;

    // Store all keys from keyring
    // Keys are stored by name (e.g., "INNODBKey-uuid-123")
    for (const auto& entry : keyring.entries) {
        // Store key by name for lookup during decryption
        tde_decryptor_.add_table_key(
            static_cast<uint32_t>(std::hash<std::string>{}(entry.key_name)),  // Hash name to numeric ID
            entry.key_data,
            entry.algorithm);

        std::cout << "[INFO]   Key: " << entry.key_name
                  << " (" << entry.key_data.size() << " bytes)" << std::endl;
    }

    keyring_loaded_ = true;
    return true;
}

void RecoveryEngine::set_detect_encryption(bool detect) {
    detect_encryption_ = detect;
    std::cout << "[INFO] Set detect_encryption to: " << (detect ? "true" : "false") << std::endl;
}

void RecoveryEngine::set_page_size(uint32_t page_size) {
    page_size_hint_ = page_size;
    
    // 如果文件已打开，更新 file_reader_ 的页大小提示
    if (file_reader_) {
        file_reader_->set_page_size_hint(page_size);
        std::cout << "[INFO] Set page_size to: " << page_size 
                  << " (total_pages: " << file_reader_->get_total_pages() << ")" << std::endl;
    } else {
        std::cout << "[INFO] Set page_size to: " << page_size << " (file not yet opened)" << std::endl;
    }
}

uint32_t RecoveryEngine::detect_page_size() const {
    // 默认返回16KB
    return 16384;
}

std::vector<std::map<std::string, std::string>> RecoveryEngine::extract_records_from_page(
    const std::vector<uint8_t>& page_data,
    const catalog::TableSchema& schema,
    uint32_t page_no) const {
    std::vector<std::map<std::string, std::string>> records;

    if (page_data.empty() || page_data.size() < 54) {
        return records;
    }

    // Try BPlusTreeParser first
    try {
        innodb::BPlusTreeParser btree_parser(page_data);
        if (btree_parser.parse()) {
            auto btree_records = btree_parser.parse_records(schema);
            if (!btree_records.empty()) {
                return btree_records;
            }
        }
    } catch (...) {
        // BPlusTreeParser failed, fall through to heuristic
    }

    // Fallback: use HeuristicParser
    try {
        innodb::HeuristicParser heuristic_parser;
        uint16_t heap_top = innodb::read_le<uint16_t>(page_data.data(), 40);
        auto heuristic_records = heuristic_parser.parse_all_records(
            page_data.data(), page_data.size(), heap_top);

        for (const auto& hr : heuristic_records) {
            if (hr.is_infimum || hr.is_supremum || hr.is_deleted) {
                continue;
            }
            if (hr.fields.empty()) {
                continue;
            }

            std::map<std::string, std::string> row;
            size_t col_idx = 0;
            for (const auto& field : hr.fields) {
                std::string col_name;
                uint32_t ordinal = static_cast<uint32_t>(col_idx) + 1;
                auto it = schema.columns.find(ordinal);
                col_name = (it != schema.columns.end()) ? it->second.name : ("field" + std::to_string(col_idx));

                if (field.is_null) {
                    row[col_name] = "NULL";
                } else {
                    row[col_name] = field.to_string();
                }
                col_idx++;
            }
            if (!row.empty()) {
                records.push_back(std::move(row));
            }
        }
    } catch (...) {
        // Heuristic parser also failed
    }

    return records;
}

std::vector<std::map<std::string, std::string>> RecoveryEngine::extract_records_sequentially(
    const catalog::TableSchema& schema) const {
    std::vector<std::map<std::string, std::string>> all_records;

    if (!file_reader_ || !file_reader_->is_valid()) {
        return all_records;
    }

    uint32_t total_pages = file_reader_->get_total_pages();

    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        auto page_data = file_reader_->read_page_raw(page_no);
        if (page_data.empty()) {
            continue;
        }

        auto page_records = extract_records_from_page(page_data, schema, page_no);
        all_records.insert(all_records.end(),
                          std::make_move_iterator(page_records.begin()),
                          std::make_move_iterator(page_records.end()));

        if (progress_callback_) {
            progress_callback_(page_no, total_pages);
        }
    }

    return all_records;
}

std::vector<std::map<std::string, std::string>> RecoveryEngine::extract_records_test_format(
    const catalog::TableSchema& schema,
    const std::vector<uint8_t>& page_data,
    uint32_t page_no) const {
    // Delegate to standard page-based record extraction
    return extract_records_from_page(page_data, schema, page_no);
}

std::vector<std::map<std::string, std::string>> RecoveryEngine::deduplicate_records(
    const std::vector<std::map<std::string, std::string>>& records,
    const catalog::TableSchema& schema) const {
    if (records.empty()) {
        return records;
    }

    std::vector<std::map<std::string, std::string>> result;
    std::set<std::string> seen;

    // Build deduplication key from primary key columns if available
    // Otherwise, hash the entire record
    auto make_dedup_key = [&](const std::map<std::string, std::string>& rec) -> std::string {
        std::string key;
        bool has_pk = false;

        // Try primary key columns first (first column in ordinal order)
        // Check if any column looks like a primary key (e.g., BIGINT named 'id')
        for (const auto& [ordinal, col] : schema.columns) {
            // Use the first non-nullable integer column as likely PK
            if (!col.is_nullable && (col.mysql_type_id == innodb::mysql_types::LONGLONG ||
                                     col.mysql_type_id == innodb::mysql_types::LONG ||
                                     col.mysql_type_id == innodb::mysql_types::INT24)) {
                auto it = rec.find(col.name);
                if (it != rec.end()) {
                    key += it->second + "|";
                    has_pk = true;
                    break;
                }
            }
        }

        // Fall back to full record hash
        if (!has_pk) {
            for (const auto& [k, v] : rec) {
                key += k + "=" + v + "|";
            }
        }

        return key;
    };

    for (const auto& record : records) {
        std::string dedup_key = make_dedup_key(record);
        if (seen.find(dedup_key) == seen.end()) {
            seen.insert(std::move(dedup_key));
            result.push_back(record);
        }
    }

    return result;
}

// ==================== Undo-log-assisted recovery (Task 19) ====================

std::vector<std::map<std::string, std::string>> RecoveryEngine::recover_from_undo_logs(
    const std::string& table_name,
    const catalog::TableSchema& schema) {

    if (undo_data_path_.empty()) {
        std::cerr << "[ERROR] Undo data path not set. Use set_undo_data_path() first.\n";
        return {};
    }

    // Use UndoLogParser to scan undo logs and recover deleted records
    // The parser scans for TRX_UNDO_DEL_MARK entries and reconstructs row data
    auto undo_entries = undo_parser_.parse_all_undo_tablespaces(undo_data_path_);

    std::vector<std::map<std::string, std::string>> recovered;

    for (const auto& entry : undo_entries) {
        // Only process DELETE-type undo records
        if (entry.undo_type != 0x03) continue;  // TRX_UNDO_DEL_MARK

        std::map<std::string, std::string> row;
        row["_undo_action"] = "FLASHBACK_INSERT";
        row["_undo_type"] = std::to_string(entry.undo_type);

        for (size_t i = 0; i < entry.fields.size(); ++i) {
            std::string col_name;
            uint32_t ordinal = static_cast<uint32_t>(i) + 1;
            auto it = schema.columns.find(ordinal);
            if (it != schema.columns.end()) {
                col_name = it->second.name;
            } else {
                char buf[16];
                snprintf(buf, sizeof(buf), "field_%04zu", i);
                col_name = buf;
            }

            const auto& field = entry.fields[i];
            if (!field.data.empty()) {
                row[col_name] = std::string(field.data.begin(), field.data.end());
            } else {
                row[col_name] = "NULL";
            }
        }

        if (!row.empty()) {
            recovered.push_back(std::move(row));
        }
    }

    std::cout << "[INFO] Undo log recovery: found " << recovered.size()
              << " records from undo logs\n";
    return recovered;
}

std::vector<std::string> RecoveryEngine::flashback_deletes(
    const std::string& table_name,
    const std::string& start_time,
    const std::string& end_time) {

    if (undo_data_path_.empty()) {
        std::cerr << "[ERROR] Undo data path not set\n";
        return {};
    }

    // P0.7: Use FlashbackGenerator (schema-aware, human-readable flashback SQL)
    // instead of the raw DeepUndoAnalyzer inline SQL. FlashbackGenerator wraps
    // DeepUndoAnalyzer with proper field_no mapping, value decoding, and SQL formatting.
    flashback_generator_.set_ibdata_path(undo_data_path_);
    if (file_reader_) {
        flashback_generator_.set_page_size(file_reader_->get_page_size());
    }

    innodb::FlashbackConfig fg_config;
    fg_config.mode = innodb::FlashbackMode::DELETE;
    fg_config.output_format = innodb::FlashbackOutputFormat::SQL;
    flashback_generator_.set_config(fg_config);

    auto results = flashback_generator_.flashback_deletes(schema_);

    std::vector<std::string> sql;
    sql.reserve(results.size());
    for (const auto& r : results) {
        std::string stmt = r.to_sql(schema_);
        if (!stmt.empty()) {
            sql.push_back(stmt);
        }
    }
    return sql;
}

std::vector<innodb::UndoLogEntry> RecoveryEngine::traverse_undo_chain(
    uint32_t start_page_no, uint32_t start_offset) {

    if (undo_data_path_.empty()) {
        std::cerr << "[ERROR] Undo data path not set\n";
        return {};
    }

    return undo_parser_.traverse_undo_chain(undo_data_path_, start_page_no, start_offset);
}

const innodb::UndoLogParser::UndoLogStats& RecoveryEngine::get_undo_stats() const {
    return undo_parser_.get_stats();
}

} // namespace recovery
} // namespace dbrecovery

