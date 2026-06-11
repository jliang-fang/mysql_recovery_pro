#include "dbrecovery/utils/cli_parser.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <cctype>

namespace dbrecovery {
namespace utils {

CliParser::CliParser(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // 处理 --key=value 形式
        if (arg.substr(0, 2) == "--") {
            auto pos = arg.find('=');
            if (pos != std::string::npos) {
                // --key=value 形式
                std::string key = arg.substr(2, pos - 2);
                std::string value = arg.substr(pos + 1);

                if (!key.empty()) {
                    options_[key] = value;
                }
            } else {
                // 检查下一个参数是否为值（--key value 形式）
                if (i + 1 < argc && argv[i+1][0] != '-') {
                    std::string key = arg.substr(2);
                    std::string value = argv[++i]; // 消费下一个参数

                    if (!key.empty()) {
                        options_[key] = value;
                    }
                } else {
                    // 布尔标志 --flag
                    std::string key = arg.substr(2);
                    if (!key.empty()) {
                        options_[key] = "true";
                    }
                }
            }
        }
        // 处理 -key value 形式（短选项）
        else if (arg[0] == '-' && arg.size() > 1) {
            // ✅ 新增：支持短选项组合形式（如 -abc）
            if (arg.size() > 2 && arg[1] != '-') {
                // 处理组合短选项，例如 -abc
                for (size_t j = 1; j < arg.size(); ++j) {
                    std::string key(1, arg[j]);
                    options_[key] = "true";
                }
            } else if (i + 1 < argc && argv[i+1][0] != '-') {
                // -o value 形式
                std::string key = arg.substr(1);
                std::string value = argv[++i];
                options_[key] = value;
            } else {
                // 短选项布尔标志（单个字符）
                std::string key = arg.substr(1);
                options_[key] = "true";
            }
        }
    }
}

bool CliParser::has_option(const std::string& name) const {
    return options_.find(name) != options_.end();
}

std::string CliParser::get_option(const std::string& name, const std::string& default_val) const {
    auto it = options_.find(name);
    return (it != options_.end()) ? it->second : default_val;
}

std::optional<int> CliParser::get_int_option(const std::string& name, int default_val) const {
    auto it = options_.find(name);
    if (it != options_.end()) {
        try {
            return std::stoi(it->second);
        } catch (...) {
            return default_val;
        }
    }
    return default_val;
}

bool CliParser::get_bool_option(const std::string& name, bool default_val) const {
    auto it = options_.find(name);
    if (it != options_.end()) {
        std::string val = it->second;
        return val == "true" || val == "1" || val == "yes" || val == "on";
    }
    return default_val;
}

std::optional<double> CliParser::get_double_option(const std::string& name, double default_val) const {
    auto it = options_.find(name);
    if (it != options_.end()) {
        try {
            return std::stod(it->second);
        } catch (...) {
            return default_val;
        }
    }
    return default_val;
}

const std::unordered_map<std::string, std::string>& CliParser::get_all_options() const {
    return options_;
}

void CliParser::require_options(const std::vector<std::string>& required) const {
    for (const auto& opt : required) {
        if (!has_option(opt)) {
            throw std::runtime_error("Missing required option: --" + opt);
        }
    }
}

bool CliParser::validate() const {
    // ✅ 修复：根据主程序实际使用的选项进行验证
    // 主程序使用 "ibd" 作为必选参数，"table-id" 和 "output" 为可选
    return has_option("ibd");
}

std::string CliParser::get_help_message(const std::string& program_name,
                                        const std::vector<std::string>& usage) {
    std::ostringstream oss;
    
    oss << "Usage: " << program_name << " [options]\n\n";
    oss << "Options:\n";
    oss << "  --ibd=<path>              Path to .ibd file (required)\n";
    oss << "  --output=<dir>            Output directory (default: /tmp/recovered)\n";
    oss << "  --table-id=<id>           Table ID for metadata lookup\n";
    oss << "  --page-size=<size>        Page size in bytes (4096/8192/16384/32768/65536)\n";
    oss << "  --row-format=<format>     Row format: compact/dynamic/redundant/compressed\n";
    oss << "  --threads=<n>             Number of threads for parallel processing\n";
    oss << "  --no-checksum             Skip checksum validation\n";
    oss << "  --blob-recovery           Enable BLOB recovery\n";
    oss << "  --truncate-recovery       Enable truncate recovery mode\n";
    oss << "  --header-repair           Enable header repair mode\n";
    oss << "  --format=<fmt>            Output format: csv/sql/ibd\n";
    oss << "  --verbose                 Enable verbose output\n";
    oss << "  --debug                   Enable debug output\n";
    oss << "  --help                    Show this help message\n";
    
    if (!usage.empty()) {
        oss << "\nExamples:\n";
        for (const auto& u : usage) {
            oss << "  " << u << "\n";
        }
    }
    
    return oss.str();
}

void CliParser::print_help(const std::string& program_name,
                          const std::vector<std::string>& usage) {
    std::cout << get_help_message(program_name, usage);
}

std::string CliParser::get_full_help(const std::string& program_name) {
    std::ostringstream oss;

    oss << "MySQL Recovery Pro v2.1.0 - Enhanced Recovery Features\n";
    oss << "=====================================================\n\n";
    oss << "Usage: " << program_name << " [options]\n\n";

    oss << "Basic Options:\n";
    oss << "  --ibd=<path>               Path to .ibd file (required for single file recovery)\n";
    oss << "  --data-dir=<dir>           MySQL data directory path (required for scan/search/bulk)\n";
    oss << "  --table-name=<name>        Table name to search (supports * and ? wildcards)\n";
    oss << "  --output=<dir>             Output directory (default: /tmp/recovered)\n";
    oss << "  --table-id=<id>            Table ID for SDI lookup (default: 0)\n";
    oss << "  --page-size=<size>         Page size: 4096/8192/16384/32768/65536 (default: 16384)\n";
    oss << "  --max-depth=<depth>        Max directory depth for scan (default: 10)\n";
    oss << "  --format=<csv|sql|json>    Output format (default: csv)\n";
    oss << "  --user=<name>              Database user for SQL export (default: root)\n";
    oss << "  --database=<name>          Database name for SQL export (default: recovered_db)\n";
    oss << "  --verbose                  Enable verbose output\n";
    oss << "  --debug                    Enable debug output\n\n";

    oss << "Recovery Modes:\n";
    oss << "  --check-headers            Check page headers only\n";
    oss << "  --check-corruption         Check for corruption in pages\n";
    oss << "  --repair                   Repair corrupted pages\n";
    oss << "  --truncate-recovery        Recover from truncated table\n";
    oss << "  --rmrf-recovery            Recover from rm -rf scenario\n";
    oss << "  --blob-recovery            Enable BLOB recovery\n";
    oss << "  --redo-forward             Enable redo log forward recovery\n";
    oss << "  --redo-log=<path>          Path to redo log file\n";
    oss << "  --disaster-recovery        Full disaster recovery from deleted data directory\n";
    oss << "  --validate                 Validate data integrity\n";
    oss << "  --schemaless               Schemaless recovery mode (heuristic parsing without SDI)\n";
    oss << "  --raw-scan                 Raw disk/partition scanning for InnoDB pages\n";
    oss << "  --extract-tuples           Schema-aware tuple extraction\n\n";

    oss << "Real Scenario Recovery (no backups):\n";
    oss << "  --disk=<path>              Scan raw disk/partition for deleted .ibd files\n";
    oss << "  --frm=<path>               Parse table schema from .frm file\n";
    oss << "  --scan-device              Enable PhotoRec-style device scanning\n";
    oss << "  --drop-recovery            Enable DROP TABLE recovery from disk\n\n";

    oss << "Enhanced Modes:\n";
    oss << "  --scan                     Scan mode: scan data directory for tables\n";
    oss << "  --search                   Search mode: search for tables by name\n";
    oss << "  --bulk                     Bulk mode: recover all found tables\n\n";

    oss << "Output Formats:\n";
    oss << "  csv                        Export to CSV format\n";
    oss << "  sql                        Export to SQL file with CREATE TABLE and INSERT\n";
    oss << "  json                       Export to JSON format\n";
    oss << "  ibd                        Repair and save as IBD file\n\n";

    oss << "Examples:\n\n";

    oss << "  # 1. Basic recovery from specific .ibd file\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --output=/tmp/recovery\n\n";

    oss << "  # 2. Recovery with specific table name and user\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/test/users.ibd --table-name=users --user=admin --database=testdb --output=/tmp/recovery\n\n";

    oss << "  # 3. Recovery with SQL output format\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --format=sql --output=/tmp/recovery\n\n";

    oss << "  # 4. Recovery with JSON output format\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --format=json --output=/tmp/recovery\n\n";

    oss << "  # 5. Scan entire data directory for all tables\n";
    oss << "  " << program_name << " --data-dir=/var/lib/mysql --scan --output=/tmp/scanned\n\n";

    oss << "  # 6. Search for table by name (supports wildcards)\n";
    oss << "  " << program_name << " --data-dir=/var/lib/mysql --table-name=users* --search --output=/tmp/search\n\n";

    oss << "  # 7. Bulk recovery from rm -rf scenario\n";
    oss << "  " << program_name << " --data-dir=/var/lib/mysql --rmrf-recovery --bulk --format=sql --output=/tmp/recovered\n\n";

    oss << "  # 8. Truncate recovery with specific page size\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --truncate-recovery --page-size=8192 --output=/tmp/recovery\n\n";

    oss << "  # 9. Corruption check and repair\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --check-corruption --repair --output=/tmp/repair\n\n";

    oss << "  # 10. Full disaster recovery from deleted data directory\n";
    oss << "  " << program_name << " --data-dir=/var/lib/mysql --disaster-recovery --format=all --output=/tmp/disaster\n\n";

    oss << "  # 11. Redo log forward recovery\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --redo-forward --redo-log=/var/lib/mysql/ib_logfile0 --output=/tmp/redo\n\n";

    oss << "  # 12. BLOB recovery with verbose output\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --blob-recovery --verbose --output=/tmp/blob\n\n";

    oss << "  # 13. Validate data integrity\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --validate --output=/tmp/validate\n\n";

    oss << "  # 14. Recovery with custom table ID\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --table-id=12345 --output=/tmp/recovery\n\n";

    oss << "  # 15. Multiple format export (CSV + SQL)\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --format=csv --output=/tmp/recovery\n";
    oss << "  " << program_name << " --ibd=/var/lib/mysql/db/table.ibd --format=sql --output=/tmp/recovery\n\n";

    oss << "Real Scenario Recovery Examples (no backups):\n\n";
    oss << "  # 16. TRUNCATE recovery - scan disk for residual data\n";
    oss << "  " << program_name << " --disk=/var/lib/mysql/db/table.ibd --output=/tmp/recovery\n\n";
    oss << "  # 17. DROP TABLE recovery - PhotoRec-style carving\n";
    oss << "  " << program_name << " --disk=/dev/sda1 --drop-recovery --output=/tmp/recovery\n\n";
    oss << "  # 18. Parse schema from .frm file (MySQL 5.6)\n";
    oss << "  " << program_name << " --frm=/var/lib/mysql/db/table.frm --ibd=/var/lib/mysql/db/table.ibd --output=/tmp/recovery\n\n";
    oss << "  # 19. Device scan - find deleted .ibd files on raw disk\n";
    oss << "  " << program_name << " --disk=/dev/sda1 --scan-device --output=/tmp/carved\n\n";
    oss << "  # 20. Full disk scan with raw-scan mode\n";
    oss << "  " << program_name << " --ibd=/dev/sda1 --raw-scan --output=/tmp/scan\n\n";

    oss << "=====================================================\n";
    oss << "For more information, visit: https://github.com/example/mysql-recovery\n";

    return oss.str();
}

} // namespace utils
} // namespace dbrecovery
