#include "dbrecovery/export/data_exporter.hpp"
#include "dbrecovery/innodb/index_page_parser.hpp"
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>

namespace dbrecovery {
namespace exporter {

namespace {
std::string csv_escape(const std::string& value) {
    const bool needs_quotes = value.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs_quotes) return value;

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '"') escaped.push_back('"');
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

std::string hex_encode(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char c : value) {
        encoded.push_back(digits[c >> 4]);
        encoded.push_back(digits[c & 0x0f]);
    }
    return encoded;
}

bool has_invalid_utf8(const std::string& value) {
    size_t i = 0;
    while (i < value.size()) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        size_t width = 0;
        uint32_t codepoint = 0;
        if (c <= 0x7f) {
            ++i;
            continue;
        } else if (c >= 0xc2 && c <= 0xdf) {
            width = 2; codepoint = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            width = 3; codepoint = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            width = 4; codepoint = c & 0x07;
        } else {
            return true;
        }
        if (i + width > value.size()) return true;
        for (size_t j = 1; j < width; ++j) {
            const unsigned char continuation = static_cast<unsigned char>(value[i + j]);
            if ((continuation & 0xc0) != 0x80) return true;
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if ((width == 3 && codepoint < 0x800) ||
            (width == 4 && codepoint < 0x10000) ||
            codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) return true;
        i += width;
    }
    return false;
}

std::string canonical_csv_text(const ExportValue& value) {
    if (value.is_null) return {};
    if (value.kind == ExportValue::Kind::BINARY) return hex_encode(value.value);
    if (value.kind == ExportValue::Kind::INVALID_UTF8 ||
        (value.kind == ExportValue::Kind::TEXT && has_invalid_utf8(value.value))) return {};
    std::string text;
    text.reserve(value.value.size());
    for (unsigned char c : value.value) {
        if (c == 0) text += "\\u0000";
        else text.push_back(static_cast<char>(c));
    }
    return text;
}

bool export_value_valid(const ExportValue& value) {
    return value.is_null || value.kind != ExportValue::Kind::INVALID_UTF8 &&
        (value.kind != ExportValue::Kind::TEXT || !has_invalid_utf8(value.value));
}

std::string csv_row(const std::vector<std::string>& headers,
                    const std::map<std::string, ExportValue>& record) {
    std::ostringstream oss;
    for (size_t i = 0; i < headers.size(); ++i) {
        if (i > 0) oss << ',';
        const auto it = record.find(headers[i]);
        if (it != record.end()) oss << csv_escape(canonical_csv_text(it->second));
    }
    return oss.str();
}

nlohmann::json json_value(const ExportValue& value) {
    if (value.is_null) return nullptr;
    switch (value.kind) {
        case ExportValue::Kind::NUMBER:
            try { return nlohmann::json::parse(value.value); }
            catch (...) { return value.value; }
        case ExportValue::Kind::BOOLEAN:
            return value.value == "true";
        case ExportValue::Kind::BINARY:
            return hex_encode(value.value);
        case ExportValue::Kind::INVALID_UTF8:
            return nullptr;
        case ExportValue::Kind::TEXT:
        default:
            return value.value;
    }
}

std::string json_record(const std::vector<std::string>& headers,
                        const std::map<std::string, ExportValue>& record,
                        bool pretty) {
    nlohmann::json object = nlohmann::json::object();
    for (const auto& header : headers) {
        const auto it = record.find(header);
        object[header] = it == record.end() ? nlohmann::json(nullptr)
                                            : json_value(it->second);
    }
    return object.dump(pretty ? 4 : -1);
}
}

CSVExporter::CSVExporter(const std::vector<std::string>& headers) : headers_(headers) {}

CSVExporter::~CSVExporter() = default;

bool CSVExporter::export_record(const std::map<std::string, std::string>& record) {
    std::map<std::string, ExportValue> values;
    for (const auto& [key, value] : record) values[key] = ExportValue::text(value);
    rows_.push_back(csv_row(headers_, values));
    return true;
}

std::string CSVExporter::get_result() const {
    std::ostringstream oss;
    for (size_t i = 0; i < headers_.size(); ++i) {
        if (i > 0) oss << ',';
        oss << csv_escape(headers_[i]);
    }
    oss << '\n';
    for (const auto& row : rows_) oss << row << '\n';
    return oss.str();
}

bool Exporter::export_to_csv(const std::vector<std::map<std::string, std::string>>& records,
                             const std::vector<std::string>& headers,
                             const std::string& output_path) {
    CSVExporter exporter(headers);
    for (const auto& record : records) {
        if (!exporter.export_record(record)) return false;
    }
    return exporter.export_to_file(output_path);
}

bool Exporter::export_to_csv(const std::vector<std::map<std::string, ExportValue>>& records,
                             const std::vector<std::string>& headers,
                             const std::string& output_path) {
    std::ostringstream output;
    for (size_t i = 0; i < headers.size(); ++i) {
        if (i > 0) output << ',';
        output << csv_escape(headers[i]);
    }
    output << '\n';
    size_t record_index = 0;
    for (const auto& record : records) {
        for (const auto& [header, value] : record) {
            if (!export_value_valid(value)) {
                if (innodb::g_verbose) {
                    std::cerr << "[DEBUG EXPORT] invalid CSV value at record="
                              << record_index << " column=" << header
                              << " bytes=" << hex_encode(value.value) << "\n";
                }
                return false;
            }
        }
        output << csv_row(headers, record) << '\n';
        ++record_index;
    }

    std::ofstream file(output_path);
    if (!file) return false;
    file << output.str();
    return static_cast<bool>(file);
}

// ==================== JSONExporter ====================

JSONExporter::JSONExporter(bool pretty) : pretty_(pretty) {}
JSONExporter::~JSONExporter() = default;

bool JSONExporter::export_record(const std::map<std::string, std::string>& record) {
    std::map<std::string, ExportValue> values;
    for (const auto& [key, value] : record) values[key] = ExportValue::text(value);
    std::vector<std::string> headers;
    headers.reserve(values.size());
    for (const auto& [key, value] : values) {
        (void)value;
        headers.push_back(key);
    }
    rows_.push_back(json_record(headers, values, pretty_));
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

bool Exporter::export_to_json(const std::vector<std::map<std::string, ExportValue>>& records,
                              const std::vector<std::string>& headers,
                              const std::string& output_path, bool pretty) {
    std::ofstream file(output_path);
    if (!file) return false;
    file << '[';
    for (size_t i = 0; i < records.size(); ++i) {
        for (const auto& [header, value] : records[i]) {
            (void)header;
            if (!export_value_valid(value)) return false;
        }
        if (i > 0) file << ',';
        if (pretty) file << '\n';
        file << json_record(headers, records[i], pretty);
    }
    if (pretty && !records.empty()) file << '\n';
    file << "]\n";
    return static_cast<bool>(file);
}

} // namespace exporter
} // namespace dbrecovery

