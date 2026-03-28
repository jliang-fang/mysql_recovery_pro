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

} // namespace utils
} // namespace dbrecovery
