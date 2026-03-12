#include "dbrecovery/utils/cli_parser.hpp"
#include <algorithm>
#include <iostream>

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
            if (i + 1 < argc && argv[i+1][0] != '-') {
                std::string key = arg.substr(1);
                std::string value = argv[++i];
                options_[key] = value;
            } else {
                // 短选项布尔标志
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

void CliParser::require_options(const std::vector<std::string>& required) const {
    for (const auto& opt : required) {
        if (!has_option(opt)) {
            throw std::runtime_error("Missing required option: --" + opt);
        }
    }
}

bool CliParser::validate() const {
    // ✅ 新增：基本验证逻辑
    return !options_.empty();
}

} // namespace utils
} // namespace dbrecovery
