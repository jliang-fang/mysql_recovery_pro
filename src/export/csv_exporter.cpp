#include "dbrecovery/export/data_exporter.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include <sstream>
#include <algorithm>
#include <iomanip>

namespace dbrecovery {
namespace exporter {

CSVExporter::CSVExporter(const std::vector<std::string>& headers) : headers_(headers) {}

CSVExporter::~CSVExporter() = default;

bool CSVExporter::export_record(const std::map<std::string, std::string>& record) {
    std::ostringstream oss;

    // DEBUG: Track comma count
    size_t comma_count = 0;

    for (size_t i = 0; i < headers_.size(); ++i) {
        if (i > 0) {
            oss << ",";
            comma_count++;
        }

        auto it = record.find(headers_[i]);
        if (it != record.end()) {
            // 处理 NULL 值
            std::string value = it->second;
            // CRITICAL FIX: Remove NUL bytes from value to prevent CSV corruption
            value.erase(std::remove(value.begin(), value.end(), '\0'), value.end());

            if (value == "NULL") {
                oss << "";  // CSV 中 NULL 表示为空字符串
            } else {
                // 转义CSV字段中的逗号、引号、换行符
                bool needs_quotes = value.find(',') != std::string::npos ||
                                   value.find('"') != std::string::npos ||
                                   value.find('\n') != std::string::npos ||
                                   value.find('\r') != std::string::npos;

                if (needs_quotes) {
                    oss << "\"";
                    for (char c : value) {
                        if (c == '"') oss << "\"\"";
                        else oss << c;
                    }
                    oss << "\"";
                } else {
                    oss << value;
                    // Check if value contains comma (shouldn't happen if needs_quotes is correct)
                    size_t value_commas = 0;
                    for (char c : value) {
                        if (c == ',') value_commas++;
                    }
                    comma_count += value_commas;
                }
            }
        } else {
            // 字段不存在，输出空字符串
            oss << "";
        }
    }

    // DEBUG: Check comma count
    static int comma_debug = 0;
    if (comma_debug < 5 && comma_count != headers_.size() - 1) {
        if (dbrecovery::innodb::g_verbose)         std::cerr << "[DEBUG COMMA] expected " << (headers_.size() - 1) << " commas, got " << comma_count << "\n";
        comma_debug++;
    }

    rows_.push_back(oss.str());
    return true;
}

std::string CSVExporter::get_result() const {
    std::ostringstream oss;

    // 添加表头
    for (size_t i = 0; i < headers_.size(); ++i) {
        if (i > 0) oss << ",";
        oss << headers_[i];
    }
    oss << "\n";

    // 添加数据行
    for (const auto& row : rows_) {
        // DEBUG: Check row content
        static int get_result_debug = 0;
        if (get_result_debug < 3) {
            if (dbrecovery::innodb::g_verbose)             std::cerr << "[DEBUG GET_RESULT] row length=" << row.size()
                      << " first 50 chars=" << row.substr(0, 50) << "\n";
            get_result_debug++;
        }
        oss << row << "\n";
    }

    return oss.str();
}

// Exporter::export_to_csv only - other functions in ibd_exporter.cpp
bool Exporter::export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                             const std::vector<std::string>& headers,
                             const std::string& output_path) {
    CSVExporter exporter(headers);

    for (const auto& record : records) {
        if (!exporter.export_record(record)) {
            std::cerr << "[ERROR] Failed to export record to CSV" << std::endl;
            return false;
        }
    }

    return exporter.export_to_file(output_path);
}

// ==================== JSONExporter ====================

JSONExporter::JSONExporter(bool pretty) : pretty_(pretty) {}
JSONExporter::~JSONExporter() = default;

bool JSONExporter::export_record(const std::map<std::string, std::string>& record) {
    std::ostringstream oss;
    oss << (pretty_ ? "    {\n" : "{");

    bool first = true;
    for (const auto& [key, value] : record) {
        if (!first) oss << (pretty_ ? ",\n" : ",");
        if (pretty_) oss << "        ";
        oss << "\"" << escape_json(key) << "\": ";

        // Detect numeric/bool/null values
        if (value == "NULL" || value.empty()) {
            oss << "null";
        } else if (value == "true" || value == "false") {
            oss << value;
        } else {
            // Try as number, fall back to string
            bool is_num = !value.empty() && (std::isdigit(value[0]) || value[0] == '-');
            if (is_num) {
                bool all_digits = true;
                bool has_dot = false;
                for (size_t i = (value[0] == '-' ? 1 : 0); i < value.size(); i++) {
                    if (value[i] == '.' && !has_dot) { has_dot = true; continue; }
                    if (!std::isdigit(value[i])) { all_digits = false; break; }
                }
                if (all_digits) {
                    oss << value;
                } else {
                    oss << "\"" << escape_json(value) << "\"";
                }
            } else {
                oss << "\"" << escape_json(value) << "\"";
            }
        }

        first = false;
    }
    oss << (pretty_ ? "\n    }" : "}");

    rows_.push_back(oss.str());
    return true;
}

std::string JSONExporter::get_result() const {
    std::ostringstream oss;
    oss << (pretty_ ? "[\n" : "[");
    for (size_t i = 0; i < rows_.size(); i++) {
        if (i > 0) oss << (pretty_ ? ",\n" : ",");
        oss << rows_[i];
    }
    oss << (pretty_ ? "\n]\n" : "]\n");
    return oss.str();
}

bool JSONExporter::export_to_file_streaming(const std::string& file_path) {
    std::ofstream file(file_path);
    if (!file) return false;

    file << (pretty_ ? "[\n" : "[");
    for (size_t i = 0; i < rows_.size(); i++) {
        if (i > 0) file << (pretty_ ? ",\n" : ",");
        file << rows_[i];
    }
    file << (pretty_ ? "\n]\n" : "]\n");
    file.close();
    return true;
}

std::string escape_json(const std::string& str) {
    std::ostringstream oss;
    for (char c : str) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c);
                } else {
                    oss << c;
                }
        }
    }
    return oss.str();
}

// ==================== Exporter::export_to_json ====================

bool Exporter::export_to_json(const std::vector<std::map<std::string, std::string>>& records,
                              const std::string& output_path, bool pretty) {
    JSONExporter exporter(pretty);

    for (const auto& record : records) {
        if (!exporter.export_record(record)) {
            return false;
        }
    }

    return exporter.export_to_file_streaming(output_path);
}

} // namespace exporter
} // namespace dbrecovery

