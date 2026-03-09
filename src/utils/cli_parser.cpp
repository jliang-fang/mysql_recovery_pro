#include "dbrecovery/utils/cli_parser.hpp"
#include <algorithm>

namespace dbrecovery {
namespace utils {

CliParser::CliParser(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
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
                        options_[key] = "";
                    }
                }
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

} // namespace utils
} // namespace dbrecovery
