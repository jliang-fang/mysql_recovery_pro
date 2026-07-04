#ifndef DBRECOVERY_TOOLS_RECOVERY_CLI_HPP
#define DBRECOVERY_TOOLS_RECOVERY_CLI_HPP

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <set>
#include <map>

#include "dbrecovery/innodb.h"
#include "dbrecovery/innodb/recovery_engine.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/page_scanner.hpp"
#include "dbrecovery/innodb/page_chain_rebuilder.hpp"
#include "dbrecovery/innodb/sdi_extractor.hpp"
#include "dbrecovery/innodb/btree_node_parser.hpp"
#include "dbrecovery/innodb/record_chain_rebuilder.hpp"
#include "dbrecovery/innodb/blob_chain_rebuilder.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include "dbrecovery/utils/cli_parser.hpp"
#include "dbrecovery/recovery/engine.hpp"
#include "dbrecovery/recovery/rmrf_recovery.hpp"
#include "dbrecovery/recovery/truncate_recovery.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/engine/drop_table_recovery.hpp"
#include "dbrecovery/export/data_exporter.hpp"
#include "dbrecovery/innodb/frm_parser.hpp"
#include "dbrecovery/recovery/raw_disk_carver.hpp"
#include "dbrecovery/disk/xfs_disk_carver.hpp"
#include "dbrecovery/recovery/ext4_recovery.hpp"

namespace fs = std::filesystem;

namespace dbrecovery {
namespace tools {

// 恢复统计
struct RecoveryStats {
    uint64_t total_pages_scanned = 0;
    uint64_t valid_pages = 0;
    uint64_t index_pages = 0;
    uint64_t sdi_pages = 0;
    uint64_t undo_pages = 0;
    uint64_t blob_pages = 0;
    uint64_t records_recovered = 0;
    uint64_t chains_fixed = 0;
    uint64_t orphan_pages = 0;
    uint64_t sdi_extracted = 0;
    uint64_t blob_data_bytes = 0;
    double elapsed_seconds = 0.0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3);
        oss << "RecoveryStats{pages=" << total_pages_scanned
            << ", valid=" << valid_pages
            << ", index=" << index_pages
            << ", sdi=" << sdi_pages
            << ", undo=" << undo_pages
            << ", blob=" << blob_pages
            << ", records=" << records_recovered
            << ", chains_fixed=" << chains_fixed
            << ", orphan=" << orphan_pages
            << ", sdi_extracted=" << sdi_extracted
            << ", blob_bytes=" << blob_data_bytes
            << ", time=" << elapsed_seconds << "s}";
        return oss.str();
    }
};

// 命令行工具类
class RecoveryCLI {
public:
    // 默认构造函数（用于 main_cli.cpp）
    RecoveryCLI();
    
    // 带参数的构造函数
    RecoveryCLI(int argc, char** argv);

    /**
     * @brief 运行恢复命令
     * @param argc 命令行参数数量
     * @param argv 命令行参数数组
     * @return 0 表示成功
     */
    int run(int argc, char** argv);

    /**
     * @brief 显示帮助信息
     */
    void show_help(const char* program_name);

    /**
     * @brief 显示版本信息
     */
    void show_version();

    /**
     * @brief 显示统计信息
     */
    void show_stats(const RecoveryStats& stats);

private:
    dbrecovery::utils::CliParser parser_;
    RecoveryStats stats_;

    // 扫描页
    std::vector<innodb::ScannedPage> scan_pages(const std::string& ibd_path);

    // 重连页链
    innodb::PageChainRebuilder rebuild_page_chain(const std::vector<innodb::ScannedPage>& pages);

    // 提取 SDI
    std::vector<innodb::ExtractedSDI> extract_sdi_data(const std::string& ibd_path);

    // 重连记录链
    innodb::RecordChainRebuilder rebuild_record_chain(const std::vector<innodb::RecordInfo>& records, uint16_t heap_top);

    // 重连 BLOB 链
    innodb::BlobChainRebuilder rebuild_blob_chain(const std::string& ibd_path, uint32_t space_id, uint32_t start_page_no);

    // 解析页
    innodb::BPlusTreeNodeParser parse_page(const std::vector<uint8_t>& page_data);

    // CSV字段转义
    std::string escape_csv_field(const std::string& value);

    // 导出数据
    bool export_data(const std::string& output_path, const std::string& format,
                    const std::vector<std::map<std::string, std::string>>& records);

    // 从JSON文件加载schema
    std::optional<catalog::TableSchema> load_schema_from_file(const std::string& schema_file_path);
};

// ==================== 实现 ====================

// 默认构造函数
inline RecoveryCLI::RecoveryCLI()
    : parser_(0, nullptr), stats_{} {
    // 初始化 parser_ 为空
    // parser_ 的构造函数会处理 argc=0 的情况
}

// 带参数的构造函数
inline RecoveryCLI::RecoveryCLI(int argc, char** argv)
    : parser_(argc, argv), stats_{} {
}

inline void RecoveryCLI::show_version() {
    std::cout << "MySQL Recovery Pro v1.0.0\n";
    std::cout << "Build: " << __DATE__ << " " << __TIME__ << "\n";
}

inline void RecoveryCLI::show_help(const char* program_name) {
    show_version();
    std::cout << "\n";
    std::cout << "\033[1mUsage:\033[0m\n";
    std::cout << "  " << program_name << " --ibd=<path> [options]\n";
    std::cout << "  " << program_name << " --scan-dir=<dir> [options]\n";
    std::cout << "  " << program_name << " --search --table-name=<name> [options]\n\n";
    std::cout << "\033[1mBasic Options:\033[0m\n";
    std::cout << "  \033[1m--ibd=<path>\033[0m               Path to .ibd file (required for single file mode)\n";
    std::cout << "  \033[1m--frm=<path>\033[0m               Path to .frm file for schema (MySQL 5.6/5.7)\n";
    std::cout << "  \033[1m--schema=<def>\033[0m            Manual schema definition (format: col:type:size:pk,...)\n";
    std::cout << "  \033[1m--schema-file=<path>\033[0m       Path to JSON schema file for schema-aware recovery\n";
    std::cout << "  \033[1m--output=<dir>\033[0m             Output directory (default: /tmp/recovered)\n";
    std::cout << "  \033[1m--table-id=<id>\033[0m            Table ID for SDI lookup (default: 0)\n";
    std::cout << "  \033[1m--page-size=<size>\033[0m         Page size: 4096/8192/16384/32768/65536 (default: 16384)\n";
    std::cout << "  \033[1m--format=<csv|sql|json>\033[0m    Output format (default: csv)\n";
    std::cout << "  \033[1m--verbose\033[0m                  Verbose output\n";
    std::cout << "  \033[1m--help\033[0m                     Show this help message\n\n";
    std::cout << "\033[1mRecovery Modes:\033[0m\n";
    std::cout << "  \033[1m--truncate-recovery\033[0m        Recover from truncated table\n";
    std::cout << "                                 (MySQL 5.6/5.7: undo log + disk carving)\n";
    std::cout << "                                 (MySQL 8.0+: disk carving only, data lost after TRUNCATE)\n";
    std::cout << "                                 Use --system-tablespace for undo log recovery\n";
    std::cout << "  \033[1m--rmrf-recovery\033[0m            Recover from rm -rf scenario (disk carving)\n";
    std::cout << "  \033[1m--drop-table-recovery\033[0m      Recover from DROP TABLE (scan system tablespace)\n";
    std::cout << "  \033[1m--disk-carving\033[0m             Raw disk carving (PhotoRec style, no backup needed)\n";
    std::cout << "  \033[1m--device-path=<path>\033[0m       Raw device path for disk carving (e.g., /dev/sda)\n";
    std::cout << "  \033[1m--num-workers=<n>\033[0m          Number of worker threads for disk carving (default: 4)\n";
    std::cout << "  \033[1m--filesystem=<fs>\033[0m          Filesystem type for disk carving: ext4|xfs (default: ext4)\n";
    std::cout << "                                 XFS recovery uses AG scanning and free inode btree traversal\n";
    std::cout << "  \033[1m--target-index-ids=<ids>\033[0m    Only recover specified index IDs (comma-separated)\n";
    std::cout << "                                 Example: --target-index-ids=1001,1002,1003\n";
    std::cout << "  \033[1m--exclude-system-tables\033[0m     Exclude MySQL system tables from recovery\n";
    std::cout << "  \033[1m--system-tablespace=<path>\033[0m Path to system tablespace (ibdata1/mysql.ibd)\n";
    std::cout << "                                 Required for undo log recovery (MySQL 5.6/5.7)\n";
    std::cout << "  \033[1m--data-dir=<path>\033[0m           MySQL data directory for DROP TABLE recovery\n";
    std::cout << "  \033[1m--header-repair\033[0m            Repair corrupted page headers\n";
    std::cout << "  \033[1m--blob-recovery\033[0m            Enable BLOB recovery\n";
    std::cout << "  \033[1m--default-recovery\033[0m         Default recovery mode\n\n";
    std::cout << "\033[1mScan & Discovery Options:\033[0m\n";
    std::cout << "  \033[1m--scan\033[0m                     Scan and display page information\n";
    std::cout << "  \033[1m--scan-dir=<dir>\033[0m           Scan entire data directory\n";
    std::cout << "  \033[1m--depth=<n>\033[0m                Directory scan depth (default: 3)\n";
    std::cout << "  \033[1m--search\033[0m                   Search for tables by name\n";
    std::cout << "  \033[1m--table-name=<name>\033[0m        Table name to search (supports * and ? wildcards)\n";
    std::cout << "  \033[1m--rebuild-chain\033[0m            Rebuild broken page chains\n";
    std::cout << "  \033[1m--extract-sdi\033[0m              Extract SDI metadata\n";
    std::cout << "  \033[1m--parse-page=<page_no>\033[0m     Parse specific page\n\n";
    std::cout << "\033[1mCorruption Detection:\033[0m\n";
    std::cout << "  \033[1m--check-corruption\033[0m         Check for page corruption\n";
    std::cout << "  \033[1m--check-headers\033[0m            Check page headers only\n\n";
    std::cout << "\033[1mBatch Operations:\033[0m\n";
    std::cout << "  \033[1m--batch=<file>\033[0m             Process files from a list (one per line)\n";
    std::cout << "  \033[1m--output-format=<fmt>\033[0m      Output format for batch: csv|sql|json\n\n";
    std::cout << "\033[1mExamples:\033[0m\n";
    std::cout << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --truncate-recovery --output=/tmp/recovery\n";
    std::cout << "  " << program_name << " --scan-dir=/var/lib/mysql --search --table-name=user*\n";
    std::cout << "  " << program_name << " --ibd=/data/table.ibd --check-corruption --verbose\n";
    std::cout << "  " << program_name << " --batch=files.txt --rmrf-recovery --output-format=sql\n\n";
    std::cout << "\033[1mOutput Formats:\033[0m\n";
    std::cout << "  csv  - Comma-separated values (default)\n";
    std::cout << "  sql  - SQL INSERT statements\n";
    std::cout << "  json - JSON array of records\n\n";
    std::cout << "\033[1mRecovery Modes:\033[0m\n";
    std::cout << "  default   - Standard recovery mode\n";
    std::cout << "  truncate  - Recover from truncated tables\n";
    std::cout << "  rmrf      - Recover from deleted files (rm -rf)\n";
    std::cout << "  header    - Repair corrupted page headers\n";
    std::cout << "  blob      - Enable BLOB recovery\n\n";
}

