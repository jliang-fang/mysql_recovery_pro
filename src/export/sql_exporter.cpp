#include "dbrecovery/export/data_exporter.hpp"
#include <sstream>

namespace dbrecovery {
namespace export {

SQLOutputter::SQLOutputter(const std::string& table_name) : table_name_(table_name) {}

SQLOutputter::~SQLOutputter() = default;

bool SQLOutputter::export_record(const std::map<std::string, std::string>& record) {
    std::ostringstream oss;

    oss << "INSERT INTO " << table_name_ << " (";

    // 构建列名列表
    bool first = true;
    for (const auto& [key, value] : record) {
        if (!first) oss << ", ";
        oss << key;
        first = false;
    }

    oss << ") VALUES (";

    // 构建值列表
    first = true;
    for (const auto& [key, value] : record) {
        if (!first) oss << ", ";

        // 转义SQL特殊字符
        std::string escaped_value;
        for (char c : value) {
            if (c == '\'') escaped_value += "''";
            else if (c == '\\') escaped_value += "\\\\";
            else escaped_value += c;
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
