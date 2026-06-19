#include "dbrecovery/export/data_exporter.hpp"
#include <sstream>

namespace dbrecovery {
namespace exporter {

SQLOutputter::SQLOutputter(const std::string& table_name) : table_name_(table_name) {}

SQLOutputter::SQLOutputter(const std::string& table_name, const std::vector<std::string>& column_order)
    : table_name_(table_name), column_order_(column_order) {}

SQLOutputter::~SQLOutputter() = default;

bool SQLOutputter::export_record(const std::map<std::string, std::string>& record) {
    std::ostringstream oss;

    oss << "INSERT INTO `" << table_name_ << "` (";

    // 确定列顺序
    std::vector<std::string> columns;
    if (!column_order_.empty()) {
        // 使用指定的列顺序
        columns = column_order_;
    } else {
        // 使用 map 的顺序（字母排序）
        for (const auto& [key, value] : record) {
            columns.push_back(key);
        }
    }

    // 构建列名列表
    bool first = true;
    for (const auto& col_name : columns) {
        auto it = record.find(col_name);
        if (it == record.end()) continue;  // 跳过不存在的列

        if (!first) oss << ", ";
        oss << "`" << col_name << "`";
        first = false;
    }

    oss << ") VALUES (";

    // 构建值列表
    first = true;
    for (const auto& col_name : columns) {
        auto it = record.find(col_name);
        if (it == record.end()) continue;  // 跳过不存在的列

        if (!first) oss << ", ";

        const std::string& value = it->second;

        // 处理 NULL 值
        if (value == "NULL") {
            oss << "NULL";
        } else {
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
        }
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

