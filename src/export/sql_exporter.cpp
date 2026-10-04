#include "dbrecovery/export/data_exporter.hpp"
#include <sstream>
#include <algorithm>

namespace dbrecovery {
namespace exporter {

SQLOutputter::SQLOutputter(const std::string& table_name) : table_name_(table_name) {}

SQLOutputter::SQLOutputter(const std::string& table_name, const std::vector<std::string>& column_order)
    : table_name_(table_name), column_order_(column_order) {}

SQLOutputter::~SQLOutputter() = default;

bool SQLOutputter::export_record(const std::map<std::string, std::string>& record) {
    std::ostringstream oss;

    oss << "INSERT INTO `" << table_name_ << "` (";

    // Determine column order
    std::vector<std::string> columns;
    if (!column_order_.empty()) {
        columns = column_order_;
    } else {
        for (const auto& kv : record) {
            columns.push_back(kv.first);
        }
        std::sort(columns.begin(), columns.end());
    }

    // Build column names
    bool first = true;
    for (const auto& col_name : columns) {
        auto it = record.find(col_name);
        if (it == record.end()) continue;
        if (!first) oss << ", ";
        oss << "`" << col_name << "`";
        first = false;
    }

    oss << ") VALUES (";

    // Build values
    first = true;
    for (const auto& col_name : columns) {
        auto it = record.find(col_name);
        if (it == record.end()) continue;
        if (!first) oss << ", ";

        const std::string& value = it->second;
        if (value.empty() && binary_columns_.count(col_name)) {
            oss << "NULL";
        } else if (binary_columns_.count(col_name)) {
            // Binary columns are stored by the recovery pipeline as 0x<hex>.
            // UNHEX keeps the bytes intact instead of re-encoding them as text.
            std::string hex_value;
            if (value.size() > 2 && (value[0] == '0') && (value[1] == 'x' || value[1] == 'X')) {
                hex_value = value.substr(2);
            } else {
                static constexpr char digits[] = "0123456789abcdef";
                hex_value.reserve(value.size() * 2);
                for (unsigned char byte : value) {
                    hex_value.push_back(digits[byte >> 4]);
                    hex_value.push_back(digits[byte & 0x0f]);
                }
            }
            oss << "UNHEX('" << hex_value << "')";
        } else if (value == "NULL") {
            oss << "NULL";
        } else {
            std::string escaped_value;
            for (char c : value) {
                switch (c) {
                    case '\0': escaped_value += "\\0"; break;
                    case '\n': escaped_value += "\\n"; break;
                    case '\r': escaped_value += "\\r"; break;
                    case '\\': escaped_value += "\\\\"; break;
                    case '\'': escaped_value += "''"; break;
                    case '"': escaped_value += c; break;
                    case '\x1a': escaped_value += "\\Z"; break;
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
    return get_result_batch(1);  // Default: one INSERT per row
}

std::string SQLOutputter::get_result_batch(size_t batch_size) const {
    std::ostringstream oss;

    if (sql_statements_.empty()) return "";

    if (batch_size > 1) {
        // Multi-row batch header: column names appear once before the first
        // VALUES group.
        if (!column_order_.empty()) {
            oss << "INSERT INTO `" << table_name_ << "` (";
            bool first = true;
            for (const auto& col : column_order_) {
                if (!first) oss << ", ";
                oss << "`" << col << "`";
                first = false;
            }
            oss << ") VALUES\n";
        }
    }

    size_t batch_count = 0;
    for (size_t i = 0; i < sql_statements_.size(); i++) {
        if (batch_size > 1) {
            // Batch mode: extract VALUES part and group
            const std::string& stmt = sql_statements_[i];
            size_t values_pos = stmt.find("VALUES (");
            if (values_pos == std::string::npos) {
                oss << stmt << "\n";
                continue;
            }

            std::string values_part = stmt.substr(values_pos + 7);  // Skip "VALUES"
            // Remove trailing ");"
            if (values_part.size() >= 2 && values_part.back() == ';')
                values_part.pop_back();
            if (values_part.size() >= 1 && values_part.back() == ')')
                values_part.pop_back();

            if (batch_count == 0) {
                // Start new batch with column names
                if (column_order_.empty()) {
                    // Extract column names from first statement
                    size_t col_end = stmt.find(") VALUES");
                    std::string col_part = stmt.substr(0, col_end + 1);
                    oss << col_part << " VALUES\n";
                }
            }

            oss << "(" << values_part << ")";
            batch_count++;

            if (batch_count >= batch_size || i == sql_statements_.size() - 1) {
                oss << ";\n";
                batch_count = 0;
            } else {
                oss << ",\n";
            }
        } else {
            oss << sql_statements_[i] << "\n";
        }
    }

    return oss.str();
}

} // namespace export
} // namespace dbrecovery

