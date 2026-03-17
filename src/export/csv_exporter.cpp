#include "dbrecovery/export/data_exporter.hpp"
#include <sstream>

namespace dbrecovery {
namespace export {

DataExporter::DataExporter() {}

DataExporter::~DataExporter() = default;

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

} // namespace export
} // namespace dbrecovery

