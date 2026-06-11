#ifndef DBRECOVERY_INNODB_DATA_EXPORTER_HPP
#define DBRECOVERY_INNODB_DATA_EXPORTER_HPP

#include "dbrecovery/innodb/page_types_v2.hpp"
#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace dbrecovery {
namespace innodb {

/**
 * @brief 导出格式
 */
enum class ExportFormat {
    SQL,        // SQL INSERT 语句
    CSV,        // CSV 格式
    JSON,       // JSON 格式
    XML,        // XML 格式
    YAML,       // YAML 格式
    Avro,       // Avro 格式
    TEXT,       // 纯文本格式
    IBD,        // 修复后的 IBD 文件
};

/**
 * @brief 导出配置
 */
struct ExportConfig {
    ExportFormat format = ExportFormat::SQL;
    bool include_header = true;        // 包含列名
    bool include_transaction = false;  // 包含事务信息
    bool include_timestamp = false;    // 包含时间戳
    size_t batch_size = 1000;          // 批量大小
    std::string table_name;            // 表名
    std::vector<std::string> columns;  // 列名
    
    std::string to_string() const;
};

/**
 * @brief 导出结果
 */
struct ExportResult {
    size_t rows_exported;
    size_t bytes_written;
    size_t errors;
    std::string error_message;
    
    std::string to_string() const;
};

/**
 * @brief 数据导出器
 */
class DataExporter {
public:
    DataExporter();
    
    /**
     * @brief 设置配置
     */
    void set_config(const ExportConfig& config);
    
    /**
     * @brief 导出页数据
     */
    ExportResult export_page(const uint8_t* data, size_t size);
    
    /**
     * @brief 导出 B+树索引页
     */
    ExportResult export_index_page(const IndexPageV2& page);
    
    /**
     * @brief 导出行数据
     */
    std::string export_row(const CompactRow& row);
    std::string export_row(const DynamicRow& row);
    std::string export_row(const RedundantRow& row);
    
    /**
     * @brief 导出为 SQL
     */
    std::string to_sql(const CompactRow& row);
    std::string to_sql(const DynamicRow& row);
    std::string to_sql(const RedundantRow& row);
    
    /**
     * @brief 导出为 CSV
     */
    std::string to_csv(const CompactRow& row);
    std::string to_csv(const DynamicRow& row);
    std::string to_csv(const RedundantRow& row);
    
    /**
     * @brief 导出为 JSON
     */
    std::string to_json(const CompactRow& row);
    std::string to_json(const DynamicRow& row);
    std::string to_json(const RedundantRow& row);
    
    /**
     * @brief 导出为 XML
     */
    std::string to_xml(const CompactRow& row);
    std::string to_xml(const DynamicRow& row);
    std::string to_xml(const RedundantRow& row);
    
    /**
     * @brief 导出为文本
     */
    std::string to_text(const CompactRow& row);
    std::string to_text(const DynamicRow& row);
    std::string to_text(const RedundantRow& row);
    
    /**
     * @brief 获取导出结果
     */
    ExportResult get_result() const;
    
    /**
     * @brief 重置导出器
     */
    void reset();
    
private:
    ExportConfig config_;
    ExportResult result_;
    
    std::string escape_sql(const std::string& str);
    std::string escape_csv(const std::string& str);
    std::string escape_xml(const std::string& str);
    std::string format_value(const ColumnInfo& col);
};

// ==================== 实现 ====================

inline DataExporter::DataExporter() {
    config_ = ExportConfig{};
    result_ = {0, 0, 0, ""};
}

inline void DataExporter::set_config(const ExportConfig& config) {
    config_ = config;
}

inline ExportResult DataExporter::get_result() const {
    return result_;
}

inline void DataExporter::reset() {
    result_ = {0, 0, 0, ""};
}

inline std::string ExportConfig::to_string() const {
    std::ostringstream oss;
    oss << "ExportConfig{"
        << "format=" << static_cast<int>(format)
        << ", header=" << (include_header ? "true" : "false")
        << ", txn=" << (include_transaction ? "true" : "false")
        << ", ts=" << (include_timestamp ? "true" : "false")
        << ", batch=" << batch_size
        << ", table=" << table_name
        << "}";
    return oss.str();
}

inline std::string ExportResult::to_string() const {
    std::ostringstream oss;
    oss << "ExportResult{"
        << "rows=" << rows_exported
        << ", bytes=" << bytes_written
        << ", errors=" << errors;
    if (!error_message.empty()) {
        oss << ", error=" << error_message;
    }
    oss << "}";
    return oss.str();
}

// ==================== DataExporter ====================

inline std::string DataExporter::escape_sql(const std::string& str) {
    std::string result;
    for (char c : str) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '\'': result += "\\'"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

inline std::string DataExporter::escape_csv(const std::string& str) {
    bool need_quote = false;
    for (char c : str) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            need_quote = true;
            break;
        }
    }
    
    if (!need_quote) {
        return str;
    }
    
    std::string result = "\"";
    for (char c : str) {
        if (c == '"') {
            result += "\"\"";
        } else {
            result += c;
        }
    }
    result += "\"";
    return result;
}

inline std::string DataExporter::escape_xml(const std::string& str) {
    std::string result;
    for (char c : str) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += c; break;
        }
    }
    return result;
}

inline std::string DataExporter::format_value(const ColumnInfo& col) {
    if (col.is_null) {
        return "NULL";
    }
    
    // 简化处理：将数据转换为十六进制字符串
    std::ostringstream oss;
    oss << "0x";
    for (uint8_t b : col.data) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_DATA_EXPORTER_HPP
