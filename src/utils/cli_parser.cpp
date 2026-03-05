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
                options_[arg.substr(2, pos - 2)] = arg.substr(pos + 1);
            } else {
                options_[arg.substr(2)] = "";
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
