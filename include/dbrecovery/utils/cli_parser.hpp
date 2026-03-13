#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

namespace dbrecovery {
namespace utils {

class CliParser {
public:
    explicit CliParser(int argc, char** argv);
    
    bool has_option(const std::string& name) const;
    std::string get_option(const std::string& name, const std::string& default_val = "") const;
    
    // ✅ 新增：参数验证方法
    void require_options(const std::vector<std::string>& required) const;
    bool validate() const;

private:
    std::unordered_map<std::string, std::string> options_;
};

// ==================== 实现部分 ====================

inline CliParser::CliParser(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        // ✅ 新增：支持 --option=value 格式
        if (arg.substr(0, 2) == "--" && arg.find('=') != std::string::npos) {
            size_t pos = arg.find('=');
            std::string key = arg.substr(2, pos - 2);
            std::string value = arg.substr(pos + 1);
            options_[key] = value;
        } else if (arg.substr(0, 2) == "--" && i + 1 < argc && argv[i+1][0] != '-') {
            // --option value 形式
            std::string key = arg.substr(2);
            std::string value = argv[++i];
            options_[key] = value;
        } else if (arg.substr(0, 2) == "--") {
            // --option 形式（布尔标志）
            std::string key = arg.substr(2);
            options_[key] = "true";
        } else if (arg[0] == '-' && i + 1 < argc && argv[i+1][0] != '-') {
            // -o value 形式
            std::string key = arg.substr(1);
            std::string value = argv[++i];
            options_[key] = value;
        } else if (arg[0] == '-' && arg.length() > 2) {
            // ✅ 新增：支持短选项组合 -abc 等价于 -a -b -c
            for (size_t j = 1; j < arg.length(); ++j) {
                options_[std::string(1, arg[j])] = "true";
            }
        } else if (arg[0] == '-') {
            // -o 形式（布尔标志）
            std::string key = arg.substr(1);
            options_[key] = "true";
        }
    }
}

inline bool CliParser::has_option(const std::string& name) const {
    return options_.find(name) != options_.end();
}

inline std::string CliParser::get_option(const std::string& name, const std::string& default_val) const {
    auto it = options_.find(name);
    if (it != options_.end()) {
        return it->second;
    }
    return default_val;
}

inline void CliParser::require_options(const std::vector<std::string>& required) const {
    for (const auto& opt : required) {
        if (!has_option(opt)) {
            throw std::runtime_error("Missing required option: " + opt);
        }
    }
}

inline bool CliParser::validate() const {
    // ✅ 增强：根据主程序实际使用的选项进行验证
    // 主程序使用 "ibd" 作为必选参数，"table-id" 和 "output" 为可选
    if (!has_option("ibd")) {
        return false;
    }
    
    // 验证 ibd 文件路径是否存在（如果文件系统可访问）
    // 这里只做基本验证，实际文件存在性检查在主程序中进行
    
    return true;
}

} // namespace utils
} // namespace dbrecovery