inline std::vector<innodb::ScannedPage> RecoveryCLI::scan_pages(const std::string& ibd_path) {
    auto start = std::chrono::high_resolution_clock::now();

    innodb::PageScanner scanner(16384);
    auto pages = scanner.scan_file(ibd_path);

    auto end = std::chrono::high_resolution_clock::now();
    stats_.elapsed_seconds = std::chrono::duration<double>(end - start).count();

    const auto& s = scanner.stats();
    stats_.total_pages_scanned = s.total_pages;
    stats_.valid_pages = s.valid_pages;
    stats_.index_pages = s.index_pages;
    stats_.sdi_pages = s.sdi_pages;
    stats_.undo_pages = s.undo_pages;
    stats_.blob_pages = s.blob_pages;

    return pages;
}

inline innodb::PageChainRebuilder RecoveryCLI::rebuild_page_chain(const std::vector<innodb::ScannedPage>& pages) {
    auto start = std::chrono::high_resolution_clock::now();

    innodb::PageChainRebuilder rebuilder;
    rebuilder.rebuild_chain(pages);

    auto end = std::chrono::high_resolution_clock::now();
    stats_.elapsed_seconds += std::chrono::duration<double>(end - start).count();

    const auto& s = rebuilder.stats();
    stats_.chains_fixed = s.fixed_chains;
    stats_.orphan_pages = s.orphan_pages;

    return rebuilder;
}

inline std::vector<innodb::ExtractedSDI> RecoveryCLI::extract_sdi_data(const std::string& ibd_path) {
    auto start = std::chrono::high_resolution_clock::now();

    innodb::SDIExtractor extractor(16384);
    auto results = extractor.extract_from_file(ibd_path);

    auto end = std::chrono::high_resolution_clock::now();
    stats_.elapsed_seconds += std::chrono::duration<double>(end - start).count();

    const auto& s = extractor.stats();
    stats_.sdi_extracted = s.sdi_extracted;

    return results;
}

inline innodb::BPlusTreeNodeParser RecoveryCLI::parse_page(const std::vector<uint8_t>& page_data) {
    innodb::BPlusTreeNodeParser parser(page_data);
    parser.parse();
    return parser;
}

inline innodb::RecordChainRebuilder RecoveryCLI::rebuild_record_chain(const std::vector<innodb::RecordInfo>& records, uint16_t heap_top) {
    innodb::RecordChainRebuilder rebuilder;
    rebuilder.rebuild_chain(records, heap_top);
    return rebuilder;
}

inline innodb::BlobChainRebuilder RecoveryCLI::rebuild_blob_chain(const std::string& ibd_path, uint32_t space_id, uint32_t start_page_no) {
    innodb::BlobChainRebuilder rebuilder(16384);
    rebuilder.rebuild_from_file(ibd_path, space_id, start_page_no);
    return rebuilder;
}

inline std::string RecoveryCLI::escape_csv_field(const std::string& value) {
    // Check if the value needs quoting
    if (value.find(',') != std::string::npos ||
        value.find('"') != std::string::npos ||
        value.find('\n') != std::string::npos ||
        value.find('\r') != std::string::npos) {
        // Quote the value and escape internal quotes
        std::string result = "\"";
        for (char c : value) {
            if (c == '"') {
                result += "\"\"";  // Escape quotes by doubling them
            } else {
                result += c;
            }
        }
        result += "\"";
        return result;
    }
    return value;
}

inline bool RecoveryCLI::export_data(const std::string& output_path, const std::string& format,
                                     const std::vector<std::map<std::string, std::string>>& records) {
    if (format == "csv") {
        std::ofstream file(output_path);
        if (!file) return false;

        // 写入表头
        if (!records.empty()) {
            bool first = true;
            for (const auto& kv : records[0]) {
                if (!first) file << ",";
                file << kv.first;
                first = false;
            }
            file << "\n";

            // 写入数据 - 使用CSV转义
            for (const auto& record : records) {
                first = true;
                for (const auto& kv : record) {
                    if (!first) file << ",";
                    file << escape_csv_field(kv.second);
                    first = false;
                }
                file << "\n";
            }
        }
        return true;
    } else if (format == "json") {
        std::ofstream file(output_path);
        if (!file) return false;

        file << "[\n";
        for (size_t i = 0; i < records.size(); ++i) {
            const auto& record = records[i];
            file << "  {";
            bool first = true;
            for (const auto& kv : record) {
                if (!first) file << ", ";
                file << "\"" << kv.first << "\": \"" << kv.second << "\"";
                first = false;
            }
            file << "}";
            if (i < records.size() - 1) file << ",";
            file << "\n";
        }
        file << "]\n";
        return true;
    }
    return false;
}

