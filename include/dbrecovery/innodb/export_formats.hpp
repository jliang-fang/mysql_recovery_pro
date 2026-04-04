#ifndef DBRECOVERY_INNODB_EXPORT_FORMATS_HPP
#define DBRECOVERY_INNODB_EXPORT_FORMATS_HPP

#include <vector>
#include <cstdint>
#include <map>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== JSON 导出器 ====================
class JSONExporter {
public:
    /**
     * @brief 导出为 JSON 格式
     * @param records 记录数据
     * @param output_path 输出文件路径
     * @param pretty 是否格式化输出
     * @return true 导出成功
     */
    static bool export_to_json(
        const std::vector<std::map<std::string, std::string>>& records,
        const std::filesystem::path& output_path,
        bool pretty = true) {
        
        nlohmann::json json_array = nlohmann::json::array();

        for (const auto& record : records) {
            nlohmann::json json_obj;
            for (const auto& [key, value] : record) {
                // 处理特殊值
                if (value == "NULL") {
                    json_obj[key] = nullptr;
                } else if (value.starts_with("BLOB_LEN_")) {
                    // BLOB 数据
                    json_obj[key] = value;
                } else {
                    json_obj[key] = value;
                }
            }
            json_array.push_back(json_obj);
        }

        std::ofstream file(output_path);
        if (!file) {
            return false;
        }

        if (pretty) {
            file << json_array.dump(4) << std::endl;
        } else {
            file << json_array.dump() << std::endl;
        }

        return file.good();
    }

    /**
     * @brief 从 JSON 文件导入记录
     * @param input_path 输入文件路径
     * @return 导入的记录
     */
    static std::vector<std::map<std::string, std::string>> import_from_json(
        const std::filesystem::path& input_path) {
        
        std::vector<std::map<std::string, std::string>> records;

        std::ifstream file(input_path);
        if (!file) {
            return records;
        }

        nlohmann::json json_array;
        file >> json_array;

        if (!json_array.is_array()) {
            return records;
        }

        for (const auto& json_obj : json_array) {
            if (!json_obj.is_object()) {
                continue;
            }

            std::map<std::string, std::string> record;
            for (auto it = json_obj.begin(); it != json_obj.end(); ++it) {
                if (it.value().is_null()) {
                    record[it.key()] = "NULL";
                } else if (it.value().is_string()) {
                    record[it.key()] = it.value().get<std::string>();
                } else if (it.value().is_number()) {
                    record[it.key()] = std::to_string(it.value().get<int64_t>());
                } else {
                    record[it.key()] = it.value().dump();
                }
            }
            records.push_back(record);
        }

        return records;
    }
};

// ==================== Parquet 导出器（简化版）====================
class ParquetExporter {
public:
    /**
     * @brief 导出为 Parquet 格式（简化实现）
     * @param records 记录数据
     * @param output_path 输出文件路径
     * @param schema 表结构
     * @return true 导出成功
     */
    static bool export_to_parquet(
        const std::vector<std::map<std::string, std::string>>& records,
        const std::filesystem::path& output_path,
        const catalog::TableSchema& schema) {
        
        // 简化实现：使用 CSV 格式作为 Parquet 的文本表示
        // 实际 Parquet 需要使用 parquet-cpp 库

        std::ofstream file(output_path);
        if (!file) {
            return false;
        }

        // 写入 Parquet 文件头（简化）
        file << "PAR1";  // Parquet magic number
        file << "\x00\x00\x00\x01";  // Version
        file << "\x00\x00\x00\x00";  // Metadata length

        // 写入记录
        for (const auto& record : records) {
            bool first = true;
            for (const auto& [key, value] : record) {
                if (!first) file << ",";
                file << value;
                first = false;
            }
            file << "\n";
        }

        return file.good();
    }

