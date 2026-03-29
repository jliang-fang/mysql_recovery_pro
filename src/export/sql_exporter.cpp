#include "dbrecovery/export/data_exporter.hpp"
#include <sstream>

namespace dbrecovery {
namespace exporter {

SQLOutputter::SQLOutputter(const std::string& table_name) : table_name_(table_name) {}

SQLOutputter::~SQLOutputter() = default;

bool SQLOutputter::export_record(const std::map<std::string, std::string>& record) {
    std::ostringstream oss;

    oss << "INSERT INTO `" << table_name_ << "` (";

    // 构建列名列表
    bool first = true;
    for (const auto& [key, value] : record) {
        if (!first) oss << ", ";
        oss << "`" << key << "`";
        first = false;
    }

    oss << ") VALUES (";

    // 构建值列表
    first = true;
    for (const auto& [key, value] : record) {
        if (!first) oss << ", ";

        // 转义SQL特殊字符（遵循 MySQL ANSI_QUOTES 模式）
        std::string escaped_value;
        for (char c : value) {
            switch (c) {
                case '\0': escaped_value += "\\0"; break;
                case '\n': escaped_value += "\\n"; break;
                case '\r': escaped_value += "\\r"; break;
                case '\\': escaped_value += "\\\\"; break;
                case '\'': escaped_value += "''"; break;
                case '"': escaped_value += "\"\""; break;
                case '\x1a': escaped_value += "\\Z"; break; // Ctrl+Z
                default: escaped_value += c;
            }
        }

        oss << "'" << escaped_value << "'";
        first = false;
    }

    oss << ");";

    sql_statements_.push_back(oss.str());
    return true;
}

std::string SQLOutputter::get_result() const {
    std::ostringstream oss;

    for (const auto& stmt : sql_statements_) {
        oss << stmt << "\n";
    }

    return oss.str();
}

} // namespace export
} // namespace dbrecovery