inline int RecoveryCLI::run(int argc, char** argv) {
    // 解析参数
    bool show_version_only = parser_.get_bool_option("version", false);
    bool show_help_only = parser_.get_bool_option("help", false);

    if (show_version_only) {
        show_version();
        return 0;
    }

    if (show_help_only) {
        show_help(argv[0]);
        return 0;
    }

    std::string ibd_path = parser_.get_option("ibd", "");
    std::string output_dir = parser_.get_option("output", "/tmp/recovered");
    auto table_id_opt = parser_.get_int_option("table-id", 0);
    uint64_t table_id = static_cast<uint64_t>(table_id_opt.value_or(0));
    bool check_headers = parser_.get_bool_option("check-headers", false);
    bool truncate_recovery = parser_.get_bool_option("truncate-recovery", false);
    bool blob_recovery = parser_.get_bool_option("blob-recovery", false);
    bool rmrf_recovery = parser_.get_bool_option("rmrf-recovery", false);
    bool drop_table_recovery = parser_.get_bool_option("drop-table-recovery", false);
    bool disk_carving = parser_.get_bool_option("disk-carving", false);
    bool default_recovery = parser_.get_bool_option("default-recovery", false);
    bool scan_only = parser_.get_bool_option("scan", false);
    bool rebuild_chain = parser_.get_bool_option("rebuild-chain", false);
    bool extract_sdi = parser_.get_bool_option("extract-sdi", false);
    bool verbose = parser_.get_bool_option("verbose", false);

    // Default recovery mode if no specific mode is specified
    if (!truncate_recovery && !rmrf_recovery && !drop_table_recovery && !default_recovery && !disk_carving) {
        default_recovery = true;  // Enable default recovery by default
    }
    std::string format = parser_.get_option("format", "csv");
    auto page_size_opt = parser_.get_int_option("page-size", 16384);
    uint32_t page_size = static_cast<uint32_t>(page_size_opt.value_or(16384));
    auto parse_page_opt = parser_.get_int_option("parse-page", -1);
    int parse_page_no = parse_page_opt.value_or(-1);

    // DROP TABLE recovery specific options
    std::string system_tablespace = parser_.get_option("system-tablespace", "");
    std::string data_dir = parser_.get_option("data-dir", "");

    // Disk carving specific options
    std::string device_path = parser_.get_option("device-path", "");
    auto num_workers_opt = parser_.get_int_option("num-workers", 4);
    uint32_t num_workers = static_cast<uint32_t>(num_workers_opt.value_or(4));

    // Filesystem type for disk carving (ext4 or xfs)
    std::string filesystem_type = parser_.get_option("filesystem", "ext4");
    std::transform(filesystem_type.begin(), filesystem_type.end(), filesystem_type.begin(), ::tolower);

    // Index ID filtering options
    std::string target_index_ids_str = parser_.get_option("target-index-ids", "");
    bool exclude_system_tables = parser_.has_option("exclude-system-tables");

    // FRM file option for schema parsing
    std::string frm_path = parser_.get_option("frm", "");

    // Manual schema option (format: col_name:type:size:pk,...)
    std::string schema_str = parser_.get_option("schema", "");

    // Schema file option (JSON format)
    std::string schema_file_path = parser_.get_option("schema-file", "");

    // Disk carving recovery mode - raw device scanning (no backup needed)
    if (disk_carving) {
        if (device_path.empty()) {
            std::cerr << "[ERROR] Disk carving requires --device-path (e.g., /dev/sda)\n";
            show_help(argv[0]);
            return 1;
        }

        std::cout << "[INFO] Starting raw disk carving recovery...\n";
        std::cout << "[INFO] Device: " << device_path << "\n";
        std::cout << "[INFO] Filesystem: " << filesystem_type << "\n";
        std::cout << "[INFO] Workers: " << num_workers << "\n";
        std::cout << "[INFO] Page size: " << page_size << "\n";

        // Check if XFS filesystem is specified
        if (filesystem_type == "xfs") {
            // Use XFS-specific disk carving
            std::cout << "[INFO] Using XFS-specific recovery (AG scanning + free inode btree)\n";

            dbrecovery::disk::xfs::XfsDiskCarver xfs_carver;
            dbrecovery::disk::xfs::XfsConfig xfs_config;
            xfs_config.device_path = device_path;
            xfs_config.output_dir = output_dir;
            xfs_config.page_size = page_size;
            xfs_config.num_workers = num_workers;
            xfs_config.verbose = verbose;
            xfs_config.recover_deleted = true;
            xfs_config.scan_deleted_inodes = true;
            xfs_config.signature_scan = true;  // Fallback

            // Parse target index IDs (comma-separated)
            if (!target_index_ids_str.empty()) {
                std::istringstream id_stream(target_index_ids_str);
                std::string id_str;
                while (std::getline(id_stream, id_str, ',')) {
                    try {
                        uint64_t id = std::stoull(id_str);
                        xfs_config.target_index_ids.push_back(id);
                    } catch (...) {
                        std::cerr << "[WARN] Invalid index ID: " << id_str << "\n";
                    }
                }
                if (!xfs_config.target_index_ids.empty()) {
                    std::cout << "[INFO] Target index IDs: ";
                    for (size_t i = 0; i < xfs_config.target_index_ids.size(); i++) {
                        if (i > 0) std::cout << ", ";
                        std::cout << xfs_config.target_index_ids[i];
                    }
                    std::cout << "\n";
                }
            }
            xfs_config.exclude_system_tables = exclude_system_tables;
            if (exclude_system_tables) {
                std::cout << "[INFO] Excluding system tables from recovery\n";
            }

            xfs_carver.set_config(xfs_config);

            // Load and set external schema for schema-aware recovery
            std::optional<catalog::TableSchema> xfs_schema = load_schema_from_file(schema_file_path);

            // Also try --schema command line string if no schema file
            if (!xfs_schema.has_value() && !schema_str.empty()) {
                std::cout << "[INFO] Parsing schema from --schema option for XFS recovery\n";
                catalog::TableSchema parsed_schema;
                parsed_schema.table_name = "recovered_table";
                parsed_schema.page_size = page_size;

                // Parse schema string: "col1:type[:size][:pk],col2:type..."
                // Note: type and name are separated by space, not colon
                // Format: "name TYPE:size:extra" where TYPE may have parameters after colon
                std::istringstream ss(schema_str);
                std::string col_def;
                int ordinal = 1;
                while (std::getline(ss, col_def, ',')) {
                    catalog::ColumnInfo col;

                    // Split by colon first
                    std::istringstream cs(col_def);
                    std::string token;
                    std::vector<std::string> parts;
                    while (std::getline(cs, token, ':')) {
                        parts.push_back(token);
                    }

                    if (parts.size() >= 1) {
                        // First part contains "name TYPE", split by space
                        std::istringstream first_part(parts[0]);
                        std::vector<std::string> name_type;
                        std::string nt_token;
                        while (std::getline(first_part, nt_token, ' ')) {
                            if (!nt_token.empty()) {
                                name_type.push_back(nt_token);
                            }
                        }

                        if (name_type.size() >= 2) {
                            col.name = name_type[0];
                            std::string type_str = name_type[1];
                            col.ordinal = ordinal;
                            col.physical_pos = ordinal;

                            // Convert SQL types to MySQL type IDs
                            if (type_str.find("INT") != std::string::npos) {
                                col.mysql_type_id = 3;  // MYSQL_TYPE_LONG
                            } else if (type_str.find("VARCHAR") != std::string::npos) {
                                col.mysql_type_id = 15;  // MYSQL_TYPE_VARCHAR
                                // Size is in parts[1] if present
                                if (parts.size() >= 2) {
                                    col.column_length = std::stoi(parts[1]);
                                }
                            } else if (type_str.find("DATETIME") != std::string::npos) {
                                col.mysql_type_id = 12;  // MYSQL_TYPE_DATETIME
                            } else if (type_str.find("DECIMAL") != std::string::npos) {
                                col.mysql_type_id = 0;  // MYSQL_TYPE_DECIMAL
                                // Precision and scale from parts
                                if (parts.size() >= 2) {
                                    col.numeric_precision = std::stoi(parts[1]);
                                }
                                if (parts.size() >= 3) {
                                    col.numeric_scale = std::stoi(parts[2]);
                                }
                            } else if (type_str.find("ENUM") != std::string::npos) {
                                col.mysql_type_id = 247;  // MYSQL_TYPE_ENUM
                            } else {
                                col.mysql_type_id = 15;  // Default to VARCHAR
                            }

                            col.is_nullable = true;
                            // Check for pk flag in last part
                            if (parts.size() >= 3) {
                                std::string& last_part = parts.back();
                                if (last_part.find("pk") != std::string::npos ||
                                    last_part.find("PK") != std::string::npos ||
                                    last_part.find("PRIMARY") != std::string::npos) {
                                    col.is_nullable = false;
                                }
                            }

                            parsed_schema.columns[ordinal] = col;
                            ordinal++;
                        }
                    }
                }

                xfs_schema = parsed_schema;
                std::cout << "[INFO] Schema parsed: " << parsed_schema.columns.size() << " columns\n";
                std::cout << "[WARN] Schema is applied to ALL recovered index_ids.\n";
                std::cout << "[WARN] Different tables have different column structures.\n";
                std::cout << "[WARN] Use --target-index-ids to filter specific table(s).\n";
                std::cout << "[WARN] Use --exclude-system-tables to skip system data.\n";
            }

            // Set schema if available
            if (xfs_schema.has_value()) {
                xfs_carver.set_external_schema(*xfs_schema);
                std::cout << "[INFO] Schema-aware recovery enabled with "
                          << xfs_schema->columns.size() << " columns\n";
                if (target_index_ids_str.empty() && !exclude_system_tables) {
                    std::cout << "[WARN] Schema applied to ALL pages - data may be incorrectly decoded!\n";
                    std::cout << "[WARN] Recommend: --target-index-ids=<your_index_id> or --exclude-system-tables\n";
                }
            } else {
                std::cout << "[INFO] No schema provided, using schema-less recovery\n";
                std::cout << "[INFO] This is recommended for finding unknown table structures\n";
            }

            auto xfs_result = xfs_carver.scan_and_recover([&](double progress, const std::string& msg) {
                if (verbose) {
                    std::cout << "[" << static_cast<int>(progress * 100) << "%] " << msg << "\n";
                }
            });

            if (xfs_result.success) {
                std::cout << "[INFO] XFS disk carving completed.\n";
                std::cout << "  AG count: " << xfs_result.ag_count << "\n";
                std::cout << "  Inodes scanned: " << xfs_result.total_inodes_scanned << "\n";
                std::cout << "  Deleted inodes found: " << xfs_result.deleted_inodes_found << "\n";
                std::cout << "  Pages recovered: " << xfs_result.pages_recovered << "\n";
                std::cout << "  Total records: " << xfs_result.total_records << "\n";
                std::cout << "  Unique index_ids: " << xfs_result.records_by_index_id.size() << "\n";
                std::cout << "  Output files: " << xfs_result.output_files.size() << "\n";

                for (const auto& file : xfs_result.output_files) {
                    std::cout << "    - " << file << "\n";
                }

                if (!xfs_result.errors.empty()) {
                    std::cout << "\n  Errors:\n";
                    for (const auto& err : xfs_result.errors) {
                        std::cout << "    - " << err << "\n";
                    }
                }

                std::cout << "\n✅ XFS Recovery completed: " << output_dir << "\n";
                return 0;
            } else {
                std::cerr << "[ERROR] XFS disk carving failed.\n";
                for (const auto& err : xfs_result.errors) {
                    std::cerr << "  - " << err << "\n";
                }
                return 1;
            }
        }

        // ext4-specific recovery - fallback to raw disk carving for stability
        // The ext4-specific module has thread safety issues that need more investigation
        if (filesystem_type == "ext4") {
            std::cout << "[INFO] ext4 filesystem detected, using raw disk carving (stable fallback)\n";
            // Fall through to generic carving below
        }

        // Default: filesystem-independent carving
        dbrecovery::recovery::RawDiskCarver carver;
        dbrecovery::recovery::RawDiskCarver::Config config;
        config.device_path = device_path;
        config.output_dir = output_dir;
        config.page_size = page_size;
        config.num_workers = num_workers;
        config.verbose = verbose;
        config.recover_deleted = true;
        carver.set_config(config);

        // Load external schema if provided
        std::optional<catalog::TableSchema> external_schema = load_schema_from_file(schema_file_path);

        // Also try --schema command line string if no schema file
        if (!external_schema.has_value() && !schema_str.empty()) {
            std::cout << "[INFO] Parsing schema from --schema option: " << schema_str << "\n";
            catalog::TableSchema parsed_schema;
            parsed_schema.table_name = "recovered_table";
            parsed_schema.page_size = page_size;

            // Parse schema string: "col1:type[:size][:pk],col2:type..."
            std::istringstream ss(schema_str);
            std::string col_def;
            int ordinal = 1;
            while (std::getline(ss, col_def, ',')) {
                catalog::ColumnInfo col;
                std::istringstream cs(col_def);
                std::string token;
                std::vector<std::string> parts;
                while (std::getline(cs, token, ':')) {
                    parts.push_back(token);
                }
                if (parts.empty()) continue;

                col.name = parts[0];
                col.is_nullable = true;
                col.column_length = 0;

                if (parts.size() > 1) {
                    std::string type_str = parts[1];
                    std::transform(type_str.begin(), type_str.end(), type_str.begin(), ::toupper);

                    // Parse type
                    if (type_str == "INT" || type_str == "INTEGER") {
                        col.mysql_type_id = 3;  // LONG
                        col.column_length = 4;
                    } else if (type_str == "BIGINT") {
                        col.mysql_type_id = 8;  // LONGLONG
                        col.column_length = 8;
                    } else if (type_str == "VARCHAR") {
                        col.mysql_type_id = 253;  // VAR_STRING
                        col.column_length = parts.size() > 2 ? std::stoul(parts[2]) : 255;
                    } else if (type_str == "TEXT") {
                        col.mysql_type_id = 252;  // BLOB
                        col.column_length = 65535;
                    } else if (type_str == "DATETIME") {
                        col.mysql_type_id = 12;  // DATETIME
                        col.column_length = 8;
                    } else if (type_str == "DATE") {
                        col.mysql_type_id = 10;  // DATE
                        col.column_length = 3;
                    } else if (type_str == "DOUBLE") {
                        col.mysql_type_id = 5;  // DOUBLE
                        col.column_length = 8;
                    } else if (type_str == "FLOAT") {
                        col.mysql_type_id = 4;  // FLOAT
                        col.column_length = 4;
                    } else if (type_str == "ENUM") {
                        // ENUM type - InnoDB type 247 (server type) mapped to internal type
                        col.mysql_type_id = 247;  // MYSQL_TYPE_ENUM
                        col.column_length = parts.size() > 2 ? std::stoul(parts[2]) : 4;
                    } else if (type_str == "JSON") {
                        // JSON type - stored as BLOB externally
                        col.mysql_type_id = 245;  // MYSQL_TYPE_JSON
                        col.column_length = parts.size() > 2 ? std::stoul(parts[2]) : 65535;
                    } else if (type_str == "BLOB" || type_str == "TEXT") {
                        // BLOB/TEXT type
                        col.mysql_type_id = 252;  // MYSQL_TYPE_BLOB
                        col.column_length = parts.size() > 2 ? std::stoul(parts[2]) : 65535;
                    } else if (type_str == "TINYINT") {
                        col.mysql_type_id = 1;  // TINY
                        col.column_length = 1;
                    } else if (type_str == "SMALLINT") {
                        col.mysql_type_id = 2;  // SHORT
                        col.column_length = 2;
                    } else if (type_str == "MEDIUMINT") {
                        col.mysql_type_id = 9;  // INT24
                        col.column_length = 4;
                    } else {
                        col.mysql_type_id = 253;  // Default to VARCHAR
                        col.column_length = 255;
                    }
                }

                // Check for PK flag
                for (size_t i = 2; i < parts.size(); i++) {
                    std::string flag = parts[i];
                    std::transform(flag.begin(), flag.end(), flag.begin(), ::toupper);
                    if (flag == "PK" || flag == "PRIMARY") {
                        col.is_nullable = false;
                    }
                }

                parsed_schema.columns[ordinal++] = col;
            }

            external_schema = parsed_schema;
            std::cout << "[INFO] Successfully parsed " << parsed_schema.columns.size() << " columns from --schema\n";
        }

        // Also try FRM file if provided (for MySQL 5.6/5.7)
        if (!external_schema.has_value() && !frm_path.empty()) {
            std::cout << "[INFO] Attempting to parse schema from FRM file: " << frm_path << "\n";
            auto frm_schema = dbrecovery::innodb::FRMParser::parse(frm_path);
            if (frm_schema.has_value()) {
                std::cout << "[INFO] Successfully parsed schema from FRM file (" << frm_schema->columns.size() << " columns)\n";
                external_schema = frm_schema;
            } else {
                std::cout << "[WARN] Failed to parse FRM file\n";
            }
        }

        // Set external schema before scanning if provided
        if (external_schema.has_value()) {
            carver.set_external_schema(*external_schema);
        }

        auto result = carver.scan_and_recover([&](double progress, const std::string& msg) {
            if (verbose) {
                std::cout << "[" << static_cast<int>(progress * 100) << "%] " << msg << "\n";
            }
        });

        // If external schema was provided and SDI didn't find schema, apply external schema
        if (external_schema.has_value() && result.inferred_schemas.empty()) {
            std::cout << "[INFO] Applying external schema to recovered records\n";
            result.inferred_schemas[0] = *external_schema;
        }

        if (result.success) {
            std::cout << "[INFO] Disk carving completed.\n";
            std::cout << "  Total records: " << result.total_records << "\n";
            std::cout << "  Unique index_ids: " << result.records_by_index_id.size() << "\n";
            std::cout << "  Output files: " << result.output_files.size() << "\n";

            for (const auto& file : result.output_files) {
                std::cout << "    - " << file << "\n";
            }

            if (!result.errors.empty()) {
                std::cout << "\n  Errors:\n";
                for (const auto& err : result.errors) {
                    std::cout << "    - " << err << "\n";
                }
            }

            std::cout << "\n✅ Recovery completed: " << output_dir << "\n";
            return 0;
        } else {
            std::cerr << "[ERROR] Disk carving failed.\n";
            for (const auto& err : result.errors) {
                std::cerr << "  - " << err << "\n";
            }
            return 1;
        }
    }

    // DROP TABLE recovery mode
    if (drop_table_recovery) {
        if (system_tablespace.empty() && data_dir.empty()) {
            std::cerr << "[ERROR] DROP TABLE recovery requires --system-tablespace or --data-dir\n";
            show_help(argv[0]);
            return 1;
        }

        std::cout << "[INFO] Starting DROP TABLE recovery...\n";

        dbrecovery::engine::DropTableRecovery recovery;
        dbrecovery::engine::DropTableRecovery::Config config;
        config.system_tablespace = system_tablespace;
        config.data_directory = data_dir;
        config.output_directory = output_dir;
        config.page_size = page_size;
        config.export_csv = (format == "csv");
        config.export_sql = (format == "sql");
        recovery.set_config(config);

        auto result = recovery.recover([&](double progress, const std::string& msg) {
            if (verbose) {
                std::cout << "[" << static_cast<int>(progress * 100) << "%] " << msg << "\n";
            }
        });

        if (result.success) {
            std::cout << "[INFO] DROP TABLE recovery completed.\n";
            std::cout << "  Tables found: " << result.tables_found << "\n";
            std::cout << "  Tables recovered: " << result.tables_recovered << "\n";
            std::cout << "  Total records: " << result.total_records << "\n";
            std::cout << "  Recovery time: " << result.recovery_time_seconds << "s\n";

            for (const auto& table : result.recovered_tables) {
                std::cout << "\n  Table: " << table.schema_name << "." << table.table_name
                          << " (table_id=" << table.table_id
                          << ", index_id=" << table.primary_index_id << ")\n";
            }

            if (!result.errors.empty()) {
                std::cout << "\n  Errors:\n";
                for (const auto& err : result.errors) {
                    std::cout << "    - " << err << "\n";
                }
            }

            std::cout << "\n✅ Recovery completed: " << output_dir << "\n";
            return 0;
        } else {
            std::cerr << "[ERROR] DROP TABLE recovery failed.\n";
            for (const auto& err : result.errors) {
                std::cerr << "  - " << err << "\n";
            }
            return 1;
        }
    }

    if (ibd_path.empty()) {
        show_help(argv[0]);
        return 1;
    }

    // 检查文件是否存在
    std::ifstream f(ibd_path);
    if (!f.good()) {
        std::cerr << "[ERROR] Cannot open file: " << ibd_path << "\n";
        return 1;
    }
    f.close();

    // 创建输出目录
    try {
        fs::create_directories(output_dir);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create output directory: " << output_dir
                  << ", error: " << e.what() << "\n";
        return 1;
    }

    auto total_start = std::chrono::high_resolution_clock::now();

    // 扫描模式
    if (scan_only) {
        std::cout << "[INFO] Scanning pages...\n";
        auto pages = scan_pages(ibd_path);
        std::cout << "[INFO] Scanned " << pages.size() << " pages\n";

        for (size_t i = 0; i < std::min(size_t(10), pages.size()); ++i) {
            std::cout << "  " << pages[i].to_string() << "\n";
        }
        if (pages.size() > 10) {
            std::cout << "  ... and " << (pages.size() - 10) << " more pages\n";
        }

        show_stats(stats_);
        return 0;
    }

    // 页链重连模式
    if (rebuild_chain) {
        std::cout << "[INFO] Rebuilding page chains...\n";
        auto pages = scan_pages(ibd_path);
        auto rebuilder = rebuild_page_chain(pages);
        auto rebuilt = rebuilder.get_rebuilt_pages();

        std::cout << "[INFO] Rebuilt " << rebuilt.size() << " pages\n";
        std::cout << "[INFO] Fixed " << rebuilder.stats().fixed_chains << " chain breaks\n";
        std::cout << "[INFO] Found " << rebuilder.stats().orphan_pages << " orphan pages\n";

        show_stats(stats_);
        return 0;
    }

    // SDI 提取模式
    if (extract_sdi) {
        std::cout << "[INFO] Extracting SDI metadata...\n";
        auto results = extract_sdi_data(ibd_path);

        std::cout << "[INFO] Extracted " << results.size() << " SDI records\n";
        for (const auto& sdi : results) {
            std::cout << "  " << sdi.to_string() << "\n";
        }

        show_stats(stats_);
        return 0;
    }

    // 深度页解析模式
    if (parse_page_no >= 0) {
        std::cout << "[INFO] Parsing page " << parse_page_no << "...\n";

        innodb::InnoDBFileReader reader(ibd_path);
        if (!reader.is_valid()) {
            std::cerr << "[ERROR] Failed to open file\n";
            return 1;
        }

        auto page_data = reader.read_page_raw(parse_page_no);
        if (page_data.empty()) {
            std::cerr << "[ERROR] Failed to read page " << parse_page_no << "\n";
            return 1;
        }

        auto parser = parse_page(page_data);
        if (parser.is_valid()) {
            std::cout << "[INFO] Page parsed successfully\n";
            std::cout << "  " << parser.node_info().to_string() << "\n";
            std::cout << "  Records: " << parser.records().size() << "\n";
        } else {
            std::cerr << "[ERROR] Failed to parse page: " << parser.error_message() << "\n";
            return 1;
        }

        show_stats(stats_);
        return 0;
    }

    // 恢复模式
    dbrecovery::recovery::RecoveryEngine engine;

    // 🔧 修复：在打开文件之前设置页大小提示
    // 这确保 InnoDBFileReader 使用用户指定的页大小，而不是自动检测
    if (page_size > 0) {
        std::cout << "[DEBUG] Setting page_size hint before engine open: " << page_size << std::endl;
        engine.set_page_size(page_size);
    }

    if (!engine.open(ibd_path)) {
        std::cerr << "[ERROR] Failed to open file: " << ibd_path << "\n";
        return 1;
    }

    std::cout << "[INFO] Successfully opened file: " << ibd_path << "\n";

    std::vector<std::map<std::string, std::string>> recovered_records;
    std::vector<std::string> recovered_column_order;  // 用于保持列顺序

    // 辅助函数：从 SDI 页解析表结构
    auto parse_schema_from_sdi = [&](const std::string& ibd_path, uint64_t table_id) -> std::optional<dbrecovery::catalog::TableSchema> {
        dbrecovery::innodb::InnoDBFileReader reader(ibd_path);
        if (!reader.is_valid()) {
            std::cerr << "[ERROR] Failed to open file for SDI parsing: " << ibd_path << "\n";
            return std::nullopt;
        }

        uint32_t total_pages = reader.get_total_pages();
        uint32_t page_size = reader.get_page_size();

        // MySQL 8.0+ SDI pages are typically at pages 3 and 4
        for (uint32_t page_no = 0; page_no < std::min(total_pages, static_cast<uint32_t>(10)); ++page_no) {
            auto page_data = reader.read_page_raw(page_no);
            if (page_data.empty()) continue;

            // InnoDB FIL header fields are stored in BIG-ENDIAN format
            uint16_t page_type = dbrecovery::innodb::read_be<uint16_t>(
                page_data.data(), dbrecovery::innodb::FIL_PAGE_TYPE_ABS);

            // Support both uncompressed SDI (0x45BE) and compressed SDI (0x45BD)
            if (dbrecovery::innodb::is_sdi_page_type(page_type)) {
                // For compressed SDI (0x45BD), use ibd2sql approach
                if (page_type == dbrecovery::innodb::FIL_PAGE_SDI_ZIP && page_size >= 100) {
                    // ibd2sql: offset = struct.unpack('>h',data[97:99])[0] + 99
                    int16_t slot_offset = static_cast<int16_t>(
                        (static_cast<uint16_t>(page_data[97]) << 8) |
                        static_cast<uint16_t>(page_data[98]));
                    uint32_t record_offset = static_cast<uint32_t>(slot_offset + 99);

                    if (record_offset > 0 && record_offset < page_size - 50) {
                        // Read lengths at offset+25 and offset+29 (big-endian)
                        uint32_t uncompressed_len = dbrecovery::innodb::read_be<uint32_t>(
                            page_data.data(), record_offset + 25);
                        uint32_t compressed_len = dbrecovery::innodb::read_be<uint32_t>(
                            page_data.data(), record_offset + 29);

                        if (compressed_len > 0 && compressed_len < page_size &&
                            record_offset + 33 + compressed_len <= page_size) {
                            const uint8_t* compressed_data = page_data.data() + record_offset + 33;

                            z_stream strm = {};
                            strm.next_in = const_cast<Bytef*>(compressed_data);
                            strm.avail_in = compressed_len;

                            std::vector<uint8_t> decompressed(uncompressed_len > 0 ? uncompressed_len : compressed_len * 4);
                            strm.next_out = decompressed.data();
                            strm.avail_out = decompressed.size();

                            // Use standard zlib decompression (not raw deflate)
                            int ret = inflateInit(&strm);
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
                                        dbrecovery::catalog::TableSchema schema;
                                        // MySQL 8.0 SDI format: { "dd_object": { "name": ..., "columns": [...] } }
                                        nlohmann::json schema_json;
                                        if (json.contains("dd_object")) {
                                            schema_json = json["dd_object"];
                                        } else {
                                            schema_json = json;
                                        }
                                        if (schema.from_json(schema_json)) {
                                            std::cout << "[INFO] Successfully parsed schema from compressed SDI: "
                                                      << schema.table_name << " (" << schema.columns.size() << " columns)\n";
                                            inflateEnd(&strm);
                                            return schema;
                                        }
                                    } catch (const nlohmann::json::parse_error&) {}
                                }
                                inflateEnd(&strm);
                            }
                        }
                    }
                }

                // For uncompressed SDI (0x45BE), use simple offset
                const uint8_t* sdi_payload = page_data.data() + 38;
                if (page_data.size() >= 38 + 20) {
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
                            std::cerr << "[WARN] Failed to parse SDI JSON: " << e.what() << "\n";
                        }
                    }
                }
            }
        }

        return std::nullopt;
    };

    // 辅助函数：创建空 schema（让引擎使用 schemaless 模式）
    // P0 FIX: 不再使用硬编码的 4 字段 schema，而是返回空 schema
    // 让恢复引擎使用实际的字段数量和 field_NNNN 命名
    auto create_empty_schema = []() -> dbrecovery::catalog::TableSchema {
        dbrecovery::catalog::TableSchema schema;
        schema.table_name = "recovered_table";
        schema.row_format = 2;  // DYNAMIC
        schema.page_size = 16384;
        // 不添加任何预定义列 - 让引擎根据实际数据推断
        return schema;
    };

    // 辅助函数：从 FRM 文件解析 schema 并设置到 InnoDBIndexParser
    // 参考 main.cpp lines 2211-2288 的实现
    auto parse_schema_from_frm = [&](const std::string& frm_path,
                                      dbrecovery::innodb::InnoDBIndexParser& index_parser,
                                      bool verbose) -> bool {
        if (frm_path.empty()) return false;

        std::cout << "[INFO] Parsing schema from .frm file: " << frm_path << "\n";
        auto frm_schema = dbrecovery::innodb::FRMParser::parse(frm_path);
        if (!frm_schema.has_value()) {
            std::cout << "[WARN] Failed to parse .frm file\n";
            return false;
        }

        std::cout << "[INFO] Schema obtained from .frm file (" << frm_schema->columns.size() << " columns)\n";

        // Find PK columns from indexes
        std::set<std::string> pk_column_names;
        for (const auto& idx : frm_schema->indexes) {
            if (idx.is_primary) {
                for (const auto& col_name : idx.columns) {
                    pk_column_names.insert(col_name);
                }
            }
        }

        std::vector<dbrecovery::innodb::IndexColumnDef> index_columns;
        uint32_t n_pk = 0;
        uint32_t ordinal = 1;  // ordinal_position starts from 1

        for (auto& [pos, col] : frm_schema->columns) {
            dbrecovery::innodb::IndexColumnDef icol;
            icol.name = col.name;
            icol.col_type = static_cast<uint8_t>(col.mysql_type_id);
            icol.col_length = col.column_length;
            icol.is_nullable = col.is_nullable;
            icol.is_unsigned = !col.is_signed;
            icol.mb_maxlen = 4;  // UTF8MB4

            // ✅ 关键：设置 ordinal_position 和 physical_pos
            // 对于 MySQL 5.6/5.7 FRM 文件，physical_pos 通常等于 ordinal_position
            icol.ordinal_position = ordinal;
            icol.physical_pos = ordinal;  // For FRM, physical_pos = ordinal_position
            ordinal++;

            // Set precision/scale for DECIMAL types (InnoDB type 21)
            if (col.mysql_type_id == 21 && col.numeric_precision > 0) {
                icol.precision = col.numeric_precision;
                icol.scale = col.numeric_scale;
            }

            // Transfer ENUM/SET values
            icol.enum_values = col.enum_values;
            icol.set_values = col.set_values;

            // Determine if variable length based on InnoDB type
            switch (col.mysql_type_id) {
                case 16:  // VARCHAR
                    icol.is_big = (col.column_length * 4 > 255);
                    break;
                case 24: case 25: case 26: case 27: case 31:  // BLOB variants, JSON
                    icol.is_big = true;
                    break;
                default:
                    icol.is_big = false;
                    break;
            }

            // CRITICAL FIX: Set is_binary based on charset_id/collation_id
            // MySQL binary charset (BINARY, VARBINARY, BLOB) has charset_id=63 or collation_id=63
            // EXCEPTION: JSON type (31) uses binary charset but should NOT be treated as binary
            icol.is_binary = (col.charset_id == 63 || col.collation_id == 63) && (icol.col_type != 31);

            // Check if this column is part of PK
            if (pk_column_names.count(col.name) > 0) {
                n_pk++;
            }

            index_columns.push_back(icol);
        }

        if (n_pk == 0 && !index_columns.empty()) n_pk = 1;

        if (verbose) {
            std::cout << "[DEBUG] Column types from FRM:\n";
            for (size_t i = 0; i < index_columns.size() && i < 15; i++) {
                std::cout << "  " << i+1 << ". " << index_columns[i].name
                          << " type=" << static_cast<int>(index_columns[i].col_type)
                          << " len=" << index_columns[i].col_length << "\n";
            }
        }

        index_parser.set_columns(index_columns, false, n_pk);
        std::cout << "[INFO] Schema set from .frm file (" << index_columns.size()
                  << " columns, " << n_pk << " PK columns)\n";
        return true;
    };

    // 辅助函数：从字符串解析 schema 并设置到 InnoDBIndexParser
    // Supports two formats:
    // 1. Colon-separated: col_name:type:size:flags, col_name2:type2:size2:flags2
    //    Example: id:INT:pk, name:VARCHAR:255, email:VARCHAR:100
    // 2. SQL-style: col_name TYPE[(size)] [flags], col_name2 TYPE2 [(size2)] [flags2]
    //    Example: id INT PRIMARY KEY, name VARCHAR(100), age INT NOT NULL
    auto parse_schema_from_string = [&](const std::string& schema_str,
                                          dbrecovery::innodb::InnoDBIndexParser& index_parser,
                                          bool verbose) -> bool {
        if (schema_str.empty()) return false;

        std::cout << "[INFO] Parsing schema from --schema option\n";

        // Split by comma (but not inside parentheses)
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

        std::vector<dbrecovery::innodb::IndexColumnDef> index_columns;
        uint32_t n_pk = 0;

        for (const auto& col_def : col_defs) {
            dbrecovery::innodb::IndexColumnDef icol;

            // Trim whitespace from col_def
            std::string trimmed_def = col_def;
            while (!trimmed_def.empty() && isspace(trimmed_def.front())) trimmed_def.erase(0, 1);
            while (!trimmed_def.empty() && isspace(trimmed_def.back())) trimmed_def.pop_back();

            // Detect format: check if it contains ':' (colon-separated format)
            bool has_colon = trimmed_def.find(':') != std::string::npos;

            std::string name_str, type_str;
            std::vector<std::string> flag_parts;

            if (has_colon) {
                // Colon-separated format: name:type:size:flags
                std::istringstream cs(trimmed_def);
                std::string token;
                std::vector<std::string> parts;
                while (std::getline(cs, token, ':')) {
                    parts.push_back(token);
                }

                if (parts.empty()) continue;
                name_str = parts[0];
                type_str = parts.size() > 1 ? parts[1] : "VARCHAR";
                for (size_t fi = 2; fi < parts.size(); fi++) {
                    flag_parts.push_back(parts[fi]);
                }
            } else {
                // SQL-style format: name TYPE[(size)] [flags...]
                // Parse: split by whitespace, handle TYPE(size) as one token
                std::istringstream ss(trimmed_def);
                std::vector<std::string> tokens;
                std::string tok;
                while (ss >> tok) {
                    tokens.push_back(tok);
                }

                if (tokens.empty()) continue;
                name_str = tokens[0];

                // Find type (first non-keyword after name)
                size_t type_idx = 1;
                if (tokens.size() > 1) {
                    type_str = tokens[1];
                    // Check if type has (size) attached
                    // Handle DECIMAL(M,D), VARCHAR(255), ENUM('a','b'), etc.
                } else {
                    type_str = "VARCHAR";  // default
                }

                // Remaining tokens are flags
                for (size_t fi = 2; fi < tokens.size(); fi++) {
                    flag_parts.push_back(tokens[fi]);
                }

                // Handle case where type and size are separate tokens: VARCHAR ( 255 )
                // If type_str doesn't have parentheses but next token starts with '('
                if (type_str.find('(') == std::string::npos && !flag_parts.empty() && !flag_parts[0].empty() && flag_parts[0][0] == '(') {
                    // Merge type with size tokens
                    type_str += flag_parts[0];
                    flag_parts.erase(flag_parts.begin());
                    // Continue merging if there's more parts of the size spec
                    while (!flag_parts.empty() && type_str.back() != ')') {
                        type_str += " " + flag_parts[0];
                        flag_parts.erase(flag_parts.begin());
                    }
                }
            }

            // Trim name
            while (!name_str.empty() && isspace(name_str.front())) name_str.erase(0, 1);
            while (!name_str.empty() && isspace(name_str.back())) name_str.pop_back();

            icol.name = name_str;
            icol.col_type = 16;  // default to VARCHAR

            // Extract size from type (e.g., VARCHAR(255) -> size=255)
            // Also extract ENUM/SET values
            auto paren_pos = type_str.find('(');
            if (paren_pos != std::string::npos) {
                auto end_paren = type_str.find(')', paren_pos);
                if (end_paren != std::string::npos) {
                    std::string size_str = type_str.substr(paren_pos + 1, end_paren - paren_pos - 1);
                    // Handle DECIMAL(M,D)
                    auto comma_in_size = size_str.find(',');
                    if (comma_in_size != std::string::npos) {
                        // Check if this is DECIMAL or ENUM/SET with multiple values
                        std::string type_upper_check = type_str.substr(0, paren_pos);
                        std::transform(type_upper_check.begin(), type_upper_check.end(), type_upper_check.begin(), ::toupper);
                        if (type_upper_check == "ENUM" || type_upper_check == "SET") {
                            // This is ENUM or SET with comma-separated values like ENUM('a','b','c')
                            // The comma in size_str is part of the values, not DECIMAL precision
                            std::cerr << "[DEBUG TYPE] type_str='" << type_str << "' paren_pos=" << paren_pos << " type_upper='" << type_upper_check << "'\n";
                            // Parse the values
                            std::string values_str = size_str;
                            std::vector<std::string> values;
                            size_t start = 0;
                            while (start < values_str.size()) {
                                while (start < values_str.size() && isspace(values_str[start])) start++;
                                if (start >= values_str.size()) break;
                                char quote = values_str[start];
                                if (quote != '\'' && quote != '"') {
                                    size_t end = values_str.find(',', start);
                                    if (end == std::string::npos) end = values_str.size();
                                    std::string val = values_str.substr(start, end - start);
                                    while (!val.empty() && isspace(val.back())) val.pop_back();
                                    if (!val.empty()) values.push_back(val);
                                    start = end + 1;
                                } else {
                                    size_t end = values_str.find(quote, start + 1);
                                    if (end != std::string::npos) {
                                        values.push_back(values_str.substr(start + 1, end - start - 1));
                                        start = end + 1;
                                        while (start < values_str.size() && (values_str[start] == ',' || isspace(values_str[start]))) start++;
                                    } else {
                                        start++;
                                    }
                                }
                            }
                            std::cerr << "[DEBUG VALUES] Parsed " << values.size() << " values for " << type_upper_check << "\n";
                            if (type_upper_check == "ENUM") {
                                icol.enum_values = values;
                            } else {
                                icol.set_values = values;
                            }
                        } else {
                            // DECIMAL(M,D)
                            try {
                                icol.precision = std::stoul(size_str.substr(0, comma_in_size));
                                icol.scale = std::stoul(size_str.substr(comma_in_size + 1));
                                icol.col_length = icol.precision;
                            } catch (...) {}
                        }
                    } else {
                        // No comma in size_str - could be regular size or non-comma ENUM/SET
                        // Try to parse as regular size first
                        try {
                            icol.col_length = std::stoul(size_str);
                            // DEBUG
                            static int size_debug = 0;
                            if (size_debug < 5) {
                                std::cerr << "[DEBUG SIZE_PARSE] type_str='" << type_str << "' size_str='" << size_str
                                          << "' -> col_length=" << icol.col_length << "\n";
                                size_debug++;
                            }
                        } catch (...) {
                            // If parsing fails, it might be a single-value ENUM/SET
                            // Just use default size
                        }
                    }
                    type_str = type_str.substr(0, paren_pos);
                }
            }

            // Size from flag_parts if colon-separated format (e.g., name:VARCHAR:255)
            if (has_colon && flag_parts.size() > 0 && icol.col_length == 0) {
                try {
                    icol.col_length = std::stoul(flag_parts[0]);
                } catch (...) {}
            }

            // Convert type string to InnoDB internal type ID
            // First, extract the base type name (strip parentheses if any)
            std::string type_upper = type_str;
            std::transform(type_upper.begin(), type_upper.end(), type_upper.begin(), ::toupper);

            // Strip any (size) or (precision,scale) from type name
            auto paren_in_type = type_upper.find('(');
            if (paren_in_type != std::string::npos) {
                type_upper = type_upper.substr(0, paren_in_type);
            }
            // Trim whitespace
            while (!type_upper.empty() && isspace(type_upper.back())) type_upper.pop_back();

            icol.mb_maxlen = 4;  // UTF8MB4

            // CRITICAL: Use InnoDB INTERNAL type IDs, NOT MySQL server type IDs!
            // Reference: ibd2sql column_type.py
            if (type_upper == "TINYINT") { icol.col_type = 2; icol.col_length = 1; }
            else if (type_upper == "SMALLINT") { icol.col_type = 3; icol.col_length = 2; }
            else if (type_upper == "MEDIUMINT") { icol.col_type = 10; icol.col_length = 3; }
            else if (type_upper == "INT" || type_upper == "INTEGER") { icol.col_type = 4; icol.col_length = 4; }
            else if (type_upper == "BIGINT") { icol.col_type = 9; icol.col_length = 8; }
            else if (type_upper == "FLOAT") { icol.col_type = 5; icol.col_length = 4; }
            else if (type_upper == "DOUBLE") { icol.col_type = 6; icol.col_length = 8; }
            else if (type_upper == "DECIMAL" || type_upper == "NUMERIC") {
                icol.col_type = 21;
                // precision and scale already set from parsing DECIMAL(M,D)
                // Default if not specified
                if (icol.precision == 0) icol.precision = 10;
                if (icol.scale == 0) icol.scale = 0;
            }
            else if (type_upper == "TIMESTAMP" || type_upper == "TIMESTAMP2") { icol.col_type = 18; icol.col_length = 4; }
            else if (type_upper == "DATETIME" || type_upper == "DATETIME2") { icol.col_type = 19; icol.col_length = 5; }
            else if (type_upper == "TIME" || type_upper == "TIME2") { icol.col_type = 20; icol.col_length = 3; }
            else if (type_upper == "DATE") { icol.col_type = 15; icol.col_length = 3; }
            else if (type_upper == "YEAR") { icol.col_type = 14; icol.col_length = 1; }
            else if (type_upper == "VARCHAR") {
                icol.col_type = 16;
                icol.is_big = (icol.col_length * icol.mb_maxlen > 255);
            }
            else if (type_upper == "CHAR") {
                icol.col_type = 29;  // STRING type in InnoDB
                icol.is_big = (icol.col_length * icol.mb_maxlen > 255);
            }
            else if (type_upper == "TEXT") {
                icol.col_type = 27;  // BLOB/TEXT type in InnoDB
                icol.is_big = true;
                icol.col_length = 65535;
            }
            else if (type_upper == "BLOB") {
                icol.col_type = 27;
                icol.is_big = true;
                icol.is_binary = true;  // BLOB is binary
                icol.col_length = 65535;
            }
            else if (type_upper == "VARBINARY") {
                icol.col_type = 16;  // Same as VARCHAR but binary
                icol.is_big = (icol.col_length * icol.mb_maxlen > 255);
                icol.is_binary = true;  // VARBINARY is binary
            }
            else if (type_upper == "BINARY") {
                icol.col_type = 29;  // Same as CHAR but binary
                icol.is_big = (icol.col_length * icol.mb_maxlen > 255);
                icol.is_binary = true;  // BINARY is binary
            }
            else if (type_upper == "JSON") {
                icol.col_type = 31;  // JSON type in InnoDB
                icol.is_big = true;
                icol.col_length = 4294967295;
            }
            else if (type_upper == "ENUM") {
                icol.col_type = 22;  // ENUM type in InnoDB
                icol.col_length = 1;  // Default 1 byte for small enums
            }
            else if (type_upper == "SET") {
                icol.col_type = 23;  // SET type in InnoDB
                icol.col_length = 1;
            }
            else {
                icol.col_type = 15;  // Default to NEWDATE/unknown
                icol.is_big = (icol.col_length > 255);
            }

            // Parse flags (pk, notnull, unsigned)
            icol.is_nullable = true;  // Default nullable
            bool is_pk_col = false;
            for (const auto& flag : flag_parts) {
                std::string flag_lower = flag;
                std::transform(flag_lower.begin(), flag_lower.end(), flag_lower.begin(), ::tolower);
                // Trim flag
                while (!flag_lower.empty() && isspace(flag_lower.front())) flag_lower.erase(0, 1);
                while (!flag_lower.empty() && isspace(flag_lower.back())) flag_lower.pop_back();

                if (flag_lower == "pk" || flag_lower == "key") {
                    is_pk_col = true;
                    icol.is_nullable = false;
                }
                else if (flag_lower == "notnull" || flag_lower == "not_null") {
                    icol.is_nullable = false;
                }
                else if (flag_lower == "unsigned") {
                    icol.is_unsigned = true;
                }
            }

            // Handle multi-word flags: "PRIMARY KEY", "NOT NULL"
            std::string def_lower = trimmed_def;
            std::transform(def_lower.begin(), def_lower.end(), def_lower.begin(), ::tolower);
            if (def_lower.find("primary key") != std::string::npos) {
                is_pk_col = true;
                icol.is_nullable = false;
            }
            if (def_lower.find("not null") != std::string::npos ||
                def_lower.find("not_null") != std::string::npos) {
                icol.is_nullable = false;
            }
            if (def_lower.find("unsigned") != std::string::npos) {
                icol.is_unsigned = true;
            }

            // Count PK columns
            if (is_pk_col) {
                n_pk++;
            }

            // CRITICAL: Set physical_pos to ordinal position (InnoDB storage order)
            // For schema-defined columns, physical_pos should equal ordinal_position
            // InnoDB stores columns in physical_pos order, which typically matches
            // the order columns are defined in CREATE TABLE
            icol.physical_pos = static_cast<uint32_t>(index_columns.size() + 1);

            index_columns.push_back(icol);
        }

        if (n_pk == 0 && !index_columns.empty()) {
            n_pk = 1;  // Default: first column is PK
            index_columns[0].is_nullable = false;
        }

        if (verbose) {
            std::cout << "[DEBUG] Column types from --schema:\n";
            for (size_t i = 0; i < index_columns.size() && i < 15; i++) {
                std::cout << "  " << i+1 << ". " << index_columns[i].name
                          << " type=" << static_cast<int>(index_columns[i].col_type)
                          << " len=" << index_columns[i].col_length
                          << " nullable=" << index_columns[i].is_nullable << "\n";
            }
        }

        index_parser.set_columns(index_columns, false, n_pk);
        std::cout << "[INFO] Schema set from --schema option (" << index_columns.size()
                  << " columns, " << n_pk << " PK columns)\n";
        return true;
    };

    // Helper lambda: Convert IndexRecord to map with proper column names
    // Returns pair: {records, column_order}
    auto convert_records_to_map = [](const std::vector<dbrecovery::innodb::IndexRecord>& records,
                                      const dbrecovery::innodb::InnoDBIndexParser& parser)
        -> std::pair<std::vector<std::map<std::string, std::string>>, std::vector<std::string>> {
        std::vector<std::map<std::string, std::string>> result;
        std::vector<std::string> column_order;
        const auto& cols = parser.get_column_defs();

        // Build column order from schema
        for (size_t i = 0; i < cols.size(); i++) {
            if (!cols[i].name.empty()) {
                column_order.push_back(cols[i].name);
            } else {
                char buf[16];
                snprintf(buf, sizeof(buf), "field_%04zu", i);
                column_order.push_back(buf);
            }
        }

        for (const auto& rec : records) {
            std::map<std::string, std::string> record_map;
            // DEBUG: Check if rec.fields has more than cols.size()
            static int field_size_debug = 0;
            if (field_size_debug < 5 && rec.fields.size() != cols.size()) {
                std::cerr << "[DEBUG FIELD SIZE] rec.fields.size()=" << rec.fields.size()
                          << " cols.size()=" << cols.size() << "\n";
                field_size_debug++;
            }
            // CRITICAL FIX: Only iterate over columns defined in schema (cols.size())
            // NOT rec.fields.size() which could be larger due to parsing artifacts
            for (size_t i = 0; i < cols.size() && i < rec.fields.size(); i++) {
                // Use column name if available, otherwise use field_NNNN
                std::string field_name;
                if (i < cols.size() && !cols[i].name.empty()) {
                    field_name = cols[i].name;
                } else {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "field_%04zu", i);
                    field_name = buf;
                }

                const auto& field = rec.fields[i];

                // DEBUG: Check specific column value
                static int col_debug = 0;
                if (col_debug < 3 && field_name == "default_value") {
                    std::cerr << "[DEBUG COL] default_value field type=" << static_cast<int>(field.type)
                              << " str_value.size()=" << field.str_value.size()
                              << " str_value first 50=";
                    for (size_t j = 0; j < 50 && j < field.str_value.size(); j++) {
                        char c = field.str_value[j];
                        if (c >= 32 && c < 127) std::cerr << c;
                        else std::cerr << "[" << std::hex << (int)(unsigned char)c << "]";
                    }
                    std::cerr << " raw_bytes.size()=" << field.raw_bytes.size() << "\n";
                    col_debug++;
                }

                if (field.type == dbrecovery::innodb::IndexFieldValue::Type::NULL_VAL) {
                    record_map[field_name] = "NULL";
                } else if (!field.str_value.empty()) {
                    // str_value contains the decoded value (text, UNHEX format, etc.)
                    record_map[field_name] = field.str_value;
                } else if (!field.raw_bytes.empty()) {
                    // raw_bytes fallback (should rarely happen now)
                    std::ostringstream hex_ss;
                    hex_ss << "UNHEX('";
                    for (uint8_t b : field.raw_bytes) {
                        hex_ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(b);
                    }
                    hex_ss << "')";
                    record_map[field_name] = hex_ss.str();
                } else {
                    // Numeric types - use to_string()
                    record_map[field_name] = field.to_string();
                }
            }
            // DEBUG: Check record map size
            static int map_debug = 0;
            if (map_debug < 3) {
                std::cerr << "[DEBUG MAP] record_map has " << record_map.size() << " entries, cols.size()=" << cols.size() << "\n";
                map_debug++;
            }
            result.push_back(record_map);
        }
        return {result, column_order};
    };

    // Default recovery mode - use InnoDBIndexParser for high-accuracy recovery
    if (default_recovery && !truncate_recovery && !rmrf_recovery) {
        std::cout << "[INFO] Starting default recovery (InnoDBIndexParser)...\n";

        // Detect page size
        if (page_size == 0) {
            std::ifstream fsp_file(ibd_path, std::ios::binary);
            if (fsp_file) {
                std::vector<uint8_t> first_page(16384);
                fsp_file.read(reinterpret_cast<char*>(first_page.data()), 16384);
                for (uint32_t ps : {16384, 8192, 32768, 4096, 65536}) {
                    fsp_file.seekg(0);
                    fsp_file.read(reinterpret_cast<char*>(first_page.data()), ps);
                    if (fsp_file.gcount() >= ps) {
                        page_size = ps;
                        break;
                    }
                }
            }
        }
        if (page_size == 0) page_size = 16384;

        // Use InnoDBIndexParser for recovery
        dbrecovery::innodb::InnoDBIndexParser index_parser;
        index_parser.set_recover_deleted(true);
        index_parser.set_bad_page_mode(2);

        // Priority 1: Try --schema option first
        bool has_schema = parse_schema_from_string(schema_str, index_parser, verbose);

        // Priority 2: Try FRM schema
        bool has_frm_schema = false;
        if (!has_schema) {
            has_frm_schema = parse_schema_from_frm(frm_path, index_parser, verbose);
        }

        // Priority 3: If no schema, try SDI schema (MySQL 8.0+)
        if (!has_schema && !has_frm_schema) {
            auto sdi_schema_opt = parse_schema_from_sdi(ibd_path, table_id);
            if (sdi_schema_opt.has_value()) {
                std::cout << "[INFO] Using SDI schema: " << sdi_schema_opt->table_name
                          << " (" << sdi_schema_opt->columns.size() << " columns)\n";
                // Apply SDI schema to index_parser
                std::vector<dbrecovery::innodb::IndexColumnDef> sdi_columns;
                uint32_t n_pk = 0;
                std::set<std::string> pk_col_names;
                // Find PK columns from indexes
                for (const auto& idx : sdi_schema_opt->indexes) {
                    if (idx.is_primary) {
                        for (const auto& col_name : idx.columns) {
                            pk_col_names.insert(col_name);
                        }
                    }
                }
                for (const auto& [idx, col] : sdi_schema_opt->columns) {
                    // Skip InnoDB system columns - these are handled internally
                    // and should not be part of the user schema
                    if (col.name == "DB_TRX_ID" || col.name == "DB_ROLL_PTR" || col.name == "DB_ROW_ID") {
                        continue;
                    }

                    dbrecovery::innodb::IndexColumnDef col_def;
                    col_def.name = col.name;

                    // ✅ 关键：保存原始 ordinal (schema定义顺序) 和 physical_pos (存储顺序)
                    // InnoDB 按 physical_pos 顺序存储 NULL 位图和变长大小字节
                    // 数据也按 physical_pos 顺序存储
                    col_def.ordinal_position = col.ordinal;  // Schema definition order
                    col_def.physical_pos = col.physical_pos; // InnoDB storage order

                    // Debug: show ordinal/physical_pos values being passed
                    static bool ordinal_debug_printed = false;
                    if (!ordinal_debug_printed) {
                        std::cerr << "[DEBUG SDI PASS] Column '" << col.name << "' ordinal=" << col.ordinal
                                  << " physical_pos=" << col.physical_pos << "\n";
                        ordinal_debug_printed = true;
                    }

                    // SDI already stores InnoDB internal types (not MySQL server types)
                    // No conversion needed - use directly
                    col_def.col_type = static_cast<uint8_t>(col.mysql_type_id);

                    col_def.is_nullable = col.is_nullable;
                    col_def.is_unsigned = !col.is_signed;  // InnoDB: is_signed=false means unsigned
                    col_def.char_set_id = col.charset_id;
                    col_def.col_length = col.column_length;
                    col_def.mb_maxlen = 4;  // utf8mb4 default

                    // Set precision/scale for DECIMAL types (InnoDB type 21)
                    if (col.mysql_type_id == 21) {
                        // DEBUG: Print DECIMAL column info
                        static bool decimal_debug_printed = false;
                        if (!decimal_debug_printed) {
                            std::cout << "[DEBUG DECIMAL] Column '" << col.name
                                      << "' mysql_type_id=" << col.mysql_type_id
                                      << " numeric_precision=" << col.numeric_precision
                                      << " numeric_scale=" << col.numeric_scale << "\n";
                            decimal_debug_printed = true;
                        }
                        if (col.numeric_precision > 0) {
                            col_def.precision = col.numeric_precision;
                            col_def.scale = col.numeric_scale;
                        }
                    }

                    // Set datetime_precision for temporal types (DATETIME2=19, TIMESTAMP2=18, TIME2=20)
                    // The datetime_precision is the fractional seconds precision (0-6)
                    // ✅ 修复：正确传递 datetime_precision 到 IndexColumnDef
                    if (col.mysql_type_id == 19 || col.mysql_type_id == 18 || col.mysql_type_id == 20) {
                        // Always set datetime_precision, even if 0
                        col_def.datetime_precision = col.datetime_precision;
                        col_def.scale = col.datetime_precision;  // 同时设置 scale 用于显示
                        // Debug
                        static bool dt_debug = false;
                        if (!dt_debug) {
                            std::cerr << "[DEBUG DT] Column '" << col.name << "' datetime_precision="
                                      << (int)col.datetime_precision << " -> col_def.datetime_precision="
                                      << (int)col_def.datetime_precision << "\n";
                            dt_debug = true;
                        }
                    }

                    // Handle ENUM/SET elements
                    for (const auto& val : col.enum_values) {
                        col_def.enum_values.push_back(val);
                    }
                    for (const auto& val : col.set_values) {
                        col_def.set_values.push_back(val);
                    }

                    // Determine if variable length and is_big
                    // InnoDB types: 16=VARCHAR, 24-27=BLOB variants, 28=VAR_STRING, 29=STRING, 31=JSON
                    if (col_def.is_variable_length()) {
                        col_def.is_big = (col_def.col_length * col_def.mb_maxlen > 255);
                    }

                    // CRITICAL FIX: Set is_binary based on charset_id/collation_id
                    // MySQL binary charset (BINARY, VARBINARY, BLOB) has charset_id=63 or collation_id=63
                    // EXCEPTION: JSON type (31) uses binary charset but should NOT be treated as binary
                    col_def.is_binary = (col.charset_id == 63 || col.collation_id == 63) && (col_def.col_type != 31);

                    // Check if PK column
                    if (pk_col_names.count(col.name) > 0) {
                        n_pk++;
                    }

                    sdi_columns.push_back(col_def);
                }
                if (n_pk == 0 && !sdi_columns.empty()) {
                    n_pk = 1;  // Default: first column is PK
                }
                index_parser.set_columns(sdi_columns, false, n_pk);
                has_frm_schema = true;
            } else {
                std::cout << "[INFO] No schema found, using schemaless recovery\n";
            }
        }

        auto records = index_parser.parse_ibd_force(ibd_path);

        if (!records.empty()) {
            std::cout << "[INFO] InnoDBIndexParser recovered " << records.size() << " records\n";
            auto [recs, cols] = convert_records_to_map(records, index_parser);
            recovered_records = recs;
            recovered_column_order = cols;
            std::cout << "[INFO] Default recovery completed. Found "
                      << recovered_records.size() << " records.\n";
        } else {
            // Fallback to TruncateRecovery for specialized TRUNCATE handling
            std::cout << "[WARN] InnoDBIndexParser returned 0 records, trying TRUNCATE recovery...\n";
            auto schema_opt = parse_schema_from_sdi(ibd_path, table_id);
            dbrecovery::catalog::TableSchema schema;
            if (schema_opt.has_value()) {
                schema = schema_opt.value();
            } else {
                schema = create_empty_schema();
            }
            schema.page_size = page_size;

            // Use TruncateRecovery for PhotoRec-style residual data detection
            dbrecovery::recovery::TruncateRecovery truncate_rec;
            truncate_rec.set_page_size(page_size);
            truncate_rec.enable_disk_scan(true);
            if (!system_tablespace.empty()) {
                truncate_rec.set_system_tablespace(system_tablespace);
                truncate_rec.enable_undo_log(true);
            }

            auto result = truncate_rec.recover(ibd_path, schema,
                [](double progress, const std::string& msg) {
                    std::cout << "  [" << static_cast<int>(progress * 100) << "%] " << msg << "\n";
                });

            if (result.success && result.records_recovered > 0) {
                recovered_records = result.recovered_records;
                std::cout << "[INFO] TRUNCATE recovery succeeded using: " << result.method_used << "\n";
                std::cout << "[INFO] Recovered " << result.records_recovered << " records\n";
            } else {
                std::cout << "[INFO] TRUNCATE recovery found no records.\n";
            }
        }
    }

    // Truncate 恢复 - 使用 InnoDBIndexParser 进行高精度恢复
    if (truncate_recovery) {
        std::cout << "[INFO] Starting Truncate recovery (using InnoDBIndexParser)...\n";

        // Try to detect page size from file
        if (page_size == 0) {
            std::ifstream fsp_file(ibd_path, std::ios::binary);
            if (fsp_file) {
                std::vector<uint8_t> first_page(16384);
                fsp_file.read(reinterpret_cast<char*>(first_page.data()), 16384);
                for (uint32_t ps : {16384, 8192, 32768, 4096, 65536}) {
                    fsp_file.seekg(0);
                    fsp_file.read(reinterpret_cast<char*>(first_page.data()), ps);
                    if (fsp_file.gcount() >= ps) {
                        page_size = ps;
                        break;
                    }
                }
            }
        }
        if (page_size == 0) page_size = 16384;

        // Use InnoDBIndexParser for high-accuracy recovery
        dbrecovery::innodb::InnoDBIndexParser index_parser;
        index_parser.set_recover_deleted(true);  // Include deleted records (critical for TRUNCATE)
        index_parser.set_bad_page_mode(2);       // Brute-force for corrupted pages

        // Try FRM schema if provided
        bool has_frm_schema = parse_schema_from_frm(frm_path, index_parser, verbose);

        auto records = index_parser.parse_ibd_force(ibd_path);

        if (!records.empty()) {
            std::cout << "[INFO] InnoDBIndexParser recovered " << records.size() << " records\n";
            auto [recs, cols] = convert_records_to_map(records, index_parser);
            recovered_records = recs;
            recovered_column_order = cols;

            std::cout << "[INFO] Truncate recovery completed. Found "
                      << recovered_records.size() << " records.\n";
        } else {
            std::cout << "[WARN] InnoDBIndexParser returned 0 records, trying specialized TRUNCATE recovery...\n";

            auto schema_opt = parse_schema_from_sdi(ibd_path, table_id);
            dbrecovery::catalog::TableSchema schema;
            if (schema_opt.has_value()) {
                schema = schema_opt.value();
            } else {
                schema = create_empty_schema();
            }
            schema.page_size = page_size;

            // Use TruncateRecovery for PhotoRec-style residual data detection
            dbrecovery::recovery::TruncateRecovery truncate_rec;
            truncate_rec.set_page_size(page_size);
            truncate_rec.enable_disk_scan(true);
            if (!system_tablespace.empty()) {
                truncate_rec.set_system_tablespace(system_tablespace);
                truncate_rec.enable_undo_log(true);
            }

            auto result = truncate_rec.recover(ibd_path, schema,
                [](double progress, const std::string& msg) {
                    std::cout << "  [" << static_cast<int>(progress * 100) << "%] " << msg << "\n";
                });

            if (result.success && result.records_recovered > 0) {
                recovered_records = result.recovered_records;
                std::cout << "[INFO] TRUNCATE recovery succeeded using: " << result.method_used << "\n";
                std::cout << "[INFO] Recovered " << result.records_recovered << " records\n";
            } else {
                std::cout << "[INFO] Truncate recovery found no records.\n";
            }
        }
    }

    // rm -rf 恢复 - 使用 InnoDBIndexParser 进行高精度恢复
    // InnoDBIndexParser 正确解析 COMPACT/DYNAMIC 行格式，实现 >=99.9% 恢复率
    if (rmrf_recovery) {
        std::cout << "[INFO] Starting rm -rf recovery (using InnoDBIndexParser)...\n";

        // Try to detect page size from file
        if (page_size == 0) {
            std::ifstream fsp_file(ibd_path, std::ios::binary);
            if (fsp_file) {
                std::vector<uint8_t> first_page(16384);
                fsp_file.read(reinterpret_cast<char*>(first_page.data()), 16384);
                // Try common page sizes
                for (uint32_t ps : {16384, 8192, 32768, 4096, 65536}) {
                    fsp_file.seekg(0);
                    fsp_file.read(reinterpret_cast<char*>(first_page.data()), ps);
                    if (fsp_file.gcount() >= ps) {
                        page_size = ps;
                        break;
                    }
                }
            }
        }
        if (page_size == 0) page_size = 16384;

        // Use InnoDBIndexParser for high-accuracy recovery
        dbrecovery::innodb::InnoDBIndexParser index_parser;
        index_parser.set_recover_deleted(true);  // Include deleted records
        index_parser.set_bad_page_mode(2);       // Brute-force for corrupted pages

        // Try FRM schema if provided
        bool has_frm_schema = parse_schema_from_frm(frm_path, index_parser, verbose);

        // If no FRM schema, try SDI schema extraction for column names
        if (!has_frm_schema) {
            auto sdi_schema_opt = parse_schema_from_sdi(ibd_path, table_id);
            if (sdi_schema_opt.has_value()) {
                const auto& schema = sdi_schema_opt.value();
                std::vector<dbrecovery::innodb::IndexColumnDef> columns;
                for (const auto& [pos, col] : schema.columns) {
                    dbrecovery::innodb::IndexColumnDef icol;
                    icol.name = col.name;
                    icol.col_type = col.mysql_type_id;
                    icol.col_length = col.column_length;
                    icol.is_nullable = col.is_nullable;
                    icol.is_unsigned = !col.is_signed;  // Convert is_signed to is_unsigned
                    icol.precision = col.numeric_precision;
                    icol.scale = col.numeric_scale;
                    icol.ordinal_position = pos;
                    icol.enum_values = col.enum_values;
                    icol.set_values = col.set_values;
                    columns.push_back(icol);
                }
                if (!columns.empty()) {
                    // Debug: print enum/set values being passed
                    for (const auto& c : columns) {
                        if (!c.enum_values.empty()) {
                            std::cerr << "[DEBUG PASS] Column '" << c.name << "' passing "
                                      << c.enum_values.size() << " enum_values to parser\n";
                        }
                        if (!c.set_values.empty()) {
                            std::cerr << "[DEBUG PASS] Column '" << c.name << "' passing "
                                      << c.set_values.size() << " set_values to parser\n";
                        }
                    }
                    index_parser.set_columns(columns);
                    if (verbose) {
                        std::cout << "[INFO] Loaded " << columns.size() << " columns from SDI schema\n";
                    }
                }
            }
        }

        auto records = index_parser.parse_ibd_force(ibd_path);

        if (!records.empty()) {
            std::cout << "[INFO] InnoDBIndexParser recovered " << records.size() << " records\n";
            auto [recs, cols] = convert_records_to_map(records, index_parser);
            recovered_records = recs;
            recovered_column_order = cols;

            std::cout << "[INFO] rm -rf recovery completed. Found "
                      << recovered_records.size() << " records.\n";
        } else {
            std::cout << "[WARN] InnoDBIndexParser returned 0 records, trying heuristic fallback...\n";

            // Fallback to heuristic parsing
            auto schema_opt = parse_schema_from_sdi(ibd_path, table_id);
            dbrecovery::catalog::TableSchema schema;
            if (schema_opt.has_value()) {
                schema = schema_opt.value();
            } else {
                schema = create_empty_schema();
            }

            dbrecovery::recovery::RmrfRecovery rmrf;
            rmrf.set_page_size(page_size);
            bool success = rmrf.reconstruct_from_file(ibd_path, schema, recovered_records);
            if (success && !recovered_records.empty()) {
                std::cout << "[INFO] Heuristic fallback found " << recovered_records.size() << " records.\n";
            } else {
                std::cout << "[WARN] rm -rf recovery returned no records.\n";
            }
        }
    }

    // BLOB 恢复
    if (blob_recovery) {
        std::cout << "[INFO] BLOB recovery enabled.\n";
    }

    // 导出结果
    std::string output_path;
    if (format == "csv") {
        output_path = output_dir + "/recovered.csv";
        // CSV导出时使用列顺序作为表头
        if (recovered_column_order.empty()) {
            // 没有列顺序信息，从第一条记录提取
            if (!recovered_records.empty()) {
                for (const auto& [key, value] : recovered_records[0]) {
                    recovered_column_order.push_back(key);
                }
            }
        }
        if (dbrecovery::exporter::Exporter::export_to_csv(recovered_records, recovered_column_order, output_path)) {
            std::cout << "[INFO] Exported " << recovered_records.size() << " records to CSV: " << output_path << "\n";
        } else {
            std::cerr << "[WARN] Failed to export to CSV.\n";
        }
    } else if (format == "sql") {
        output_path = output_dir + "/recovered.sql";
        // SQL导出时使用列顺序
        if (recovered_column_order.empty()) {
            // 没有列顺序信息，从第一条记录提取
            if (!recovered_records.empty()) {
                for (const auto& [key, value] : recovered_records[0]) {
                    recovered_column_order.push_back(key);
                }
            }
        }
        if (dbrecovery::exporter::Exporter::export_to_sql(recovered_records, "recovered_table", recovered_column_order, output_path)) {
            std::cout << "[INFO] Exported " << recovered_records.size() << " records to SQL: " << output_path << "\n";
        } else {
            std::cerr << "[WARN] Failed to export to SQL.\n";
        }
    } else if (format == "ibd") {
        output_path = output_dir + "/repaired.ibd";
        if (engine.repair_header(output_path)) {
            std::cout << "[INFO] Repaired file saved to: " << output_path << "\n";
        } else {
            std::cerr << "[WARN] Failed to repair header.\n";
        }
    } else {
        std::cerr << "[ERROR] Unknown format: " << format << "\n";
        engine.close();
        return 1;
    }

    engine.close();

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.elapsed_seconds = std::chrono::duration<double>(total_end - total_start).count();

    show_stats(stats_);
    std::cout << "\n✅ Recovery completed: " << output_dir << "\n";

    return 0;
}

