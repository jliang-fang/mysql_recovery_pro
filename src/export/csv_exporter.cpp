#include "dbrecovery/export/data_exporter.hpp"
#include <sstream>

namespace dbrecovery {
namespace exporter {

CSVExporter::CSVExporter(const std::vector<std::string>& headers) : headers_(headers) {}

CSVExporter::~CSVExporter() = default;

bool CSVExporter::export_record(const std::map<std::string, std::string>& record) {
    std::ostringstream oss;

    for (size_t i = 0; i < headers_.size(); ++i) {
        if (i > 0) oss << ",";

        auto it = record.find(headers_[i]);
        if (it != record.end()) {
            // ✅ 新增：处理 NULL 值
            std::string value = it->second;
            if (value == "NULL") {
                oss << "";  // CSV 中 NULL 表示为空字符串
            } else {
                // 转义CSV字段中的逗号和引号
                bool needs_quotes = value.find(',') != std::string::npos ||
                                   value.find('"') != std::string::npos ||
                                   value.find('\n') != std::string::npos;

                if (needs_quotes) {
                    oss << "\"";
                    for (char c : value) {
                        if (c == '"') oss << "\"\"";
                        else oss << c;
                    }
                    oss << "\"";
                } else {
                    oss << value;
                }
            }
        } else {
            // 字段不存在，输出空字符串
            oss << "";
        }
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

