#pragma once

#include <string>
#include <unordered_map>

namespace dbrecovery {
namespace utils {

class CliParser {
public:
    explicit CliParser(int argc, char** argv);
    
    bool has_option(const std::string& name) const;
    std::string get_option(const std::string& name, const std::string& default_val = "") const;

private:
    std::unordered_map<std::string, std::string> options_;
};

} // namespace utils
} // namespace dbrecovery