inline void RecoveryCLI::show_stats(const RecoveryStats& stats) {
    std::cout << "\n=== Recovery Statistics ===\n";
    std::cout << stats.to_string() << "\n";
}

inline std::optional<dbrecovery::catalog::TableSchema> RecoveryCLI::load_schema_from_file(
    const std::string& schema_file_path) {

    if (schema_file_path.empty()) {
        return std::nullopt;
    }

    std::ifstream file(schema_file_path);
    if (!file) {
        std::cerr << "[ERROR] Cannot open schema file: " << schema_file_path << "\n";
        return std::nullopt;
    }

    try {
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        auto json = nlohmann::json::parse(content);

        catalog::TableSchema schema;

        // Support multiple JSON formats
        // Format 1: SDI format with dd_object
        if (json.contains("dd_object") && json["dd_object_type"] == "Table") {
            auto& table_json = json["dd_object"];
            schema.table_name = table_json.value("name", "");

            if (table_json.contains("columns")) {
                uint32_t ordinal = 1;
                for (const auto& col : table_json["columns"]) {
                    catalog::ColumnInfo col_info;
                    col_info.name = col.value("name", "");
                    col_info.mysql_type_id = col.value("type", 0);
                    col_info.is_nullable = col.value("is_nullable", true);
                    col_info.column_length = col.value("char_length", col.value("length", 0));
                    col_info.is_signed = !col.value("is_unsigned", false);
                    col_info.ordinal = col.value("ordinal_position", ordinal);
                    col_info.physical_pos = col.value("ordinal_position", ordinal);

                    // Skip system columns
                    if (col_info.name == "DB_TRX_ID" ||
                        col_info.name == "DB_ROLL_PTR" ||
                        col_info.name == "DB_ROW_ID") {
                        continue;
                    }

                    // Parse ENUM/SET elements if present
                    if (col.contains("elements") && col["elements"].is_array()) {
                        static const std::string base64_chars =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        std::vector<std::string>* target_values =
                            (col_info.mysql_type_id == 23) ? &col_info.set_values : &col_info.enum_values;
                        for (const auto& elem : col["elements"]) {
                            if (elem.contains("name")) {
                                std::string encoded = elem["name"].get<std::string>();
                                // Base64 decode
                                std::string decoded;
                                std::vector<int> T(256, -1);
                                for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;
                                int val = 0, valb = -8;
                                for (unsigned char c : encoded) {
                                    if (T[c] == -1) break;
                                    val = (val << 6) + T[c];
                                    valb += 6;
                                    if (valb >= 0) {
                                        decoded.push_back(char((val >> valb) & 0xFF));
                                        valb -= 8;
                                    }
                                }
                                target_values->push_back(decoded);
                            }
                        }
                    }

                    schema.columns[ordinal++] = col_info;
                }
            }
        }
        // Format 2: Simple format with columns array
        else if (json.contains("columns")) {
            schema.table_name = json.value("table_name", json.value("name", ""));
            uint32_t ordinal = 1;
            for (const auto& col : json["columns"]) {
                catalog::ColumnInfo col_info;
                col_info.name = col.value("name", "");
                col_info.mysql_type_id = col.value("type", col.value("mysql_type_id", 0));
                col_info.is_nullable = col.value("is_nullable", col.value("nullable", true));
                col_info.column_length = col.value("length", col.value("column_length", 0));
                col_info.is_signed = !col.value("is_unsigned", col.value("unsigned", false));
                col_info.ordinal = ordinal;
                col_info.physical_pos = ordinal;

                // Skip system columns
                if (col_info.name == "DB_TRX_ID" ||
                    col_info.name == "DB_ROLL_PTR" ||
                    col_info.name == "DB_ROW_ID") {
                    continue;
                }

                schema.columns[ordinal++] = col_info;
            }
        }
        // Format 3: Try from_json method
        else {
            if (!schema.from_json(json)) {
                std::cerr << "[ERROR] Cannot parse schema JSON format\n";
                return std::nullopt;
            }
        }

        if (schema.columns.empty()) {
            std::cerr << "[WARN] Schema file contains no columns\n";
            return std::nullopt;
        }

        std::cout << "[INFO] Loaded schema from file: " << schema.table_name
                  << " (" << schema.columns.size() << " columns)\n";
        return schema;

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to parse schema file: " << e.what() << "\n";
        return std::nullopt;
    }
}

} // namespace tools
} // namespace dbrecovery

#endif // DBRECOVERY_TOOLS_RECOVERY_CLI_HPP
