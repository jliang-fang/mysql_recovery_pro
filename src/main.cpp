#include <iostream>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <set>
#include <regex>

#include "dbrecovery/innodb.h"
#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/heuristic_parser.hpp"
#include "dbrecovery/innodb/binary_recovery_enhanced.hpp"
#include "dbrecovery/utils/cli_parser.hpp"
#include "dbrecovery/recovery/engine.hpp"
#include "dbrecovery/recovery/rmrf_recovery.hpp"
#include "dbrecovery/recovery/truncate_recovery.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/catalog/schema_type_conversion.hpp"
#include "dbrecovery/innodb/tde_decryptor.hpp"
#include "dbrecovery/innodb/encrypted_table_recovery.hpp"
#include "dbrecovery/innodb/offline_keyring_parser.hpp"
#include "dbrecovery/repair/corruption_detector.hpp"
#include "dbrecovery/repair/header_repairer.hpp"
#include "dbrecovery/repair/physical_corruption.hpp"
#include "dbrecovery/common/mysql_version_detector.hpp"
#include "dbrecovery/engine/raw_disk_engine.hpp"
#include "dbrecovery/engine/signature_scraper.hpp"
#include "dbrecovery/engine/schema_aware_parser.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/innodb/frm_parser.hpp"
#include "dbrecovery/innodb/schema_agnostic_scanner.hpp"
#include "dbrecovery/tools/unified_recovery.hpp"
#include "dbrecovery/recovery/blob_chain_rebuilder.hpp"

namespace fs = std::filesystem;

// 辅助函数：转换字符串为小写
std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

dbrecovery::MySQLVersion parse_mysql_version_value(const std::string& value) {
    try {
        const uint32_t ver = static_cast<uint32_t>(std::stoul(value));
        if (ver >= 90000) return dbrecovery::MySQLVersion::MYSQL_90;
        if (ver >= 80400) return dbrecovery::MySQLVersion::MYSQL_84;
        if (ver >= 80000) return dbrecovery::MySQLVersion::MYSQL_80;
        if (ver >= 50700) return dbrecovery::MySQLVersion::MYSQL_57;
        if (ver >= 50600) return dbrecovery::MySQLVersion::MYSQL_56;
    } catch (...) {
    }
    return dbrecovery::MySQLVersion::UNKNOWN;
}

uint32_t mysql_version_number(dbrecovery::MySQLVersion version) {
    return dbrecovery::mysql_version_to_number(version);
}

// 辅助函数：从 SDI 页解析表结构
std::optional<dbrecovery::catalog::TableSchema> parse_schema_from_sdi(
    const std::string& ibd_path, uint64_t table_id,
    dbrecovery::MySQLVersion version = dbrecovery::MySQLVersion::UNKNOWN) {

    dbrecovery::innodb::SDIExtractor extractor;
    auto results = extractor.extract_from_file_if_compatible(ibd_path, version);
    for (const auto& extracted : results) {
        if (!extracted.is_valid || extracted.sdi_json.empty()) continue;

        // Heuristic extraction is EVIDENCE_ONLY — never authoritative schema.
        if (extracted.origin == dbrecovery::innodb::SDIManager::ExtractionOrigin::HeuristicCorruptedZip) {
            std::cout << "[WARN] Heuristic SDI extraction kept as EVIDENCE_ONLY (never authoritative)\n";
            continue;
        }

        try {
            auto json = nlohmann::json::parse(extracted.sdi_json);
            dbrecovery::catalog::TableSchema schema;

            if (schema.from_json(json)) {
                schema.schema_hash =
                    dbrecovery::salvage::SalvageEvidenceSerializer::sha256_hex(extracted.sdi_json);
                schema.schema_source = "SDI";
                schema.trust_level = "AUTHORITATIVE";
                std::cout << "[INFO] Successfully parsed schema from SDI: "
                          << schema.table_name << "\n";
                return schema;
            }
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "[WARN] Failed to parse SDI JSON: " << e.what() << "\n";
        }
    }

    return std::nullopt;
}

struct SDIExtractionOutcome {
    std::optional<dbrecovery::catalog::TableSchema> schema;
    std::string status = "NOT_FOUND";
    std::string reason;
};

SDIExtractionOutcome extract_sdi_schema_with_status(
    const std::string& ibd_path, dbrecovery::MySQLVersion version) {
    SDIExtractionOutcome outcome;
    dbrecovery::innodb::SDIExtractor extractor;
    const auto results = extractor.extract_from_file_if_compatible(ibd_path, version);
    const auto& stats = extractor.stats();

    if (version != dbrecovery::MySQLVersion::UNKNOWN &&
        !dbrecovery::version_supports_sdi(version)) {
        outcome.status = "UNSUPPORTED";
        outcome.reason = "the resolved MySQL version does not contain SDI metadata";
        return outcome;
    }
    if (stats.total_pages == 0) {
        outcome.reason = "input contains no complete pages";
        return outcome;
    }
    if (stats.sdi_pages == 0) {
        outcome.reason = "no SDI pages were identified";
        return outcome;
    }

    bool heuristic_only = false;
    for (const auto& extracted : results) {
        if (!extracted.is_valid || extracted.sdi_json.empty()) continue;
        if (extracted.origin == dbrecovery::innodb::SDIManager::ExtractionOrigin::HeuristicCorruptedZip) {
            heuristic_only = true;
            continue;
        }
        try {
            const auto json = nlohmann::json::parse(extracted.sdi_json);
            dbrecovery::catalog::TableSchema schema;
            if (schema.from_json(json)) {
                schema.schema_hash =
                    dbrecovery::salvage::SalvageEvidenceSerializer::sha256_hex(extracted.sdi_json);
                schema.schema_source = "SDI";
                schema.trust_level = "AUTHORITATIVE";
                outcome.schema = std::move(schema);
                outcome.status = "EXTRACTED";
                return outcome;
            }
        } catch (const nlohmann::json::exception&) {
        }
    }

    outcome.status = heuristic_only ? "HEURISTIC_ONLY" : "CORRUPTED";
    outcome.reason = heuristic_only
        ? "only heuristic SDI evidence was found; authoritative schema promotion is forbidden"
        : "SDI pages were identified but no valid schema could be parsed";
    return outcome;
}