    /**
     * @brief 导出为 Parquet 格式（带元数据）
     * @param records 记录数据
     * @param output_path 输出文件路径
     * @param schema 表结构
     * @return true 导出成功
     */
    static bool export_to_parquet_with_metadata(
        const std::vector<std::map<std::string, std::string>>& records,
        const std::filesystem::path& output_path,
        const catalog::TableSchema& schema) {
        
        std::ofstream file(output_path, std::ios::binary);
        if (!file) {
            return false;
        }

        // 写入 Parquet magic number
        file << "PAR1";

        // 写入元数据（简化）
        nlohmann::json metadata;
        metadata["table_name"] = schema.table_name;
        metadata["row_count"] = records.size();
        metadata["columns"] = nlohmann::json::array();

        for (const auto& [col_id, col] : schema.columns) {
            nlohmann::json col_info;
            col_info["name"] = col.name;
            col_info["type"] = col.mysql_type_id;
            col_info["nullable"] = col.is_nullable;
            metadata["columns"].push_back(col_info);
        }

        // 写入元数据长度
        std::string metadata_str = metadata.dump();
        uint32_t metadata_len = static_cast<uint32_t>(metadata_str.size());
        file.write(reinterpret_cast<const char*>(&metadata_len), sizeof(metadata_len));

        // 写入元数据
        file << metadata_str;

        // 写入记录
        for (const auto& record : records) {
            for (const auto& [key, value] : record) {
                file << value << ",";
            }
            file << "\n";
        }

        // 写入结束标记
        file << "END1";

        return file.good();
    }
};

// ==================== XML 导出器 ====================
class XMLExporter {
public:
    /**
     * @brief 导出为 XML 格式
     * @param records 记录数据
     * @param output_path 输出文件路径
     * @param root_element 根元素名称
     * @param row_element 行元素名称
     * @return true 导出成功
     */
    static bool export_to_xml(
        const std::vector<std::map<std::string, std::string>>& records,
        const std::filesystem::path& output_path,
        const std::string& root_element = "records",
        const std::string& row_element = "record") {
        
        std::ofstream file(output_path);
        if (!file) {
            return false;
        }

        // 写入 XML 声明
        file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";

        // 写入根元素
        file << "<" << root_element << ">\n";

        for (const auto& record : records) {
            file << "  <" << row_element << ">\n";
            for (const auto& [key, value] : record) {
                file << "    <" << key << ">";
                file << escape_xml(value);
                file << "</" << key << ">\n";
            }
            file << "  </" << row_element << ">\n";
        }

        file << "</" << root_element << ">\n";

        return file.good();
    }

private:
    static std::string escape_xml(const std::string& str) {
        std::ostringstream oss;
        for (char c : str) {
            switch (c) {
                case '&': oss << "&amp;"; break;
                case '<': oss << "&lt;"; break;
                case '>': oss << "&gt;"; break;
                case '\"': oss << "&quot;"; break;
                case '\'': oss << "&apos;"; break;
                default: oss << c; break;
            }
        }
        return oss.str();
    }
};

// ==================== YAML 导出器 ====================
class YAMLExporter {
public:
    /**
     * @brief 导出为 YAML 格式
     * @param records 记录数据
     * @param output_path 输出文件路径
     * @return true 导出成功
     */
    static bool export_to_yaml(
        const std::vector<std::map<std::string, std::string>>& records,
        const std::filesystem::path& output_path) {
        
        std::ofstream file(output_path);
        if (!file) {
            return false;
        }

        for (const auto& record : records) {
            file << "-\n";
            for (const auto& [key, value] : record) {
                file << "  " << key << ": ";
                if (value == "NULL") {
                    file << "null";
                } else {
                    // 简单转义
                    if (value.find(' ') != std::string::npos ||
                        value.find(':') != std::string::npos) {
                        file << "\"";
                        file << escape_yaml(value);
                        file << "\"";
                    } else {
                        file << value;
                    }
                }
                file << "\n";
            }
        }

        return file.good();
    }

private:
    static std::string escape_yaml(const std::string& str) {
        std::ostringstream oss;
        for (char c : str) {
            switch (c) {
                case '\\': oss << "\\\\"; break;
                case '\"': oss << "\\\""; break;
                case '\n': oss << "\\n"; break;
                case '\t': oss << "\\t"; break;
                default: oss << c; break;
            }
        }
        return oss.str();
    }
};

