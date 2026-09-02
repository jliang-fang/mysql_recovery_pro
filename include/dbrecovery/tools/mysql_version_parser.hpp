#ifndef DBRECOVERY_TOOLS_MYSQL_VERSION_PARSER_HPP
#define DBRECOVERY_TOOLS_MYSQL_VERSION_PARSER_HPP

#include <cctype>
#include <optional>
#include <string>

#include "dbrecovery/common/version_compat.hpp"

namespace dbrecovery {
namespace tools {

struct ParsedMySQLVersion {
    std::string family;
    MySQLVersion version = MySQLVersion::UNKNOWN;
};

inline std::optional<ParsedMySQLVersion> parse_mysql_version(const std::string& value) {
    const auto first_dot = value.find('.');
    if (first_dot == std::string::npos || first_dot == 0 || first_dot == value.size() - 1) {
        return std::nullopt;
    }

    const auto second_dot = value.find('.', first_dot + 1);
    if (second_dot != std::string::npos &&
        (second_dot == first_dot + 1 || second_dot == value.size() - 1 ||
         value.find('.', second_dot + 1) != std::string::npos)) {
        return std::nullopt;
    }

    for (const char c : value) {
        if (c != '.' && !std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }

    const std::string family = value.substr(
        0, second_dot == std::string::npos ? value.size() : second_dot);
    MySQLVersion version = MySQLVersion::UNKNOWN;
    if (family == "5.6") version = MySQLVersion::MYSQL_56;
    else if (family == "5.7") version = MySQLVersion::MYSQL_57;
    else if (family == "8.0") version = MySQLVersion::MYSQL_80;
    else if (family == "8.1") version = MySQLVersion::MYSQL_81;
    else if (family == "8.2") version = MySQLVersion::MYSQL_82;
    else if (family == "8.3") version = MySQLVersion::MYSQL_83;
    else if (family == "8.4") version = MySQLVersion::MYSQL_84;
    else if (family == "9.0") version = MySQLVersion::MYSQL_90;

    if (version == MySQLVersion::UNKNOWN) {
        return std::nullopt;
    }
    return ParsedMySQLVersion{family, version};
}

} // namespace tools
} // namespace dbrecovery

#endif // DBRECOVERY_TOOLS_MYSQL_VERSION_PARSER_HPP