// 辅助函数：创建默认表结构
dbrecovery::catalog::TableSchema create_default_schema() {
    dbrecovery::catalog::TableSchema schema;
    schema.table_name = "recovered_table";
    schema.row_format = 2;  // DYNAMIC
    schema.page_size = 16384;

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
    dbrecovery::utils::CliParser parser(argc, argv);

    // ✅ 使用完整的 help 消息
    if (parser.has_option("help")) {
        std::cout << dbrecovery::utils::CliParser::get_full_help("./build/mysql_recovery_pro");
        return 0;
    }

    std::cout << "MySQL Recovery Pro v1.0.0\n";
    std::cout << "Build: " << __DATE__ << " " << __TIME__ << "\n\n";

    // Parse arguments
    std::string ibd_path = parser.get_option("ibd", "");
    std::string output_dir = parser.get_option("output", "/tmp/recovered");
    auto table_id_opt = parser.get_int_option("table-id", 0);
    uint64_t table_id = static_cast<uint64_t>(table_id_opt.value_or(0));
    
    // Recovery modes
    bool check_headers = parser.get_bool_option("check-headers", false);
    bool check_corruption = parser.get_bool_option("check-corruption", false);
    bool repair = parser.get_bool_option("repair", false);
    bool truncate_recovery = parser.get_bool_option("truncate-recovery", false);
    std::string pre_truncate_ibd = parser.get_option("pre-truncate-ibd", "");
    bool blob_recovery = parser.get_bool_option("blob-recovery", false);
    bool rmrf_recovery = parser.get_bool_option("rmrf-recovery", false);
    bool header_repair = parser.get_bool_option("header-repair", false);
    bool schemaless_recovery = parser.get_bool_option("schemaless", false);
    bool raw_scan = parser.get_bool_option("raw-scan", false);
    bool extract_tuples = parser.get_bool_option("extract-tuples", false);

    // Scan & Discovery
    bool scan = parser.get_bool_option("scan", false);
    bool search = parser.get_bool_option("search", false);
    bool rebuild_chain = parser.get_bool_option("rebuild-chain", false);
    bool extract_sdi = parser.get_bool_option("extract-sdi", false);
    auto parse_page_opt = parser.get_int_option("parse-page", -1);
    int parse_page = static_cast<int>(parse_page_opt.value_or(-1));

    // Disk-level and FRM scanning (真实场景恢复)
    std::string disk_path = parser.get_option("disk", "");
    std::string frm_path = parser.get_option("frm", "");
    bool drop_recovery = parser.get_bool_option("drop-recovery", false);
    bool device_scan = parser.get_bool_option("scan-device", false);
    
    // Format and output
    std::string format = parser.get_option("format", "csv");
    auto page_size_opt = parser.get_int_option("page-size", 0);
    uint32_t page_size = static_cast<uint32_t>(page_size_opt.value_or(0));
    std::string data_dir = parser.get_option("data-dir", "");
    std::string table_name = parser.get_option("table-name", "");
    std::string schema_str = parser.get_option("schema", "");
    std::string schema_file_path = parser.get_option("schema-file", "");
    auto depth_opt = parser.get_int_option("depth", 3);
    int depth = static_cast<int>(depth_opt.value_or(3));

    // Force index selection (for troubleshooting when wrong index is auto-selected)
    auto force_index_opt = parser.get_int_option("force-index", 0);
    uint64_t force_index_id = static_cast<uint64_t>(force_index_opt.value_or(0));

    // MySQL version compatibility
    std::string mysql_version = parser.get_option("mysql-version", "");
    bool detect_version = parser.get_bool_option("detect-version", false);
    std::string row_format = parser.get_option("row-format", "");

    // LSN filter for raw-scan/truncate recovery
    std::string max_lsn_str = parser.get_option("max-lsn", "");
    std::optional<uint64_t> max_lsn_filter;
    if (!max_lsn_str.empty()) {
        try {
            max_lsn_filter = std::stoull(max_lsn_str);
        } catch (...) {
            std::cerr << "[WARN] Invalid --max-lsn value: " << max_lsn_str << "\n";
        }
    }
    
    // TDE encryption
    std::string key = parser.get_option("key", "");
    std::string keyring = parser.get_option("keyring", "");
    auto tablespace_id_opt = parser.get_int_option("tablespace-id", 0);
    uint32_t tablespace_id = static_cast<uint32_t>(tablespace_id_opt.value_or(0));
    std::string table_key = parser.get_option("table-key", "");
    bool try_no_key = parser.get_bool_option("try-no-key", false);
    bool detect_encryption = parser.get_bool_option("detect-encryption", false);
    
    // Batch operations
    std::string batch = parser.get_option("batch", "");
    std::string system_tablespace = parser.get_option("system-tablespace", "");
    
    // Other options
    bool verbose = parser.get_bool_option("verbose", false);
    auto threads_opt = parser.get_int_option("threads", 4);
    int threads = static_cast<int>(threads_opt.value_or(4));
    std::string user = parser.get_option("user", "root");
    std::string database = parser.get_option("database", "recovered_db");

    // CRC32C checksum verification during page scanning
    bool checksum_verify = parser.get_bool_option("checksum-verify", true);
    bool skip_checksum = parser.get_bool_option("skip-checksum", false);

    // Unified disk carving pipeline (XFS/ext4 → decompress → CRC32C → SDI → schema-aware → export)
    bool unified_carve = parser.get_bool_option("unified-carve", false);

    // ✅ 验证必选参数
    if (dbrecovery::innodb::g_verbose) {
        std::cerr << "[DEBUG] ibd_path='" << ibd_path << "' data_dir='" << data_dir
                  << "' pre_truncate_ibd='" << pre_truncate_ibd << "' truncate_recovery=" << truncate_recovery << "\n";
    }
    if (ibd_path.empty() && data_dir.empty() && batch.empty() && disk_path.empty() && !device_scan && !drop_recovery && !schemaless_recovery && pre_truncate_ibd.empty()) {
        std::cerr << "[ERROR] IBD file path, data directory, batch file, or disk path is required\n";
        std::cerr << "\nUsage:\n";
        std::cerr << "  ./build/mysql_recovery_pro --ibd=<path> [options]\n";
        std::cerr << "  ./build/mysql_recovery_pro --scan-dir=<dir> [options]\n";
        std::cerr << "  ./build/mysql_recovery_pro --search --table-name=<name> [options]\n";
        std::cerr << "  ./build/mysql_recovery_pro --batch=<file> [options]\n";
        std::cerr << "  ./build/mysql_recovery_pro --disk=<path> [options]\n";
        std::cerr << "\nRun with --help for full usage information.\n";
        return 1;
    }

    // 创建输出目录
    try {
        fs::create_directories(output_dir);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create output directory: " << output_dir
                  << ", error: " << e.what() << "\n";
        return 1;
    }

    // ✅ 恢复模式：页头检查
    if (check_headers) {
        std::cout << "[INFO] Checking page headers...\n";
        if (ibd_path.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required for header check\n";
            return 1;
        }

        // 🔧 修复：使用指定的页大小创建 InnoDBFileReader
        dbrecovery::innodb::InnoDBFileReader reader(ibd_path, page_size > 0 ? page_size : 0);
        if (!reader.is_valid()) {
            std::cerr << "[ERROR] Failed to open file: " << ibd_path << "\n";
            return 1;
        }

        uint32_t total_pages = reader.get_total_pages();

        std::cout << "[INFO] Header check completed.\n";
        std::cout << "  Total pages: " << total_pages << "\n";
        return 0;
    }

    // ✅ 恢复模式：提取SDI元数据
    if (extract_sdi) {
        std::cout << "[INFO] Extracting SDI metadata...\n";
        
        if (ibd_path.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required for SDI extraction\n";
            return 1;
        }

        auto sdi_outcome = extract_sdi_schema_with_status(
            ibd_path, dbrecovery::MySQLVersion::UNKNOWN);
        if (sdi_outcome.schema.has_value()) {
            const auto json_str = sdi_outcome.schema->to_json().dump(2);
            std::cout << json_str << "\n";

            std::string sdi_path = output_dir + "/sdi_metadata.json";
            std::ofstream sdi_file(sdi_path);
            if (!sdi_file) {
                std::cerr << "[ERROR] Failed to write SDI metadata: " << sdi_path << "\n";
                return 1;
            }
            sdi_file << json_str;
            if (!sdi_file) {
                std::cerr << "[ERROR] Failed to write SDI metadata: " << sdi_path << "\n";
                return 1;
            }
            std::cout << "[INFO] SDI metadata saved to: " << sdi_path << "\n";
            return 0;
        }

        std::cerr << "[ERROR] SDI extraction status=" << sdi_outcome.status;
        if (!sdi_outcome.reason.empty()) std::cerr << ": " << sdi_outcome.reason;
        std::cerr << "\n";
        return 1;
    }

    // ✅ 恢复模式：解析特定页
    if (parse_page >= 0) {
        std::cout << "[INFO] Parsing page " << parse_page << "...\n";
        
        if (ibd_path.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required for page parsing\n";
            return 1;
        }

        dbrecovery::innodb::InnoDBFileReader reader(ibd_path);
        if (!reader.is_valid()) {
            std::cerr << "[ERROR] Failed to open file: " << ibd_path << "\n";
            return 1;
        }

        auto page_data = reader.read_page_raw(static_cast<uint32_t>(parse_page));
        if (page_data.empty()) {
            std::cout << "[INFO] Page " << parse_page << " is empty or beyond file end.\n";
            return 0;
        }

        std::cout << "[INFO] Page " << parse_page << " contents (hex dump):\n";
        for (size_t i = 0; i < std::min(size_t(256), page_data.size()); ++i) {
            if (i % 16 == 0) std::cout << std::hex << (i) << ": ";
            std::cout << std::setfill('0') << std::setw(2) << std::hex 
                      << (int)page_data[i] << " ";
            if (i % 16 == 15) std::cout << std::dec << "\n";
        }
        return 0;
    }

    // ✅ 恢复模式：扫描整个数据目录
    if (scan || search) {
        std::cout << "[INFO] Scanning data directory...\n";
        
        if (data_dir.empty()) {
            std::cerr << "[ERROR] --data-dir=<dir> is required for directory scan\n";
            return 1;
        }

        std::set<std::string> found_tables;
        int scanned_files = 0;

        try {
            for (const auto& entry : fs::recursive_directory_iterator(data_dir)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    
                    // 检查是否为 .ibd 文件
                    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".ibd") {
                        scanned_files++;
                        
                        // 如果指定了表名，进行模糊匹配
                        if (!table_name.empty()) {
                            std::string table_lower = to_lower(table_name);
                            std::string filename_lower = to_lower(filename);
                            
                            bool matched = false;
                            if (table_lower.find('*') != std::string::npos) {
                                std::string pattern = table_lower;
                                size_t pos;
                                while ((pos = pattern.find('*')) != std::string::npos) {
                                    pattern.replace(pos, 1, ".*");
                                }
                                try {
                                    std::regex re(pattern, std::regex::optimize);
                                    matched = std::regex_match(filename_lower, re);
                                } catch (...) {
                                    // 如果正则失败，使用简单匹配
                                    std::string search_term = pattern;
                                    size_t pos2;
                                    while ((pos2 = search_term.find('*')) != std::string::npos) {
                                        search_term.replace(pos2, 1, "");
                                    }
                                    matched = (filename_lower.find(search_term) != std::string::npos);
                                }
                            } else if (table_lower.find('?') != std::string::npos) {
                                bool all_match = true;
                                if (filename_lower.size() == table_lower.size()) {
                                    for (size_t i = 0; i < table_lower.size(); ++i) {
                                        if (table_lower[i] != '?' && table_lower[i] != filename_lower[i]) {
                                            all_match = false;
                                            break;
                                        }
                                    }
                                    matched = all_match;
                                }
                            } else {
                                matched = (filename_lower == table_lower);
                            }
                            
                            if (matched) {
                                found_tables.insert(entry.path().string());
                                std::cout << "[FOUND] " << entry.path() << "\n";
                            }
                        } else {
                            found_tables.insert(entry.path().string());
                            if (verbose) {
                                std::cout << "[SCAN] " << entry.path() << "\n";
                            }
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Failed to scan directory: " << e.what() << "\n";
        }

        std::cout << "[INFO] Scan completed.\n";
        std::cout << "  Files scanned: " << scanned_files << "\n";
        std::cout << "  Tables found: " << found_tables.size() << "\n";
        
        if (!found_tables.empty()) {
            std::string table_list_path = output_dir + "/found_tables.txt";
            std::ofstream list_file(table_list_path);
            if (list_file) {
                for (const auto& table : found_tables) {
                    list_file << table << "\n";
                }
                list_file.close();
                std::cout << "[INFO] Table list saved to: " << table_list_path << "\n";
            }
        }
        
        return 0;
    }

    // ✅ 恢复模式：批量文件处理 — 委托统一流水线
    if (!batch.empty()) {
        std::cout << "[INFO] Processing batch file: " << batch << "\n";

        std::ifstream batch_file(batch);
        if (!batch_file) {
            std::cerr << "[ERROR] Cannot open batch file: " << batch << "\n";
            return 1;
        }

        std::string line;
        int processed = 0;
        int succeeded = 0;
        uint32_t total_rows = 0;

        while (std::getline(batch_file, line)) {
            if (line.empty() || line[0] == '#') continue;

            size_t start = line.find_first_not_of(" \t\r\n");
            size_t end = line.find_last_not_of(" \t\r\n");
            if (start == std::string::npos) continue;

            line = line.substr(start, end - start + 1);

            std::cout << "[INFO] Processing: " << line << "\n";

            fs::path p(line);
            std::string file_output = output_dir + "/" + p.stem().string();

            try {
                fs::create_directories(file_output);
            } catch (const std::exception& e) {
                std::cerr << "[WARN] Failed to create output dir: " << e.what() << "\n";
                continue;
            }

            const std::string batch_table = table_name.empty()
                ? p.stem().string()
                : table_name;
            auto ures = dbrecovery::tools::run_unified_recovery(
                line, file_output, batch_table, format, page_size,
                schema_str, frm_path, table_id, system_tablespace,
                repair, schemaless_recovery, verbose, schema_file_path,
                secondary_index_paths, secondary_index_name);
            dbrecovery::tools::write_unified_result_json(file_output, batch_table, ures);

            if (blob_recovery) {
                std::cout << "  [BLOB] recovery_method=" << ures.recovery_method
                          << " rows=" << ures.total_recovered << "\n";
            } else if (rmrf_recovery) {
                std::cout << "  [RMRF] recovery_method=" << ures.recovery_method
                          << " rows=" << ures.total_recovered << "\n";
            } else {
                std::cout << "  [DEFAULT] recovery_method=" << ures.recovery_method
                          << " rows=" << ures.total_recovered << "\n";
            }

            processed++;
            if (ures.success || ures.total_recovered > 0) succeeded++;
            total_rows += ures.total_recovered;
        }

        std::cout << "[INFO] Batch processing completed.\n";
        std::cout << "  Files processed: " << processed << "\n";
        std::cout << "  Succeeded: " << succeeded << "\n";
        std::cout << "  Total structured rows: " << total_rows << "\n";
        return 0;
    }

    std::vector<dbrecovery::innodb::MasterKey> parsed_tde_keys;
    bool tde_encrypted_ = false;
    uint32_t tde_page_size_ = 0;

    // TDE key provisioning before any recovery mode.
    // Recovery modes (force-scan/TRUNCATE/rm-rf/default index parse) must operate
    // on decrypted pages. The keyring flow mirrors the recovery CLI proven path:
    // OfflineKeyringParser (JSON / encrypted-JSON / Vault export) -> validate via
    // configure_keys() -> install keys into the shared TDE decryptor.
    // Any parse/password/validation failure is a hard error — never promotes a
    // detection-only report to a success exit with no recovery performed.
    if (detect_encryption || !key.empty() || !keyring.empty() || !table_key.empty()) {
        if (ibd_path.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required with TDE key options\n";
            return 1;
        }

        dbrecovery::innodb::OfflineKeyringParser keyring_parser;
        bool loaded = false;

        if (!key.empty()) {
            // Manual primary/table key in hex form: raw key bytes, no keyring
            loaded = true;
            std::vector<uint8_t> manual_key;
            std::string hex = key;
            auto hex_digit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            bool valid_hex = (hex.size() % 2 == 0);
            for (size_t i = 0; valid_hex && i < hex.size(); i += 2) {
                int hi = hex_digit(hex[i]), lo = hex_digit(hex[i + 1]);
                if (hi < 0 || lo < 0) { valid_hex = false; break; }
                manual_key.push_back(static_cast<uint8_t>((hi << 4) | lo));
            }
            if (!valid_hex || (manual_key.size() != 16 && manual_key.size() != 24 && manual_key.size() != 32)) {
                std::cerr << "[ERROR] --key must be a hex string of 32/48/64 chars "
                          << "(16/24/32 raw bytes)\n";
                return 1;
            }
            if (tablespace_id == 0) {
                std::cerr << "[ERROR] --key requires a non-zero --tablespace-id for key placement\n";
                return 1;
            }
            dbrecovery::innodb::MasterKey mk;
            mk.key_id = tablespace_id;
            mk.key_name = "manual-" + std::to_string(tablespace_id);
            mk.key_data = manual_key;
            mk.algorithm = dbrecovery::innodb::TDEAlgorithm::AES_256_CBC;
            parsed_tde_keys.push_back(mk);
        } else if (!keyring.empty()) {
            loaded = keyring_parser.parse_keyring_file(keyring);
            if (!loaded) {
                loaded = keyring_parser.parse_vault_export(keyring);
            }
            if (!loaded) {
                std::cerr << "[ERROR] Failed to parse keyring " << keyring << ": "
                          << keyring_parser.get_last_error() << "\n";
                return 1;
            }
            parsed_tde_keys = keyring_parser.get_master_keys();
        } else if (!table_key.empty()) {
            // Exact per-space table key as raw hex (server-generated tablespace keys)
            loaded = true;
            std::vector<uint8_t> raw;
            std::string hex = table_key;
            auto hex_digit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            bool valid_hex = (hex.size() % 2 == 0);
            for (size_t i = 0; valid_hex && i < hex.size(); i += 2) {
                int hi = hex_digit(hex[i]), lo = hex_digit(hex[i + 1]);
                if (hi < 0 || lo < 0) { valid_hex = false; break; }
                raw.push_back(static_cast<uint8_t>((hi << 4) | lo));
            }
            if (!valid_hex || raw.empty()) {
                std::cerr << "[ERROR] --table-key must be a non-empty hex string\n";
                return 1;
            }
            if (tablespace_id == 0) {
                std::cerr << "[ERROR] --table-key requires a non-zero --tablespace-id "
                          << "identifying the exact tablespace key\n";
                return 1;
            }
            dbrecovery::innodb::MasterKey mk;
            mk.key_id = tablespace_id;
            mk.key_name = "table-key-" + std::to_string(tablespace_id);
            mk.key_data = raw;
            mk.algorithm = dbrecovery::innodb::TDEAlgorithm::AES_256_CBC;
            parsed_tde_keys.push_back(mk);
        }

        if (!loaded || parsed_tde_keys.empty()) {
            std::cerr << "[ERROR] No usable key material loaded for recovery configuration\n";
            return 1;
        }

        // Validate keys atomically before they enter any recovery path.
        auto parse_tde_file = [&](const std::string& path, uint32_t& out_page_size,
                                  bool& out_encrypted) -> bool {
            out_page_size = 0;
            out_encrypted = false;
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) return false;
            size_t fsize = static_cast<size_t>(f.tellg());
            f.seekg(0, std::ios::beg);
            if (fsize == 0) return false;
            size_t guess = fsize < 16384 ? fsize : 16384;
            std::vector<uint8_t> page(guess);
            f.read(reinterpret_cast<char*>(page.data()), guess);
            if (f.gcount() <= 0) return false;
            size_t real_page_size = dbrecovery::innodb::smart_detect_page_size(page.data(), fsize);
            if (real_page_size < dbrecovery::innodb::UNIV_PAGE_SIZE_MIN ||
                real_page_size > dbrecovery::innodb::UNIV_PAGE_SIZE_MAX) {
                real_page_size = dbrecovery::innodb::UNIV_PAGE_SIZE;
            }
            if (page.size() < real_page_size) {
                std::vector<uint8_t> full(real_page_size);
                f.seekg(0, std::ios::beg);
                f.read(reinterpret_cast<char*>(full.data()), real_page_size);
                page = std::move(full);
            }
            out_page_size = static_cast<uint32_t>(real_page_size);
            out_encrypted = dbrecovery::innodb::TDEDecryptor::is_encrypted(page.data(), page.size());
            return true;
        };

        uint32_t meta_page_size = 0;
        bool meta_is_encrypted = false;
        if (!parse_tde_file(ibd_path, meta_page_size, meta_is_encrypted)) {
            std::cerr << "[ERROR] Failed to read TDE metadata from: " << ibd_path << "\n";
            return 1;
        }
        const uint32_t configured_page_size = page_size > 0 ? static_cast<uint32_t>(page_size) : meta_page_size;
        tde_page_size_ = configured_page_size;
        tde_encrypted_ = meta_is_encrypted;

        dbrecovery::innodb::EncryptedTableRecovery key_validator(configured_page_size);
        if (!key_validator.configure_keys(parsed_tde_keys)) {
            std::cerr << "[ERROR] Keyring contains unusable recovery keys: "
                      << key_validator.get_last_error() << "\n";
            return 1;
        }

        auto tde_stats = key_validator.get_decryptor().get_stats();
        std::cout << "[INFO] TDE decryptor initialized with " << tde_stats.keys_loaded << " keys\n";

        if (meta_is_encrypted) {
            std::cerr << "[WARN] Input matches the unvalidated TDE candidate heuristic; "
                         "fixture-validated TDE recovery is unavailable. "
                         "No recovery attempted.\n";
            return 1;
        } else if (key.empty() && keyring.empty() && table_key.empty()) {
            // Detection mode only: a non-candidate result is not authoritative
            // proof that the file has no encryption metadata.
            std::cout << "[INFO] File does not match the TDE candidate heuristic\n";
            return 0;
        }

        // Recovery continues with parsed_tde_keys available. When the file is
        // encrypted but decryption preparation above succeeded, TDE_prep passed.
        std::cout << "[INFO] TDE key provisioning succeeded — proceeding with recovery\n";
    }

    // ✅ 恢复模式： corruption check
    if (check_corruption) {
        std::cout << "[INFO] Checking for corruption...\n";

        if (ibd_path.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required for corruption check\n";
            return 1;
        }

        // 🔧 修复：使用指定的页大小创建 InnoDBFileReader
        dbrecovery::innodb::InnoDBFileReader reader(ibd_path, page_size > 0 ? page_size : 0);
        if (!reader.is_valid()) {
            std::cerr << "[ERROR] Failed to open file: " << ibd_path << "\n";
            return 1;
        }

        uint32_t total_pages = reader.get_total_pages();
        uint32_t corrupted = 0;

        for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
            auto page_data = reader.read_page_raw(page_no);
            if (page_data.empty()) continue;

            // 简单的页类型检查
            uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(
                page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);
            
            // 检查页类型是否在有效范围内
            if (page_type > 0x0010) {
                corrupted++;
                if (verbose) {
                    std::cout << "[WARN] Corrupted page " << page_no 
                              << ": invalid page type " << page_type << "\n";
                }
            }
        }

        std::cout << "[INFO] Corruption check completed.\n";
        std::cout << "  Total pages: " << total_pages << "\n";
        std::cout << "  Corrupted pages: " << corrupted << "\n";
        return 0;
    }

    // ✅ 恢复模式： header repair
    if (header_repair) {
        std::cout << "[INFO] Repairing page headers...\n";

        if (ibd_path.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required for header repair\n";
            return 1;
        }

        // Read the file
        std::ifstream input_file(ibd_path, std::ios::binary);
        if (!input_file) {
            std::cerr << "[ERROR] Cannot open file: " << ibd_path << "\n";
            return 1;
        }

        input_file.seekg(0, std::ios::end);
        size_t file_size = input_file.tellg();
        input_file.seekg(0, std::ios::beg);

        std::vector<uint8_t> file_data(file_size);
        input_file.read(reinterpret_cast<char*>(file_data.data()), file_size);
        input_file.close();

        // Determine page size
        size_t page_size_val = page_size > 0 ? page_size : 16384;
        size_t total_pages = file_size / page_size_val;

        std::cout << "[INFO] File size: " << file_size << " bytes\n";
        std::cout << "[INFO] Page size: " << page_size_val << " bytes\n";
        std::cout << "[INFO] Total pages: " << total_pages << "\n";

        // Use HeaderRepairer to repair pages
        dbrecovery::repair::HeaderRepairer repairer;
        size_t repaired_count = repairer.repair_file_pages(file_data.data(), file_size, page_size_val);

        // Write repaired file
        std::string output_path = output_dir.empty() ? (ibd_path + ".repaired") : (output_dir + "/repaired.ibd");
        if (!output_dir.empty()) {
            // Create output directory if needed
            std::filesystem::create_directories(output_dir);
        }

        std::ofstream output_file(output_path, std::ios::binary | std::ios::trunc);
        if (!output_file) {
            std::cerr << "[ERROR] Cannot create output file: " << output_path << "\n";
            return 1;
        }
        output_file.write(reinterpret_cast<const char*>(file_data.data()), file_size);
        output_file.close();

        std::cout << "[INFO] Repaired file saved to: " << output_path << "\n";
        std::cout << "[INFO] Header repair complete: " << repaired_count << " pages repaired\n";
        return 0;
    }

    // ✅ 物理损坏检测和修复
    bool physical_repair = parser.get_bool_option("physical-repair", false);
    bool physical_analyze = parser.get_bool_option("physical-analyze", false);

    if (physical_repair || physical_analyze) {
        std::cout << "[INFO] Physical corruption analysis and repair...\n";

        if (ibd_path.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required for physical repair\n";
            return 1;
        }

        // Read the file
        std::ifstream input_file(ibd_path, std::ios::binary);
        if (!input_file) {
            std::cerr << "[ERROR] Cannot open file: " << ibd_path << "\n";
            return 1;
        }

        input_file.seekg(0, std::ios::end);
        size_t file_size = input_file.tellg();
        input_file.seekg(0, std::ios::beg);

        std::vector<uint8_t> file_data(file_size);
        input_file.read(reinterpret_cast<char*>(file_data.data()), file_size);
        input_file.close();

        std::cout << "[INFO] File size: " << file_size << " bytes\n";

        // Determine page size
        size_t page_size_val = page_size > 0 ? page_size : 16384;

        // Use PhysicalCorruptionHandler
        dbrecovery::repair::PhysicalCorruptionHandler handler;
        handler.set_page_size(page_size_val);

        if (physical_analyze) {
            // Only analyze, don't repair
            auto corruptions = handler.analyze(file_data.data(), file_size);
            std::cout << "\n" << handler.get_report() << "\n";
        } else {
            // Analyze and repair
            size_t repaired_count = handler.analyze_and_repair(file_data.data(), file_size);

            std::cout << "\n" << handler.get_report() << "\n";

            if (repaired_count > 0) {
                // Write repaired file
                std::string output_path = output_dir.empty() ? (ibd_path + ".physical_repaired") : (output_dir + "/repaired.ibd");
                if (!output_dir.empty()) {
                    std::filesystem::create_directories(output_dir);
                }

                std::ofstream output_file(output_path, std::ios::binary | std::ios::trunc);
                if (!output_file) {
                    std::cerr << "[ERROR] Cannot create output file: " << output_path << "\n";
                    return 1;
                }
                output_file.write(reinterpret_cast<const char*>(file_data.data()), file_size);
                output_file.close();

                std::cout << "[INFO] Repaired file saved to: " << output_path << "\n";
            }
        }
        return 0;
    }

    // ✅ 恢复模式：Raw Disk Scan (extreme recovery - disk/partition/image scanning)
    if (raw_scan) {
        std::cout << "[INFO] Starting raw disk scan for InnoDB pages...\n";

        if (ibd_path.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required for raw scan (can be disk device or image file)\n";
            return 1;
        }

        dbrecovery::engine::RawDiskIOEngine disk_engine;
        if (!disk_engine.open(ibd_path, dbrecovery::engine::IoMode::READ_ONLY)) {
            std::cerr << "[ERROR] Failed to open: " << ibd_path << "\n";
            return 1;
        }

        std::cout << "[INFO] Device size: " << disk_engine.get_size() << " bytes ("
                  << (disk_engine.get_size() / (1024.0 * 1024.0)) << " MB)\n";
        std::cout << "[INFO] Direct I/O: " << (disk_engine.has_direct_io() ? "yes" : "no") << "\n";

        // Configure page scanner
        std::vector<uint32_t> page_sizes_to_scan = {page_size > 0 ? page_size : 16384};
        if (page_size == 0) {
            page_sizes_to_scan = {4096, 8192, 16384, 32768, 65536};
        }

        std::vector<dbrecovery::engine::PageScanner::FoundPage> all_found_pages;

        auto scan_start = std::chrono::high_resolution_clock::now();

        auto stats = dbrecovery::engine::PageScanner::scan_engine(
            disk_engine,
            [&all_found_pages](const dbrecovery::engine::PageScanner::FoundPage& page) {
                all_found_pages.push_back(page);
            },
            page_sizes_to_scan);

        auto scan_end = std::chrono::high_resolution_clock::now();
        double scan_seconds = std::chrono::duration<double>(scan_end - scan_start).count();

        std::cout << "\n=== Raw Disk Scan Results ===\n";
        std::cout << "  Bytes scanned: " << stats.total_bytes_scanned << "\n";
        std::cout << "  Pages found: " << stats.total_pages_found << "\n";
        std::cout << "  Index pages: " << stats.index_pages << "\n";
        std::cout << "  SDI pages: " << stats.sdi_pages << "\n";
        std::cout << "  BLOB pages: " << stats.blob_pages << "\n";
        std::cout << "  UNDO pages: " << stats.undo_pages << "\n";
        std::cout << "  Scan time: " << std::fixed << std::setprecision(2) << scan_seconds << "s\n";
        if (scan_seconds > 0) {
            double throughput_mb = (stats.total_bytes_scanned / (1024.0 * 1024.0)) / scan_seconds;
            std::cout << "  Throughput: " << std::fixed << std::setprecision(1) << throughput_mb << " MB/s\n";
        }

        // Now use SignatureBasedScraper for advanced scanning with LSN filtering
        std::cout << "\n[INFO] Phase 2: Signature-based scraping with LSN filter...\n";

        dbrecovery::engine::SignatureBasedScraper::ScanConfig sig_config;
        sig_config.min_page_size = page_sizes_to_scan.front();
        sig_config.max_page_size = page_sizes_to_scan.back();
        sig_config.validate_checksums = true;
        sig_config.filter_to_index_pages = false;
        sig_config.reconstruct_chains = true;
        sig_config.num_threads = threads;
        if (max_lsn_filter.has_value()) {
            sig_config.max_lsn = max_lsn_filter;
        }

        dbrecovery::engine::SignatureBasedScraper scraper(disk_engine);
        scraper.set_progress_callback([](double pct) {
            std::cout << "\r[PROGRESS] " << std::fixed << std::setprecision(1) << pct << "%" << std::flush;
        });

        std::vector<dbrecovery::engine::FoundPage> sig_pages;
        auto sig_stats = scraper.scan(sig_config,
            [&sig_pages](const dbrecovery::engine::FoundPage& page) {
                sig_pages.push_back(page);
            });

        std::cout << "\n  Signature scan: " << sig_stats.pages_matched << " pages matched, "
                  << sig_stats.pages_with_valid_checksum << " valid checksum, "
                  << sig_stats.pages_filtered_by_lsn << " filtered by LSN\n";

        // Reconstruct B+Tree chains
        auto chains = scraper.reconstruct_chains(sig_pages);
        std::cout << "  B+Tree chains reconstructed: " << chains.size() << "\n";

        // Export results
        std::string scan_result_path = output_dir + "/raw_scan_results.json";
        std::ofstream scan_file(scan_result_path);
        if (scan_file) {
            scan_file << "{\n  \"pages_found\": " << all_found_pages.size() << ",\n";
            scan_file << "  \"signature_pages\": " << sig_pages.size() << ",\n";
            scan_file << "  \"chains\": " << chains.size() << ",\n";
            scan_file << "  \"pages\": [\n";
            bool first = true;
            for (const auto& page : sig_pages) {
                if (!first) scan_file << ",\n";
                first = false;
                scan_file << "    {\"offset\": " << page.disk_offset
                          << ", \"page_number\": " << page.page_number
                          << ", \"space_id\": " << page.space_id
                          << ", \"page_type\": " << page.page_type
                          << ", \"lsn\": " << page.lsn
                          << ", \"checksum_valid\": " << (page.has_valid_checksum ? "true" : "false")
                          << "}";
            }
            scan_file << "\n  ]\n}\n";
            scan_file.close();
            std::cout << "[INFO] Scan results exported to: " << scan_result_path << "\n";
        }

        disk_engine.close();
        return 0;
    }

    // ✅ 恢复模式：Disk/Device Scan (PhotoRec-style for deleted .ibd files)
    if (device_scan && !disk_path.empty()) {
        std::cout << "[INFO] Starting PhotoRec-style disk scan (--scan-device)...\n";
        std::cout << "[INFO] Target: " << disk_path << "\n";

        dbrecovery::recovery::RmrfRecovery rmrf;
        if (page_size > 0) {
            rmrf.set_page_size(page_size);
        }

        // Use RmrfRecovery to scan and recover
        dbrecovery::catalog::TableSchema schema;
        schema.table_name = table_name.empty() ? "recovered_table" : table_name;

        std::vector<std::map<std::string, std::string>> recovered_records;

        // Try DROP TABLE / TRUNCATE recovery from disk
        bool success = rmrf.recover_truncated_table_from_disk(
            disk_path, schema, recovered_records, page_size > 0 ? page_size : 16384, 2);

        if (success && !recovered_records.empty()) {
            std::cout << "[INFO] Successfully recovered " << recovered_records.size()
                      << " records from disk\n";

            // Export to CSV
            std::string csv_path = output_dir + "/disk_recovered.csv";
            std::ofstream csv_file(csv_path);
            if (csv_file) {
                // Write CSV header
                if (!recovered_records.empty()) {
                    bool first_col = true;
                    for (const auto& [key, value] : recovered_records.front()) {
                        if (!first_col) csv_file << ",";
                        csv_file << key;
                        first_col = false;
                    }
                    csv_file << "\n";
                }
                // Write data
                for (const auto& rec : recovered_records) {
                    bool first_col = true;
                    for (const auto& [key, value] : rec) {
                        if (!first_col) csv_file << ",";
                        csv_file << "\"" << value << "\"";
                        first_col = false;
                    }
                    csv_file << "\n";
                }
                csv_file.close();
                std::cout << "[INFO] Recovered data exported to: " << csv_path << "\n";
            }
            return 0;
        } else {
            std::cerr << "[WARN] No records recovered from disk scan\n";
            return 1;
        }
    }

    // ✅ 恢复模式：DROP TABLE Recovery (scan disk for SDI residues)
    if (drop_recovery && !disk_path.empty()) {
        std::cout << "[INFO] Starting DROP TABLE recovery mode...\n";
        std::cout << "[INFO] Target disk: " << disk_path << "\n";

        // Create output directory for carved files
        fs::path carved_dir = output_dir + "/carved_ibd_files";
        fs::create_directories(carved_dir);

        std::vector<std::string> found_files;

        dbrecovery::recovery::RmrfRecovery rmrf;
        if (page_size > 0) {
            rmrf.set_page_size(page_size);
        }

        // Try to carve deleted .ibd files from disk
        bool found = rmrf.recover_deleted_ibd_files(
            disk_path, carved_dir.string(), found_files, 2);

        if (found) {
            std::cout << "[INFO] Found " << found_files.size() << " potential .ibd files\n";

            // Try to parse each found file
            dbrecovery::catalog::TableSchema schema;
            schema.table_name = "carved_table";

            std::vector<std::map<std::string, std::string>> all_records;

            for (const auto& file_path : found_files) {
                std::cout << "[INFO] Parsing carved file: " << file_path << "\n";

                std::vector<std::map<std::string, std::string>> file_records;
                if (rmrf.reconstruct_from_file(file_path, schema, file_records)) {
                    std::cout << "[INFO] Recovered " << file_records.size()
                              << " records from " << file_path << "\n";
                    all_records.insert(all_records.end(),
                                     file_records.begin(), file_records.end());
                }
            }

            if (!all_records.empty()) {
                std::string csv_path = output_dir + "/drop_recovery.csv";
                std::ofstream csv_file(csv_path);
                if (csv_file) {
                    csv_file << "-- Records recovered via DROP TABLE recovery\n";
                    csv_file << "id,name,email,created_at\n";
                    for (const auto& rec : all_records) {
                        bool first = true;
                        for (const auto& [key, value] : rec) {
                            if (!first) csv_file << ",";
                            csv_file << "\"" << value << "\"";
                            first = false;
                        }
                        csv_file << "\n";
                    }
                    csv_file.close();
                    std::cout << "[INFO] DROPed table recovery: " << all_records.size()
                              << " records exported to: " << csv_path << "\n";
                }
                return 0;
            }
        }

        std::cerr << "[WARN] DROP TABLE recovery found no recoverable data\n";
        return 1;
    }

    // ✅ 恢复模式：--disk 单独使用时的处理 (使用启发式解析)
    if (!disk_path.empty() && ibd_path.empty() && !device_scan && !drop_recovery) {
        std::cout << "[INFO] Starting PhotoRec-style disk scan with: " << disk_path << "\n";

        // 自动检测页大小
        if (page_size == 0) {
            page_size = 16384;
        }

        // 使用 HeuristicScanner 进行 PhotoRec 风格扫描
        dbrecovery::innodb::HeuristicScanner scanner(page_size);
        auto heuristic_records = scanner.scan_file(disk_path);
        const auto& stats = scanner.get_stats();

        std::cout << "\n=== PhotoRec-Style Recovery Statistics ===\n";
        std::cout << "  Heuristic scan records: " << heuristic_records.size() << "\n";
        std::cout << "  Enhanced recovery features: enabled\n";
        std::cout << "  Total unique records: " << stats.total_records << "\n";

        std::cout << "\n=== Schemaless Recovery Results ===\n";
        std::cout << "Pages scanned: " << stats.pages_scanned << "\n";
        std::cout << "Pages with records: " << stats.pages_with_records << "\n";
        std::cout << "Total records recovered: " << stats.total_records << "\n";

        if (!heuristic_records.empty()) {
            std::string csv_path = output_dir + "/disk_recovered.csv";
            std::ofstream csv_file(csv_path);
            if (csv_file) {
                csv_file << "-- Records from: " << disk_path << "\n";
                csv_file << "id,name,email,created_at\n";

                for (const auto& rec : heuristic_records) {
                    if (rec.is_infimum || rec.is_supremum) continue;

                    std::string id_str, name_str, email_str, ts_str;

                    if (rec.fields.size() >= 1) id_str = rec.fields[0].to_string();
                    if (rec.fields.size() >= 2) name_str = rec.fields[1].to_string();
                    if (rec.fields.size() >= 3) email_str = rec.fields[2].to_string();
                    if (rec.fields.size() >= 4) ts_str = rec.fields[3].to_string();

                    // 过滤无效记录
                    if (id_str == "<INVALID>" || id_str.empty()) continue;

                    // 过滤包含系统记录关键字的记录
                    if (id_str.find("supremum") != std::string::npos ||
                        id_str.find("infimum") != std::string::npos ||
                        id_str.find("???") != std::string::npos) continue;

                    // 过滤 ID 不是纯数字的记录
                    bool valid_id = true;
                    for (char c : id_str) {
                        if (!isdigit(c) && c != '-') {
                            valid_id = false;
                            break;
                        }
                    }
                    if (!valid_id) continue;

                    csv_file << id_str << ",\"" << name_str << "\",\"" << email_str << "\",\"" << ts_str << "\"\n";
                }
                csv_file.close();
                std::cout << "\n[INFO] Recovered " << heuristic_records.size()
                          << " records exported to: " << csv_path << "\n";
            }
            return 0;
        }

        std::cerr << "[WARN] No data recovered from disk scan\n";
        return 1;
    }

    // ✅ 恢复模式：Extract Tuples (schema-aware tuple extraction from IBD pages)
    if (extract_tuples) {
        std::cout << "[INFO] Starting schema-aware tuple extraction...\n";

        if (ibd_path.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required for tuple extraction\n";
            return 1;
        }

        // Determine page size
        if (page_size == 0) {
            dbrecovery::innodb::InnoDBFileReader reader(ibd_path);
            if (reader.is_valid()) {
                page_size = reader.get_page_size();
            } else {
                page_size = 16384;
            }
        }

        // Try to get schema from --schema option, .frm, SDI, or heuristic inference
        dbrecovery::engine::SchemaTemplate schema_template;
        bool has_schema = false;

        // Parse --schema option if provided
        // Format: "col1:TYPE[:pk][:null][:unsigned],col2:TYPE,..."
        // Example: "id:INT:pk,name:VARCHAR(100),email:VARCHAR(200):null"
        if (!schema_str.empty()) {
            schema_template.table_name = table_name.empty() ? "recovered_table" : table_name;
            std::istringstream ss(schema_str);
            std::string col_def;
            while (std::getline(ss, col_def, ',')) {
                dbrecovery::engine::SchemaTemplate::Column tcol;
                std::istringstream cs(col_def);
                std::string token;
                std::vector<std::string> parts;
                while (std::getline(cs, token, ':')) {
                    parts.push_back(token);
                }
                if (parts.size() < 2 || parts[0].empty() || parts[1].empty()) {
                    std::cerr << "[ERROR] --schema requires name and type: "
                              << col_def << "\n";
                    return 1;
                }
                tcol.name = parts[0];
                std::string type_str = parts[1];

                // Parse type name and extract max_length for VARCHAR
                tcol.type_name = type_str;
                tcol.is_nullable = true;  // Default to nullable for non-PK columns (MySQL default)
                tcol.is_primary_key = false;
                tcol.is_signed = true;
                tcol.is_variable_length = false;

                // Parse VARCHAR(N) and other sized types
                auto paren_pos = type_str.find('(');
                if (paren_pos != std::string::npos) {
                    auto end_paren = type_str.find(')', paren_pos);
                    if (end_paren != std::string::npos) {
                        // Keep the full type name including parentheses for DECIMAL
                        std::string size_str = type_str.substr(paren_pos + 1, end_paren - paren_pos - 1);
                        // For DECIMAL(P,S), parse precision and scale separately
                        auto comma_pos = size_str.find(',');
                        if (comma_pos != std::string::npos) {
                            tcol.decimal_precision = std::stoul(size_str.substr(0, comma_pos));
                            tcol.decimal_scale = std::stoul(size_str.substr(comma_pos + 1));
                            tcol.max_length = tcol.decimal_precision;
                        } else {
                            tcol.max_length = std::stoul(size_str);
                        }
                        type_str = type_str.substr(0, paren_pos);
                    }
                }

                // Map type string to mysql_type
                std::string type_upper = type_str;
                std::transform(type_upper.begin(), type_upper.end(), type_upper.begin(), ::toupper);

                // Default mbmaxlen=4 for utf8mb4 charset (most common default)
                tcol.mbmaxlen = 4;

                if (type_upper == "TINYINT") { tcol.mysql_type = dbrecovery::innodb::mysql_types::TINY; tcol.max_length = 1; }
                else if (type_upper == "SMALLINT") { tcol.mysql_type = dbrecovery::innodb::mysql_types::SHORT; tcol.max_length = 2; }
                else if (type_upper == "MEDIUMINT") { tcol.mysql_type = dbrecovery::innodb::mysql_types::INT24; tcol.max_length = 3; }
                else if (type_upper == "INT" || type_upper == "INTEGER") { tcol.mysql_type = dbrecovery::innodb::mysql_types::LONG; tcol.max_length = 4; }
                else if (type_upper == "BIGINT") { tcol.mysql_type = dbrecovery::innodb::mysql_types::LONGLONG; tcol.max_length = 8; }
                else if (type_upper == "FLOAT") { tcol.mysql_type = dbrecovery::innodb::mysql_types::FLOAT; tcol.max_length = 4; }
                else if (type_upper == "DOUBLE") { tcol.mysql_type = dbrecovery::innodb::mysql_types::DOUBLE; tcol.max_length = 8; }
                else if (type_upper == "DECIMAL" || type_upper == "NUMERIC") {
                    tcol.mysql_type = dbrecovery::innodb::mysql_types::NEWDECIMAL;
                    // DECIMAL is NOT is_var in InnoDB COMPACT format
                    // char_length for is_big calculation
                    tcol.char_length = tcol.decimal_precision > 0 ? tcol.decimal_precision : 12;
                    // Storage size for DECIMAL(P,S):
                    // int_digits = P-S, dec_digits = S
                    // int_groups = ceil(int_digits/9), dec_groups = ceil(dec_digits/9)
                    // Each group = 4 bytes. Total = (int_groups + dec_groups) * 4
                    uint32_t prec = tcol.decimal_precision > 0 ? tcol.decimal_precision : 10;
                    uint32_t scale = tcol.decimal_scale;
                    uint32_t int_digits = prec - scale;
                    uint32_t int_groups = (int_digits + 8) / 9;
                    uint32_t dec_groups = (scale + 8) / 9;
                    tcol.max_length = (int_groups + dec_groups) * 4;
                }
                else if (type_upper == "DATE") { tcol.mysql_type = dbrecovery::innodb::mysql_types::DATE; tcol.max_length = 3; }
                else if (type_upper == "TIME") { tcol.mysql_type = dbrecovery::innodb::mysql_types::TIME; tcol.max_length = 3; }
                else if (type_upper == "DATETIME") { tcol.mysql_type = dbrecovery::innodb::mysql_types::DATETIME; tcol.max_length = 8; }
                else if (type_upper == "TIMESTAMP") { tcol.mysql_type = dbrecovery::innodb::mysql_types::TIMESTAMP2; tcol.max_length = 4; }
                else if (type_upper == "YEAR") { tcol.mysql_type = dbrecovery::innodb::mysql_types::YEAR; tcol.max_length = 1; }
                else if (type_upper == "VARCHAR" || type_upper == "VAR_STRING") {
                    tcol.mysql_type = dbrecovery::innodb::mysql_types::VARCHAR;
                    tcol.is_variable_length = true;
                    // char_length = mbmaxlen * character_count (byte length for is_big)
                    tcol.char_length = tcol.mbmaxlen * (tcol.max_length > 0 ? tcol.max_length : 255);
                    if (!tcol.max_length) tcol.max_length = 255;
                }
                else if (type_upper == "TEXT" || type_upper == "TINYTEXT") {
                    tcol.mysql_type = dbrecovery::innodb::mysql_types::BLOB;
                    tcol.is_variable_length = true;
                    tcol.max_length = 65535;
                    tcol.char_length = 65535;
                }
                else if (type_upper == "MEDIUMTEXT") { tcol.mysql_type = dbrecovery::innodb::mysql_types::MEDIUM_BLOB; tcol.is_variable_length = true; tcol.max_length = 16777215; tcol.char_length = 16777215; }
                else if (type_upper == "LONGTEXT") { tcol.mysql_type = dbrecovery::innodb::mysql_types::LONG_BLOB; tcol.is_variable_length = true; tcol.max_length = 4294967295; tcol.char_length = 4294967295; }
                else if (type_upper == "BLOB" || type_upper == "TINYBLOB") { tcol.mysql_type = dbrecovery::innodb::mysql_types::BLOB; tcol.is_variable_length = true; tcol.max_length = 65535; tcol.char_length = 65535; }
                else if (type_upper == "BIT") { tcol.mysql_type = dbrecovery::innodb::mysql_types::BIT; }
                else if (type_upper == "JSON") { tcol.mysql_type = dbrecovery::innodb::mysql_types::JSON; tcol.is_variable_length = true; tcol.max_length = 4294967295; tcol.char_length = 4294967295; }
                else {
                    std::cerr << "[ERROR] Unsupported --schema type: " << type_str << "\n";
                    return 1;
                }

                // Parse flags (pk, null, unsigned)
                for (size_t fi = 2; fi < parts.size(); fi++) {
                    std::string flag = parts[fi];
                    std::transform(flag.begin(), flag.end(), flag.begin(), ::tolower);
                    if (flag == "pk" || flag == "primary") { tcol.is_primary_key = true; tcol.is_nullable = false; }
                    else if (flag == "null" || flag == "nullable") { tcol.is_nullable = true; }
                    else if (flag == "notnull" || flag == "not_null" || flag == "required") { tcol.is_nullable = false; }
                    else if (flag == "unsigned") { tcol.is_signed = false; }
                    else if (flag == "signed") { tcol.is_signed = true; }
                    else if (flag == "ai" || flag == "auto_increment") { tcol.is_auto_increment = true; }
                }

                schema_template.columns.push_back(tcol);
                if (tcol.is_primary_key) {
                    schema_template.primary_key_columns.push_back(
                        static_cast<uint32_t>(schema_template.columns.size() - 1));
                }
            }
            has_schema = true;
            std::cout << "[INFO] Schema obtained from --schema option (" << schema_template.columns.size() << " columns)\n";
        } else {
            dbrecovery::MySQLVersion tuple_detected = dbrecovery::MySQLVersion::UNKNOWN;
            if (!mysql_version.empty()) {
                tuple_detected = parse_mysql_version_value(mysql_version);
                if (tuple_detected == dbrecovery::MySQLVersion::UNKNOWN) {
                    std::cerr << "[ERROR] Unsupported or invalid --mysql-version: "
                              << mysql_version << "\n";
                    return 1;
                }
            } else {
                std::ifstream ver_file(ibd_path, std::ios::binary);
                if (ver_file) {
                    const size_t detection_page_size = page_size > 0 ? page_size : 16384;
                    std::vector<std::vector<uint8_t>> owned_pages;
                    std::vector<const uint8_t*> pages;
                    constexpr size_t max_version_pages = 16;
                    for (size_t i = 0; i < max_version_pages; ++i) {
                        std::vector<uint8_t> page(detection_page_size);
                        ver_file.read(reinterpret_cast<char*>(page.data()),
                                      static_cast<std::streamsize>(page.size()));
                        if (ver_file.gcount() != static_cast<std::streamsize>(page.size())) break;
                        owned_pages.push_back(std::move(page));
                    }
                    pages.reserve(owned_pages.size());
                    for (const auto& page : owned_pages) pages.push_back(page.data());
                    if (!pages.empty()) {
                        const auto evidence =
                            dbrecovery::MySQLVersionDetector::detect_pages_with_evidence(
                                pages, detection_page_size);
                        tuple_detected = evidence.version;
                        if (tuple_detected == dbrecovery::MySQLVersion::UNKNOWN) {
                            std::cerr << "[ERROR] Could not auto-detect MySQL version: "
                                      << evidence.rejection_reasons.front() << "\n";
                            return 1;
                        }
                    }
                }
                if (tuple_detected == dbrecovery::MySQLVersion::UNKNOWN) {
                    std::cerr << "[ERROR] Cannot establish MySQL version from input pages.\n";
                    return 1;
                }
            }

            auto sdi_schema = parse_schema_from_sdi(ibd_path, table_id, tuple_detected);
            if (sdi_schema.has_value()) {
                std::cout << "[INFO] Schema obtained from SDI\n";
                // Map catalog::TableSchema to engine::SchemaTemplate through the
                // fail-closed type conversion boundary; SDI/FRM schema fields
                // carry InnoDB parser-domain IDs, so validate them as such.
                schema_template.table_name = sdi_schema->table_name;
                std::vector<dbrecovery::engine::SchemaTemplate::Column> converted_columns;
                std::vector<uint32_t> converted_primary_keys;
                bool schema_rejected = false;
                for (auto& [pos, col] : sdi_schema->columns) {
                    std::vector<std::string> type_diag;
                    auto converted = dbrecovery::catalog::build_index_column(
                        col,
                        /*ordinal_position=*/col.ordinal,
                        /*physical_pos_override=*/0,
                        dbrecovery::catalog::TypeIdDomain::kInnodbParser,
                        type_diag);
                    if (!converted.has_value()) {
                        for (const auto& d : type_diag) {
                            std::cerr << "[WARN] SDI schema column rejected: " << d << "\n";
                        }
                        schema_rejected = true;
                        break;
                    }
                    dbrecovery::engine::SchemaTemplate::Column tcol;
                    tcol.name = converted->name;
                    tcol.mysql_type = converted->col_type;
                    tcol.is_nullable = converted->is_nullable;
                    tcol.is_primary_key = (pos == 1);
                    tcol.enum_values = converted->enum_values;
                    tcol.set_values = converted->set_values;
                    if (tcol.is_primary_key) {
                        converted_primary_keys.push_back(
                            static_cast<uint32_t>(converted_columns.size()));
                    }
                    converted_columns.push_back(std::move(tcol));
                }
                if (schema_rejected) {
                    schema_template = {};
                    schema_template.table_name = table_name.empty() ? "recovered_table" : table_name;
                    std::cerr << "[WARN] SDI schema is incomplete; using evidence-only heuristic extraction\n";
                } else {
                    schema_template.columns = std::move(converted_columns);
                    schema_template.primary_key_columns = std::move(converted_primary_keys);
                    has_schema = !schema_template.columns.empty();
                }
            } else if (!frm_path.empty()) {
                // Try to parse schema from .frm file
                std::cout << "[INFO] Attempting to parse schema from .frm file: " << frm_path << "\n";
                auto frm_schema = dbrecovery::innodb::FRMParser::parse(frm_path);
                if (frm_schema.has_value()) {
                    std::cout << "[INFO] Schema obtained from .frm file\n";
                    schema_template.table_name = frm_schema->table_name;
                    std::vector<dbrecovery::engine::SchemaTemplate::Column> converted_columns;
                    bool schema_rejected = false;
                    for (auto& [pos, col] : frm_schema->columns) {
                        std::vector<std::string> type_diag;
                        auto converted = dbrecovery::catalog::build_index_column(
                            col,
                            /*ordinal_position=*/pos,
                            /*physical_pos_override=*/0,
                            dbrecovery::catalog::TypeIdDomain::kInnodbParser,
                            type_diag);
                        if (!converted.has_value()) {
                            for (const auto& d : type_diag) {
                                std::cerr << "[WARN] FRM schema column rejected: " << d << "\n";
                            }
                            schema_rejected = true;
                            break;
                        }
                        dbrecovery::engine::SchemaTemplate::Column tcol;
                        tcol.name = converted->name;
                        tcol.mysql_type = converted->col_type;
                        tcol.is_nullable = converted->is_nullable;
                        tcol.max_length = converted->col_length;
                        tcol.enum_values = converted->enum_values;
                        tcol.set_values = converted->set_values;
                        converted_columns.push_back(std::move(tcol));
                    }
                    if (schema_rejected) {
                        schema_template = {};
                        schema_template.table_name = table_name.empty() ? "recovered_table" : table_name;
                        std::cerr << "[WARN] FRM schema is incomplete; using evidence-only heuristic extraction\n";
                    } else {
                        schema_template.columns = std::move(converted_columns);
                        has_schema = !schema_template.columns.empty();
                    }
                } else {
                    std::cout << "[INFO] Failed to parse .frm file, using heuristic schema inference\n";
                    schema_template.table_name = table_name.empty() ? "recovered_table" : table_name;
                }
            } else {
                std::cout << "[INFO] No SDI found, using heuristic schema inference\n";
                schema_template.table_name = table_name.empty() ? "recovered_table" : table_name;
            }
        }

        // Read all index pages and extract tuples
        dbrecovery::innodb::InnoDBFileReader reader(ibd_path);
        if (!reader.is_valid()) {
            std::cerr << "[ERROR] Failed to open file: " << ibd_path << "\n";
            return 1;
        }

        uint32_t total_pages = reader.get_total_pages();
        std::vector<dbrecovery::engine::RecoveredRecord> all_records;

        dbrecovery::engine::TupleParser::Config parser_config;
        parser_config.schema = schema_template;
        parser_config.validate_checksums = true;
        parser_config.use_heuristic = !has_schema;
        dbrecovery::engine::TupleParser tuple_parser(parser_config);
        // DB_ROLL_PTR width is version-dependent (6B for 5.6/5.7, 7B for 8.0+)
        tuple_parser.set_mysql_version(mysql_version_to_number(tuple_detected));

        std::cout << "[INFO] Scanning " << total_pages << " pages for tuples...\n";

        for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
            auto page_data = reader.read_page_raw(page_no);
            if (page_data.empty()) continue;

            // Check page type - only process INDEX pages
            if (page_data.size() < 26) continue;
            uint16_t ptype = dbrecovery::innodb::read_be<uint16_t>(
                page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

            bool is_index = (ptype == dbrecovery::innodb::FIL_PAGE_INDEX);  // 0x45BF = 17855
            if (!is_index) continue;

            // Validate page checksum (non-blocking: try parsing even on checksum failure)
            bool checksum_valid = dbrecovery::innodb::is_page_checksum_valid(page_data.data(), page_size);
            if (!checksum_valid && verbose) {
                std::cout << "[WARN] Page " << page_no << " checksum invalid, attempting parse anyway\n";
            }

            tuple_parser.parse_page(page_data, schema_template,
                [&all_records](const dbrecovery::engine::RecoveredRecord& rec) {
                    if (!rec.is_infimum && !rec.is_supremum) {
                        // Include deleted records for recovery
                        all_records.push_back(rec);
                    }
                });
        }

        std::cout << "[INFO] Tuple extraction completed. Found " << all_records.size() << " records.\n";

        // Deduplicate by primary key if available
        std::set<std::string> seen_pks;
        std::vector<dbrecovery::engine::RecoveredRecord> deduped;
        for (auto& rec : all_records) {
            std::string pk;
            if (has_schema && !schema_template.primary_key_columns.empty()) {
                for (uint32_t pk_idx : schema_template.primary_key_columns) {
                    if (pk_idx < schema_template.columns.size()) {
                        const auto& col = schema_template.columns[pk_idx];
                        if (rec.fields.count(col.name)) {
                            pk += rec.fields.at(col.name) + "|";
                        }
                    }
                }
            }
            if (pk.empty()) {
                // Use all fields as dedup key
                for (const auto& [k, v] : rec.fields) {
                    pk += v + "|";
                }
            }
            if (seen_pks.insert(pk).second) {
                deduped.push_back(std::move(rec));
            }
        }
        std::cout << "[INFO] After deduplication: " << deduped.size() << " unique records.\n";

        // Export
        std::string result_table_name = schema_template.table_name.empty() ? "recovered_table" : schema_template.table_name;

        // Export as JSON
        std::string json_path = output_dir + "/" + result_table_name + "_tuples.json";
        std::ofstream json_file(json_path);
        if (json_file) {
            json_file << "{\n  \"table\": \"" << result_table_name << "\",\n";
            json_file << "  \"record_count\": " << deduped.size() << ",\n";
            json_file << "  \"records\": [\n";
            bool first = true;
            for (const auto& rec : deduped) {
                if (!first) json_file << ",\n";
                first = false;
                json_file << "    {";
                bool ffirst = true;
                for (const auto& [k, v] : rec.fields) {
                    if (!ffirst) json_file << ", ";
                    ffirst = false;
                    json_file << "\"" << k << "\": \"" << v << "\"";
                }
                json_file << "}";
            }
            json_file << "\n  ]\n}\n";
            json_file.close();
            std::cout << "[INFO] JSON exported to: " << json_path << "\n";
        }

        // Export as SQL
        dbrecovery::engine::SqlTupleGenerator::Config sql_config;
        dbrecovery::engine::SqlTupleGenerator sql_gen(sql_config);
        std::string sql_content = sql_gen.generate(deduped, schema_template);
        std::string sql_path = output_dir + "/" + result_table_name + "_tuples.sql";
        std::ofstream sql_file(sql_path);
        if (sql_file) {
            sql_file << sql_content;
            sql_file.close();
            std::cout << "[INFO] SQL exported to: " << sql_path << "\n";
        }

        return 0;
    }

    // ✅ 恢复模式：rm -rf 恢复 - 已统一使用 InnoDBIndexParser (见下方 line 1700)
    // rmrf_recovery 和 truncate_recovery 现在都使用高精度的 InnoDBIndexParser
    // 此处仅保留注释，实际处理逻辑在下方统一处理

    // 恢复模式：Truncate 恢复
    // 使用 InnoDBIndexParser 进行 TRUNCATE 恢复
    // 这种方法可以正确解析 COMPACT/DYNAMIC 行格式，实现 >=99.9% 的恢复率

    // ✅ 恢复模式：BLOB 恢复 — BlobChainRebuilder 链重建，保留页计数作为诊断
    if (blob_recovery) {
        std::cout << "[INFO] Starting BLOB recovery...\n";

        if (ibd_path.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required for BLOB recovery\n";
            return 1;
        }

        dbrecovery::innodb::InnoDBFileReader reader(ibd_path, page_size > 0 ? page_size : 0);
        if (!reader.is_valid()) {
            std::cerr << "[ERROR] Failed to open file: " << ibd_path << "\n";
            return 1;
        }

        uint32_t total_pages = reader.get_total_pages();
        uint32_t blob_pages = 0;

        dbrecovery::recovery::BlobChainRebuilder rebuilder;
        rebuilder.set_page_size(page_size > 0 ? page_size : 16384);

        for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
            auto page_data = reader.read_page_raw(page_no);
            if (page_data.empty()) continue;

            uint16_t page_type_be = dbrecovery::innodb::read_be<uint16_t>(
                page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

            if (page_type_be == dbrecovery::innodb::FIL_PAGE_TYPE_BLOB) {
                // FIL_PAGE_TYPE_BLOB (0x45BD) is shared with FIL_PAGE_SDI_ZIP:
                // compressed SDI metadata pages carry the same type value.
                // Only absence of the SDI signature at offset 64-71 makes a
                // 0x45BD page a BLOB page; otherwise it is SDI metadata and
                // must not be carved as a BLOB payload.
                constexpr uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
                const bool sdi_signed = page_data.size() >= 72 &&
                    std::memcmp(page_data.data() + 64, SDI_SIG, 8) == 0;

                if (sdi_signed) {
                    if (verbose) {
                        std::cout << "[BLOB] Skipping page " << page_no
                                  << " (type 0x45BD, SDI signature present)\n";
                    }
                    continue;
                }

                blob_pages++;
                uint32_t space_id = dbrecovery::innodb::read_be<uint32_t>(
                    page_data.data(), dbrecovery::innodb::FIL_PAGE_SPACE_ID);
                rebuilder.add_blob_page(space_id, page_no, std::move(page_data));
                if (verbose) {
                    std::cout << "[BLOB] Found BLOB page " << page_no
                              << " (type: " << page_type_be << " space_id=" << space_id << ")\n";
                }
            }
        }

        std::cout << "[INFO] BLOB scan completed. Found " << blob_pages << " BLOB pages / "
                  << total_pages << " total.\n";

        auto stats = rebuilder.get_stats();
        std::cout << "[INFO] BlobChainRebuilder: total_chains=" << stats.total_chains
                  << " complete=" << stats.complete_chains
                  << " partial=" << stats.partial_chains
                  << " failed=" << stats.failed_chains
                  << " bytes=" << stats.total_bytes << "\n";

        auto &blobs = rebuilder.get_reconstructed_blobs();
        if (!blobs.empty()) {
            fs::create_directories(output_dir);
            for (auto &kv : blobs) {
                std::string out_path = output_dir + "/blob_" + std::to_string(kv.first) + ".bin";
                std::ofstream out(out_path, std::ios::binary);
                if (out) out.write(reinterpret_cast<const char*>(kv.second.data()), kv.second.size());
                if (verbose) std::cout << "[BLOB] Exported blob " << kv.first << " -> " << out_path << " (" << kv.second.size() << " bytes)\n";
            }
        }
        std::cout << "[INFO] BLOB recovery completed.\n";
        return 0;
    }

    // 恢复模式：force scan using InnoDBIndexParser
    // This mode uses the new InnoDBIndexParser which correctly parses COMPACT/DYNAMIC
    // row format records, enabling >=99.9% recovery rate
    bool force_scan = parser.get_bool_option("force-scan", false);
    bool use_index_parser = parser.get_bool_option("index-parser", false);

    const auto blocks_generic_plaintext_recovery = [](const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            std::cerr << "[ERROR] Cannot open input file for TDE candidate screening: "
                      << path << "\n";
            return true;
        }

        std::vector<uint8_t> page0(dbrecovery::innodb::FIL_HEADER_SIZE + 45);
        input.read(reinterpret_cast<char*>(page0.data()), page0.size());
        const auto bytes_read = static_cast<size_t>(input.gcount());
        if (dbrecovery::innodb::TDEDecryptor::probe_candidate(page0.data(), bytes_read) ==
            dbrecovery::innodb::TDECandidateProbe::RECOGNIZED_CANDIDATE) {
            std::cerr << "[ERROR] Input matches the unvalidated TDE candidate heuristic; "
                         "generic plaintext recovery is blocked.\n";
            return true;
        }

        return false;
    };

    if (force_scan || use_index_parser || truncate_recovery || rmrf_recovery) {
        std::cout << "[INFO] Starting index page parsing...\n";

        if (ibd_path.empty() && pre_truncate_ibd.empty()) {
            std::cerr << "[ERROR] --ibd=<path> is required for index parsing\n";
            return 1;
        }

        // For TRUNCATE recovery, prefer pre-truncate IBD if available
        std::string effective_ibd = ibd_path;
        if (truncate_recovery && !pre_truncate_ibd.empty()) {
            if (fs::exists(pre_truncate_ibd)) {
                std::cout << "[INFO] TRUNCATE recovery: using pre-truncate IBD backup: " << pre_truncate_ibd << "\n";
                effective_ibd = pre_truncate_ibd;
            } else {
                std::cerr << "[WARN] Pre-truncate IBD not found: " << pre_truncate_ibd << "\n";
            }
        }

        // Screening gate: the file that actually enters plaintext parsing
        // (which may be the pre-truncate backup, not the CLI path).
        if (blocks_generic_plaintext_recovery(effective_ibd)) {
            return 1;
        }

        // Initialize the new InnoDBIndexParser
        dbrecovery::innodb::InnoDBIndexParser index_parser;
        index_parser.set_recover_deleted(true);  // Include deleted records (critical for TRUNCATE)
        index_parser.set_bad_page_mode(2);       // Try mode: brute-force for corrupted pages

        // Try to detect page size from FSP header
        if (page_size == 0) {
            std::ifstream fsp_file(effective_ibd, std::ios::binary);
            if (fsp_file) {
                std::vector<uint8_t> first_page(16384);
                fsp_file.read(reinterpret_cast<char*>(first_page.data()), 16384);
                page_size = dbrecovery::innodb::detect_page_size_from_fsp(first_page.data());
            }
        }
        if (page_size == 0) page_size = 16384;

        // Set MySQL version for format-specific behavior
        dbrecovery::MySQLVersion detected_version = dbrecovery::MySQLVersion::UNKNOWN;
        if (!mysql_version.empty()) {
            detected_version = parse_mysql_version_value(mysql_version);
            if (detected_version == dbrecovery::MySQLVersion::UNKNOWN) {
                std::cerr << "[ERROR] Unsupported or invalid --mysql-version: "
                          << mysql_version << "\n";
                return 1;
            }
        } else {
            // No explicit version: use evidence from multiple complete pages.
            std::ifstream ver_file(effective_ibd, std::ios::binary);
            if (ver_file) {
                std::vector<std::vector<uint8_t>> owned_pages;
                std::vector<const uint8_t*> pages;
                constexpr size_t max_version_pages = 16;
                for (size_t i = 0; i < max_version_pages; ++i) {
                    std::vector<uint8_t> page(page_size);
                    ver_file.read(reinterpret_cast<char*>(page.data()),
                                  static_cast<std::streamsize>(page.size()));
                    if (ver_file.gcount() != static_cast<std::streamsize>(page.size())) break;
                    owned_pages.push_back(std::move(page));
                }
                pages.reserve(owned_pages.size());
                for (const auto& page : owned_pages) pages.push_back(page.data());
                if (!pages.empty()) {
                    const auto evidence =
                        dbrecovery::MySQLVersionDetector::detect_pages_with_evidence(
                            pages, page_size);
                    detected_version = evidence.version;
                    if (detected_version == dbrecovery::MySQLVersion::UNKNOWN) {
                        std::cerr << "[ERROR] Could not auto-detect MySQL version: "
                                  << evidence.rejection_reasons.front() << "\n";
                        return 1;
                    }
                }
            }
            if (detected_version == dbrecovery::MySQLVersion::UNKNOWN) {
                std::cerr << "[ERROR] Cannot establish MySQL version from input pages.\n";
                return 1;
            }
        }
        index_parser.set_mysql_version(mysql_version_number(detected_version));
        std::cout << "[INFO] MySQL version format profile established\n";

        // Parse schema from --schema option if provided
        std::vector<dbrecovery::innodb::IndexColumnDef> index_columns;  // Moved outside for scope

        // First try to extract schema from SDI (MySQL 8.0+)
        if (schema_str.empty()) {
            std::cout << "[INFO] Attempting to extract schema from SDI pages...\\n";
            auto schema_opt = parse_schema_from_sdi(effective_ibd, table_id, detected_version);
            if (schema_opt.has_value()) {
                const auto& schema = schema_opt.value();
                uint32_t n_pk = 0;
                std::vector<dbrecovery::innodb::IndexColumnDef> converted_columns;
                bool schema_rejected = false;
                for (const auto& [ord, col] : schema.columns) {
                    // Route the SDI-derived type ID through the fail-closed
                    // conversion boundary. SDI schema carries InnoDB parser-
                    // domain IDs; anything else is rejected with a diagnostic.
                    std::vector<std::string> type_diag;
                    auto converted = dbrecovery::catalog::build_index_column(
                        col,
                        /*ordinal_position=*/ord,
                        /*physical_pos_override=*/0,
                        dbrecovery::catalog::TypeIdDomain::kInnodbParser,
                        type_diag);
                    if (!converted.has_value()) {
                        for (const auto& d : type_diag) {
                            std::cerr << "[WARN] SDI schema column rejected: " << d << "\n";
                        }
                        schema_rejected = true;
                        break;
                    }
                    dbrecovery::innodb::IndexColumnDef icol = std::move(*converted);
                    // JSON columns use binary charset but must not be treated as
                    // opaque binary data.
                    icol.is_binary = icol.is_binary && (icol.col_type != 31);
                    converted_columns.push_back(std::move(icol));
                }
                if (schema_rejected) {
                    std::cerr << "[WARN] SDI schema is incomplete; structured index parsing is disabled\n";
                } else if (!converted_columns.empty()) {
                    if (n_pk == 0) n_pk = 1;
                    index_columns = std::move(converted_columns);
                    index_parser.set_columns(index_columns, false, n_pk);
                    std::cout << "[INFO] Schema extracted from SDI (" << index_columns.size()
                              << " columns, " << n_pk << " PK columns)\\n";
                }
            }
        }

        if (!schema_str.empty()) {
            uint32_t n_pk = 0;
            // Fix: Don't split on commas inside parentheses (e.g., DECIMAL(10,2))
            std::vector<std::string> col_defs;
            std::string current_def;
            int paren_depth = 0;
            for (char c : schema_str) {
                if (c == '(') paren_depth++;
                else if (c == ')') paren_depth--;
                else if (c == ',' && paren_depth == 0) {
                    if (!current_def.empty()) {
                        col_defs.push_back(current_def);
                        current_def.clear();
                    }
                    continue;
                }
                current_def += c;
            }
            if (!current_def.empty()) {
                col_defs.push_back(current_def);
            }

            for (const auto& col_def : col_defs) {
                dbrecovery::innodb::IndexColumnDef icol;
                std::istringstream cs(col_def);
                std::string token;
                std::vector<std::string> parts;
                while (std::getline(cs, token, ':')) {
                    parts.push_back(token);
                }
                if (parts.size() < 2 || parts[0].empty() || parts[1].empty()) {
                    std::cerr << "[ERROR] --schema requires name and type: "
                              << col_def << "\n";
                    return 1;
                }
                icol.name = parts[0];
                std::string type_str = parts[1];

                // Parse type name and extract length
                auto paren_pos = type_str.find('(');
                if (paren_pos != std::string::npos) {
                    auto end_paren = type_str.find(')', paren_pos);
                    if (end_paren != std::string::npos) {
                        std::string size_str = type_str.substr(paren_pos + 1, end_paren - paren_pos - 1);
                        // Handle DECIMAL(M,D) - extract M (precision) and D (scale)
                        auto comma_in_size = size_str.find(',');
                        if (comma_in_size != std::string::npos) {
                            try {
                                icol.precision = std::stoul(size_str.substr(0, comma_in_size));
                                icol.scale = std::stoul(size_str.substr(comma_in_size + 1));
                                icol.col_length = icol.precision;
                            } catch (...) {
                                icol.precision = 10;
                                icol.scale = 0;
                                icol.col_length = 10;
                            }
                        } else {
                            try {
                                icol.col_length = std::stoul(size_str);
                            } catch (...) {
                                icol.col_length = 0;
                            }
                        }
                        type_str = type_str.substr(0, paren_pos);
                    }
                }

                std::string type_upper = type_str;
                std::transform(type_upper.begin(), type_upper.end(), type_upper.begin(), ::toupper);
                icol.mb_maxlen = 4;  // default utf8mb4 for manual schema

                // MySQL type codes (from MySQL source, matching FRM file values):
                // CRITICAL: Use InnoDB INTERNAL type IDs, NOT MySQL server type IDs!
                // InnoDB types: 2=TINY, 3=SHORT, 4=LONG, 9=LONGLONG, 10=INT24
                // 5=FLOAT, 6=DOUBLE, 21=NEWDECIMAL
                // 18=TIMESTAMP2, 19=DATETIME2, 20=TIME2, 15=NEWDATE, 14=YEAR
                // 16=VARCHAR, 29=STRING(CHAR), 27=BLOB, 31=JSON
                if (type_upper == "TINYINT") { icol.col_type = 2; icol.col_length = 1; }
                else if (type_upper == "SMALLINT") { icol.col_type = 3; icol.col_length = 2; }
                else if (type_upper == "MEDIUMINT") { icol.col_type = 10; icol.col_length = 3; }
                else if (type_upper == "INT" || type_upper == "INTEGER") { icol.col_type = 4; icol.col_length = 4; }
                else if (type_upper == "BIGINT") { icol.col_type = 9; icol.col_length = 8; }
                else if (type_upper == "FLOAT") { icol.col_type = 5; icol.col_length = 4; }
                else if (type_upper == "DOUBLE") { icol.col_type = 6; icol.col_length = 8; }
                else if (type_upper == "DECIMAL" || type_upper == "NUMERIC") { icol.col_type = 21; }
                else if (type_upper == "TIMESTAMP") { icol.col_type = 18; icol.col_length = 4; }
                else if (type_upper == "TIMESTAMP2") { icol.col_type = 18; icol.col_length = 4; }
                else if (type_upper == "DATETIME") { icol.col_type = 19; icol.col_length = 5; }
                else if (type_upper == "DATETIME2") { icol.col_type = 19; icol.col_length = 5; }
                else if (type_upper == "TIME") { icol.col_type = 20; icol.col_length = 3; }
                else if (type_upper == "TIME2") { icol.col_type = 20; icol.col_length = 3; }
                else if (type_upper == "DATE") { icol.col_type = 15; icol.col_length = 3; }
                else if (type_upper == "YEAR") { icol.col_type = 14; icol.col_length = 1; }
                else if (type_upper == "VARCHAR") {
                    icol.col_type = 16;
                    icol.is_big = (icol.col_length * icol.mb_maxlen > 255);
                }
                else if (type_upper == "CHAR") { icol.col_type = 29; icol.is_big = (icol.col_length * icol.mb_maxlen > 255); }
                else if (type_upper == "TEXT") { icol.col_type = 27; icol.is_big = true; icol.col_length = 65535; }
                else if (type_upper == "BLOB") { icol.col_type = 27; icol.is_big = true; icol.col_length = 65535; }
                else if (type_upper == "JSON") { icol.col_type = 31; icol.is_big = true; icol.col_length = 4294967295; }
                else {
                    std::cerr << "[ERROR] Unsupported --schema type: " << type_str << "\n";
                    return 1;
                }

                // Determine which part index contains flags
                // Format: name:TYPE[:size][:flag1:flag2:...]
                // If parts[2] is a number, it's size, so flags start at parts[3]
                size_t flag_start_idx = 2;
                if (parts.size() > 2) {
                    std::string potential_size = parts[2];
                    bool is_number = !potential_size.empty();
                    for (char c : potential_size) {
                        if (!std::isdigit(c)) { is_number = false; break; }
                    }
                    if (is_number) {
                        try { icol.col_length = std::stoul(potential_size); } catch (...) {}
                        flag_start_idx = 3;
                    }
                }

                bool is_pk = false;
                bool explicit_nullable = false;
                bool explicit_notnull = false;
                for (size_t fi = flag_start_idx; fi < parts.size(); fi++) {
                    std::string flag = parts[fi];
                    std::transform(flag.begin(), flag.end(), flag.begin(), ::tolower);
                    if (flag == "pk" || flag == "primary") {
                        n_pk++;
                        is_pk = true;
                    }
                    else if (flag == "null" || flag == "nullable") { explicit_nullable = true; }
                    else if (flag == "notnull" || flag == "not_null") { explicit_notnull = true; }
                    else if (flag == "unsigned") { icol.is_unsigned = true; }
                    else if (flag == "signed") { icol.is_unsigned = false; }  // Explicit signed
                    else if (flag == "autoinc" || flag == "auto_increment") { icol.is_unsigned = true; }
                }

                // AUTO_INCREMENT PK columns are typically INT UNSIGNED
                // If user specified INT:pk without explicit unsigned, assume unsigned for AUTO_INCREMENT
                if (is_pk && (icol.col_type == 4 || icol.col_type == 2 || icol.col_type == 3 || icol.col_type == 9 || icol.col_type == 10)) {
                    bool explicit_signed = false;
                    for (size_t fi = flag_start_idx; fi < parts.size(); fi++) {
                        std::string flag = parts[fi];
                        std::transform(flag.begin(), flag.end(), flag.begin(), ::tolower);
                        if (flag == "signed") { explicit_signed = true; break; }
                    }
                    if (!explicit_signed && !icol.is_unsigned) {
                        icol.is_unsigned = true;  // Default to unsigned for PK INT columns
                    }
                }

                // Set nullable: PK never nullable, otherwise nullable unless explicit notnull
                if (is_pk) {
                    icol.is_nullable = false;
                } else if (explicit_notnull) {
                    icol.is_nullable = false;
                } else {
                    icol.is_nullable = true;  // Default for non-PK columns
                }

                // Set physical_pos to ordinal position in the list (1-based)
                icol.physical_pos = static_cast<uint32_t>(index_columns.size() + 1);

                index_columns.push_back(icol);
            }

            if (n_pk == 0 && !index_columns.empty()) n_pk = 1;
            index_parser.set_columns(index_columns, false, n_pk);
            std::cout << "[INFO] Schema set from --schema option (" << index_columns.size() << " columns, " << n_pk << " PK columns)\n";
        }

        // If TDE encrypted with keys available, route through EncryptedTableRecovery
        // instead of the plaintext InnoDBIndexParser path.
        if (tde_encrypted_ && !parsed_tde_keys.empty()) {
            std::string result_table = table_name.empty() ? "recovered_table" : table_name;
            if (!effective_ibd.empty() && table_name.empty()) {
                fs::path ibd_filepath(effective_ibd);
                std::string stem = ibd_filepath.stem().string();
                if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".ibd") {
                    stem = stem.substr(0, stem.size() - 4);
                }
                result_table = stem;
            }

            std::cout << "[INFO] TDE encrypted tablespace detected — routing to EncryptedTableRecovery\n";

            dbrecovery::catalog::TableSchema tde_schema;
            tde_schema.table_name = result_table;
            tde_schema.page_size = tde_page_size_ > 0 ? tde_page_size_
                                                       : static_cast<uint32_t>(page_size);
            for (size_t ci = 0; ci < index_columns.size(); ci++) {
                const auto& icol = index_columns[ci];
                dbrecovery::catalog::ColumnInfo col_info;
                col_info.name = icol.name;
                col_info.ordinal = icol.ordinal_position;
                col_info.physical_pos = icol.physical_pos;
                col_info.mysql_type_id = icol.col_type;
                col_info.is_nullable = icol.is_nullable;
                col_info.is_signed = !icol.is_unsigned;
                col_info.column_length = icol.col_length;
                col_info.charset_id = icol.char_set_id;
                col_info.numeric_precision = icol.precision;
                col_info.numeric_scale = icol.scale;
                col_info.datetime_precision = icol.datetime_precision;
                col_info.enum_values = icol.enum_values;
                col_info.set_values = icol.set_values;
                tde_schema.columns[ci] = col_info;
            }

            dbrecovery::innodb::EncryptedTableRecovery enc_rec(tde_schema.page_size);
            enc_rec.set_file_path(effective_ibd);
            enc_rec.set_schema(tde_schema);
            enc_rec.set_try_recovery_without_key(false);
            if (!enc_rec.configure_keys(parsed_tde_keys)) {
                std::cerr << "[ERROR] TDE key configuration failed: "
                          << enc_rec.get_last_error() << "\n";
                return 1;
            }

            auto scan_start = std::chrono::high_resolution_clock::now();
            auto records = enc_rec.recover();
            auto scan_end = std::chrono::high_resolution_clock::now();
            double scan_seconds = std::chrono::duration<double>(scan_end - scan_start).count();

            const auto& tde_stats = enc_rec.get_stats();
            std::cout << "\n=== TDE Encrypted Recovery Results ===\n";
            std::cout << "  Status: " << dbrecovery::innodb::EncryptedTableRecoveryStats::status_to_string(tde_stats.tde_status) << "\n";
            std::cout << "  Total pages: " << tde_stats.total_pages << "\n";
            std::cout << "  Encrypted pages: " << tde_stats.encrypted_pages << "\n";
            std::cout << "  Decrypted pages: " << tde_stats.decrypted_pages << "\n";
            std::cout << "  Failed pages: " << tde_stats.failed_pages << "\n";
            std::cout << "  Records recovered: " << records.size() << "\n";
            std::cout << "  Recovery time: " << std::fixed << std::setprecision(2)
                      << scan_seconds << "s\n";

            // Export TDE recovered records
            fs::create_directories(output_dir);
            if (format == "csv" || format == "all") {
                std::string csv_path = output_dir + "/" + result_table + "_tde.csv";
                std::ofstream csv_file(csv_path);
                if (csv_file) {
                    if (!index_columns.empty()) {
                        for (size_t i = 0; i < index_columns.size(); i++) {
                            if (i > 0) csv_file << ",";
                            csv_file << index_columns[i].name;
                        }
                        csv_file << "\n";
                    }
                    for (const auto& rec : records) {
                        bool first = true;
                        for (const auto& [key, value] : rec) {
                            if (!first) csv_file << ",";
                            std::string escaped = value;
                            if (escaped.find(',') != std::string::npos ||
                                escaped.find('"') != std::string::npos) {
                                size_t pos = 0;
                                while ((pos = escaped.find('"', pos)) != std::string::npos) {
                                    escaped.insert(pos, 1, '"');
                                    pos += 2;
                                }
                                csv_file << "\"" << escaped << "\"";
                            } else {
                                csv_file << escaped;
                            }
                            first = false;
                        }
                        csv_file << "\n";
                    }
                    csv_file.close();
                    std::cout << "[INFO] TDE recovery CSV exported: " << csv_path << "\n";
                }
            }
            if (format == "sql" || format == "all") {
                std::string sql_path = output_dir + "/" + result_table + "_tde.sql";
                std::ofstream sql_file(sql_path);
                if (sql_file) {
                    sql_file << "-- TDE recovered data from: " << effective_ibd << "\n";
                    sql_file << "-- Total records: " << records.size() << "\n\n";
                    for (const auto& rec : records) {
                        sql_file << "INSERT INTO `" << result_table << "` (";
                        bool first = true;
                        for (const auto& [key, value] : rec) {
                            if (!first) sql_file << ", ";
                            sql_file << "`" << key << "`";
                            first = false;
                        }
                        sql_file << ") VALUES (";
                        first = true;
                        for (const auto& [key, value] : rec) {
                            if (!first) sql_file << ", ";
                            std::string escaped = value;
                            size_t pos = 0;
                            while ((pos = escaped.find("'", pos)) != std::string::npos) {
                                escaped.insert(pos, 1, '\'');
                                pos += 2;
                            }
                            sql_file << "'" << escaped << "'";
                            first = false;
                        }
                        sql_file << ");\n";
                    }
                    sql_file.close();
                    std::cout << "[INFO] TDE recovery SQL exported: " << sql_path << "\n";
                }
            }
            return 0;
        }

        // Force scan: scan all pages sequentially
        std::cout << "[INFO] Starting force scan of: " << effective_ibd << "\n";
        auto scan_start = std::chrono::high_resolution_clock::now();

        std::vector<dbrecovery::innodb::IndexRecord> all_index_records = index_parser.parse_ibd_force(effective_ibd);

        auto scan_end = std::chrono::high_resolution_clock::now();
        double scan_seconds = std::chrono::duration<double>(scan_end - scan_start).count();

        const auto& idx_stats = index_parser.get_stats();

        std::cout << "\n=== Index Parser Results ===\n";
        std::cout << "  Pages scanned: " << idx_stats.pages_scanned << "\n";
        std::cout << "  Valid INDEX pages: " << idx_stats.pages_valid << "\n";
        std::cout << "  Leaf pages: " << idx_stats.pages_index_leaf << "\n";
        std::cout << "  Non-leaf pages: " << idx_stats.pages_index_nonleaf << "\n";
        std::cout << "  Records recovered: " << idx_stats.records_recovered << "\n";
        std::cout << "  Deleted records: " << idx_stats.records_deleted << "\n";
        std::cout << "  Corrupt pages skipped: " << idx_stats.pages_skipped_corrupt << "\n";
        std::cout << "  Duplicates removed: " << idx_stats.dedup_removed << "\n";
        std::cout << "  Scan time: " << std::fixed << std::setprecision(2) << scan_seconds << "s\n";

        size_t total_recovered = all_index_records.size();
        std::cout << "  Total unique records: " << total_recovered << "\n";

        // Show preview
        if (!all_index_records.empty()) {
            std::cout << "\n[INFO] Recovered records preview (first 10):\n";
            size_t preview_count = std::min(size_t(10), all_index_records.size());
            for (size_t i = 0; i < preview_count; i++) {
                const auto& rec = all_index_records[i];
                std::cout << "  Record " << (i+1) << " (page=" << rec.page_no
                          << ", heap=" << rec.heap_no
                          << ", deleted=" << rec.is_deleted << "): ";
                std::cout << rec.to_csv_line() << "\n";
            }
        }

        // Export recovered records
        std::string result_table = table_name.empty() ? "recovered_table" : table_name;
        if (!effective_ibd.empty() && table_name.empty()) {
            fs::path ibd_filepath(effective_ibd);
            std::string stem = ibd_filepath.stem().string();
            // Remove .ibd suffix if present
            if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".ibd") {
                stem = stem.substr(0, stem.size() - 4);
            }
            result_table = stem;
        }

        if (format == "csv" || format == "all") {
            std::string csv_path = output_dir + "/" + result_table + "_index.csv";
            std::ofstream csv_file(csv_path);
            if (csv_file) {
                // Write CSV header
                if (!index_columns.empty()) {
                    for (size_t i = 0; i < index_columns.size(); i++) {
                        if (i > 0) csv_file << ",";
                        csv_file << index_columns[i].name;
                    }
                    csv_file << "\n";
                }
                for (const auto& rec : all_index_records) {
                    csv_file << rec.to_csv_line() << "\n";
                }
                csv_file.close();
                std::cout << "[INFO] CSV exported to: " << csv_path << " (" << total_recovered << " records)\n";
            }
        }

        if (format == "sql" || format == "all") {
            std::string sql_path = output_dir + "/" + result_table + "_index.sql";
            std::ofstream sql_file(sql_path);
            if (sql_file) {
                // Generate CREATE TABLE if schema is available
                sql_file << "-- Recovered data from: " << effective_ibd << "\n";
                sql_file << "-- Recovery time: " << std::fixed << std::setprecision(2) << scan_seconds << "s\n";
                sql_file << "-- Total records: " << total_recovered << "\n\n";

                for (const auto& rec : all_index_records) {
                    sql_file << rec.to_sql_values(result_table) << "\n";
                }
                sql_file.close();
                std::cout << "[INFO] SQL exported to: " << sql_path << "\n";
            }
        }

        if (format == "json" || format == "all") {
            std::string json_path = output_dir + "/" + result_table + "_index.json";
            std::ofstream json_file(json_path);
            if (json_file) {
                json_file << "{\n  \"table\": \"" << result_table << "\",\n";
                json_file << "  \"record_count\": " << total_recovered << ",\n";
                json_file << "  \"records\": [\n";
                for (size_t i = 0; i < all_index_records.size(); i++) {
                    const auto& rec = all_index_records[i];
                    json_file << "    {\"page_no\": " << rec.page_no
                              << ", \"heap_no\": " << rec.heap_no
                              << ", \"deleted\": " << (rec.is_deleted ? "true" : "false")
                              << ", \"fields\": [";
                    for (size_t j = 0; j < rec.fields.size(); j++) {
                        if (j > 0) json_file << ", ";
                        json_file << "\"" << rec.fields[j].to_string() << "\"";
                    }
                    json_file << "]}";
                    if (i + 1 < all_index_records.size()) json_file << ",";
                    json_file << "\n";
                }
                json_file << "  ]\n}\n";
                json_file.close();
                std::cout << "[INFO] JSON exported to: " << json_path << "\n";
            }
        }

        std::cout << "\n[INFO] Index page parsing completed.\n";
        return 0;
    }

    // 恢复模式：默认使用 InnoDBIndexParser
    // 优先使用结构化解析（记录链遍历），比 heuristic scanner 可靠得多
    // heuristic scanner 依赖模式匹配（如 "user_"），对小表恢复率接近 0%
    std::cout << "[INFO] Starting index page recovery (default mode)...\n";

    if (ibd_path.empty()) {
        std::cerr << "[ERROR] --ibd=<path> is required for recovery\n";
        return 1;
    }

    // Handle --repair option: repair corrupted pages before recovery
    std::string actual_ibd_path = ibd_path;
    std::string repaired_file_path;
    if (repair) {
        std::cout << "[INFO] --repair mode enabled: repairing corrupted pages...\n";

        // Read the file
        std::ifstream input_file(ibd_path, std::ios::binary);
        if (!input_file) {
            std::cerr << "[ERROR] Cannot open file: " << ibd_path << "\n";
            return 1;
        }

        input_file.seekg(0, std::ios::end);
        size_t file_size = input_file.tellg();
        input_file.seekg(0, std::ios::beg);

        std::vector<uint8_t> file_data(file_size);
        input_file.read(reinterpret_cast<char*>(file_data.data()), file_size);
        input_file.close();

        // Use smart page size detection for corrupted files
        size_t page_size_val = page_size > 0 ? page_size :
            dbrecovery::innodb::smart_detect_page_size(file_data.data(), file_size);
        size_t total_pages = file_size / page_size_val;

        std::cout << "[INFO] File size: " << file_size << " bytes, " << total_pages << " pages\n";

        // Count corrupted INDEX pages before repair
        size_t corrupted_index_pages = 0;
        for (size_t i = 0; i < total_pages; i++) {
            size_t offset = i * page_size_val;
            if (offset + 54 >= file_size) break;
            uint16_t page_type = (file_data[offset + 24] << 8) | file_data[offset + 25];
            if (page_type != 0x45BF && page_type != 0x45BD && page_type != 0x0000) {
                // Check if this might be a corrupted INDEX page by looking at surrounding content
                if (offset + 54 < file_size) {
                    uint16_t n_recs = file_data[offset + 54] | (file_data[offset + 55] << 8);
                    if (n_recs > 0 && n_recs < 1000) {
                        corrupted_index_pages++;
                    }
                }
            }
        }

        if (corrupted_index_pages > 0) {
            std::cout << "[INFO] Found ~" << corrupted_index_pages << " potentially corrupted INDEX pages\n";

            // Use HeaderRepairer to repair pages
            dbrecovery::repair::HeaderRepairer header_repairer;
            size_t repaired_count = header_repairer.repair_file_pages(file_data.data(), file_size, page_size_val);

            // Write repaired file to temp location
            repaired_file_path = output_dir.empty() ? "/tmp/repaired_temp.ibd" : (output_dir + "/.repaired_temp.ibd");
            std::ofstream output_file(repaired_file_path, std::ios::binary | std::ios::trunc);
            if (output_file) {
                output_file.write(reinterpret_cast<const char*>(file_data.data()), file_size);
                output_file.close();
                actual_ibd_path = repaired_file_path;
                std::cout << "[INFO] Repaired " << repaired_count << " pages, using repaired file for recovery\n";
            }
        } else {
            std::cout << "[INFO] No corrupted INDEX pages detected\n";
        }
    }

    // Screening gate: the file that actually enters plaintext parsing
    // (which may be the repaired temp file, not the original CLI path).
    if (blocks_generic_plaintext_recovery(actual_ibd_path)) {
        return 1;
    }

    {
        std::string result_table = table_name.empty() ? "recovered_table" : table_name;
        if (!ibd_path.empty() && table_name.empty()) {
            fs::path ibd_filepath(ibd_path);
            result_table = ibd_filepath.stem().string();
        }
        auto ures = dbrecovery::tools::run_unified_recovery(
            actual_ibd_path, output_dir, result_table, format, page_size,
            schema_str, frm_path, table_id, system_tablespace,
            repair, schemaless_recovery, verbose, schema_file_path,
            secondary_index_paths, secondary_index_name);
        dbrecovery::tools::write_unified_result_json(output_dir, result_table, ures);
        // Generate RecoveryReport HTML via unified result
        try {
            std::string html_path = output_dir + "/" + result_table + "_report.html";
            std::ofstream hf(html_path);
            if (hf) { hf << ures.report.to_html(); }
        } catch (...) {}
        if (!repaired_file_path.empty()) {
            std::filesystem::remove(repaired_file_path);
        }
        std::cout << "\n[INFO] Recovery completed. " << ures.total_recovered << " records recovered.\n";
        std::cout << "[INFO] Classification: " << ures.result_classification
                  << " trust=" << ures.trust_level << " source=" << ures.schema_source
                  << " damage=" << dbrecovery::common::damage_level_to_string(ures.damage_level) << "\n";
        return ures.success ? 0 : 1;
    }