// ==================== Avro 导出器（简化版）====================
class AvroExporter {
public:
    /**
     * @brief 导出为 Avro 格式（简化实现）
     * @param records 记录数据
     * @param output_path 输出文件路径
     * @param schema 表结构
     * @return true 导出成功
     */
    static bool export_to_avro(
        const std::vector<std::map<std::string, std::string>>& records,
        const std::filesystem::path& output_path,
        const catalog::TableSchema& schema) {
        
        std::ofstream file(output_path, std::ios::binary);
        if (!file) {
            return false;
        }

        // 写入 Avro magic
        file << "Obj\x01";  // Avro magic number

        // 写入 schema（JSON 格式）
        nlohmann::json avro_schema;
        avro_schema["type"] = "record";
        avro_schema["name"] = schema.table_name;
        avro_schema["fields"] = nlohmann::json::array();

        for (const auto& [col_id, col] : schema.columns) {
            nlohmann::json field;
            field["name"] = col.name;
            
            // 映射 MySQL 类型到 Avro 类型
            std::string avro_type = "string";
            switch (col.mysql_type_id) {
                case 1: case 2: case 3: case 8:  // TINY, SHORT, LONG, LONGLONG
                    avro_type = "long";
                    break;
                case 4: case 5:  // FLOAT, DOUBLE
                    avro_type = "double";
                    break;
                case 10:  // DATE
                    avro_type = "int";
                    break;
                case 12:  // DATETIME
                    avro_type = "long";
                    break;
            }

            if (col.is_nullable) {
                field["type"] = nlohmann::json::array();
                field["type"].push_back("null");
                field["type"].push_back(avro_type);
            } else {
                field["type"] = avro_type;
            }

            avro_schema["fields"].push_back(field);
        }

        std::string schema_str = avro_schema.dump();
        uint32_t schema_len = static_cast<uint32_t>(schema_str.size());
        file.write(reinterpret_cast<const char*>(&schema_len), sizeof(schema_len));
        file << schema_str;

        // 写入记录
        for (const auto& record : records) {
            for (const auto& [key, value] : record) {
                file << value << "\x00";  // Null-terminated
            }
            file << "\n";
        }

        return file.good();
    }
};

// ==================== 导出格式管理器 ====================
class ExportFormatManager {
public:
    /**
     * @brief 导出为指定格式
     * @param records 记录数据
     * @param output_path 输出文件路径
     * @param format 格式名称
     * @param schema 表结构
     * @return true 导出成功
     */
    static bool export_to_format(
        const std::vector<std::map<std::string, std::string>>& records,
        const std::filesystem::path& output_path,
        const std::string& format,
        const catalog::TableSchema& schema) {
        
        std::string lower_format = format;
        std::transform(lower_format.begin(), lower_format.end(), lower_format.begin(), ::tolower);

        if (lower_format == "json") {
            return JSONExporter::export_to_json(records, output_path);
        } else if (lower_format == "json-pretty") {
            return JSONExporter::export_to_json(records, output_path, true);
        } else if (lower_format == "parquet") {
            return ParquetExporter::export_to_parquet(records, output_path, schema);
        } else if (lower_format == "parquet-meta") {
            return ParquetExporter::export_to_parquet_with_metadata(records, output_path, schema);
        } else if (lower_format == "xml") {
            return XMLExporter::export_to_xml(records, output_path);
        } else if (lower_format == "yaml") {
            return YAMLExporter::export_to_yaml(records, output_path);
        } else if (lower_format == "avro") {
            return AvroExporter::export_to_avro(records, output_path, schema);
        } else if (lower_format == "csv") {
            // 使用默认的 CSV 导出
            return false;  // 应该使用其他导出器
        } else if (lower_format == "sql") {
            // 使用默认的 SQL 导出
            return false;  // 应该使用其他导出器
        } else {
            return false;
        }
    }

    /**
     * @brief 获取支持的格式列表
     */
    static std::vector<std::string> get_supported_formats() {
        return {
            "json",
            "json-pretty",
            "parquet",
            "parquet-meta",
            "xml",
            "yaml",
            "avro",
            "csv",
            "sql",
            "ibd"
        };
    }

    /**
     * @brief 获取格式描述
     */
    static std::string get_format_description(const std::string& format) {
        std::string lower_format = format;
        std::transform(lower_format.begin(), lower_format.end(), lower_format.begin(), ::tolower);

        static const std::map<std::string, std::string> descriptions = {
            {"json", "JSON 格式（紧凑）"},
            {"json-pretty", "JSON 格式（格式化）"},
            {"parquet", "Parquet 格式（简化）"},
            {"parquet-meta", "Parquet 格式（带元数据）"},
            {"xml", "XML 格式"},
            {"yaml", "YAML 格式"},
            {"avro", "Avro 格式（简化）"},
            {"csv", "CSV 格式"},
            {"sql", "SQL 格式"},
            {"ibd", "IBD 格式"}
        };

        auto it = descriptions.find(lower_format);
        if (it != descriptions.end()) {
            return it->second;
        }
        return "未知格式";
    }
};

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_EXPORT_FORMATS_HPP
