#include "dbrecovery/export/data_exporter.hpp"
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
        std::cerr << "[DEBUG COMMA] expected " << (headers_.size() - 1) << " commas, got " << comma_count << "\n";
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
            std::cerr << "[DEBUG GET_RESULT] row length=" << row.size()
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

} // namespace exporter
} // namespace dbrecovery

