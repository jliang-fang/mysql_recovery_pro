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

} // namespace utils
} // namespace dbrecovery
