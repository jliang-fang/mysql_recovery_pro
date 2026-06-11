#include "dbrecovery/innodb/page_exporter.hpp"
#include "dbrecovery/innodb/page_parser_v2.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace dbrecovery {
namespace innodb {

// ==================== PageExporter 实现 ====================

PageExporter::PageExporter(const PageExportOptions& options)
    : options_(options) {
}

void PageExporter::set_options(const PageExportOptions& options) {
    options_ = options;
}

std::string PageExporter::escape_json(const std::string& str) {
    std::ostringstream oss;
    for (char c : str) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    oss << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<int>(c);
                } else {
                    oss << c;
                }
        }
    }
    return oss.str();
}

std::string PageExporter::escape_csv(const std::string& str) {
    if (str.find(',') != std::string::npos || 
        str.find('"') != std::string::npos ||
        str.find('\n') != std::string::npos) {
        return "\"" + escape_json(str) + "\"";
    }
    return str;
}

std::string PageExporter::escape_sql(const std::string& str) {
    std::ostringstream oss;
    for (char c : str) {
        if (c == '\'') {
            oss << "''";
        } else if (c == '\\') {
            oss << "\\\\";
        } else {
            oss << c;
        }
    }
    return oss.str();
}

std::string PageExporter::bytes_to_hex(const uint8_t* data, size_t size) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < size && i < 64; i++) { // 限制显示前64字节
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    if (size > 64) {
        oss << "...";
    }
    return oss.str();
}

PageExportResult PageExporter::export_page(const uint8_t* data, size_t size) {
    PageExportResult result;
    result.bytes_exported = 0;
    result.records_exported = 0;

    // 解析页
    PageParserV2 parser(data, size);
    PageParseResult parse_result = parser.parse();

    if (!parse_result.success) {
        result.success = false;
        result.error_message = "Failed to parse page: " + parse_result.error_message;
        return result;
    }

    return export_page(parse_result);
}

PageExportResult PageExporter::export_page(const PageParseResult& result) {
    PageExportResult export_result;
    export_result.bytes_exported = 0;
    export_result.records_exported = 0;

    if (options_.output_format == "json") {
        export_result.output = to_json(result);
        export_result.bytes_exported = export_result.output.size();
        export_result.records_exported = result.records.size();
    } else if (options_.output_format == "csv") {
        export_result.output = to_csv(result);
        export_result.bytes_exported = export_result.output.size();
        export_result.records_exported = result.records.size();
    } else if (options_.output_format == "sql") {
        export_result.output = to_sql(result);
        export_result.bytes_exported = export_result.output.size();
        export_result.records_exported = result.records.size();
    } else if (options_.output_format == "binary") {
        auto binary = to_binary(result);
        export_result.output.assign(binary.begin(), binary.end());
        export_result.bytes_exported = binary.size();
        export_result.records_exported = result.records.size();
    } else {
        export_result.success = false;
        export_result.error_message = "Unknown output format: " + options_.output_format;
        return export_result;
    }

    export_result.success = true;
    return export_result;
}

PageExportResult PageExporter::export_page_chain(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& pages) {
    PageExportResult result;
    result.success = true;
    result.records_exported = 0;

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"pages\": [\n";

    bool first = true;
    for (const auto& [page_no, data] : pages) {
        if (!first) oss << ",\n";
        first = false;

        oss << "    {\n";
        oss << "      \"page_no\": " << page_no << ",\n";
        
        if (options_.include_header) {
            oss << "      \"header_hex\": \"" << bytes_to_hex(data.data(), 38) << "\",\n";
        }
        
        if (options_.include_records && data.size() > 38) {
            PageParserV2 parser(data.data(), data.size());
            auto parse_result = parser.parse();
            if (parse_result.success) {
                oss << "      \"records\": [\n";
                for (size_t i = 0; i < parse_result.records.size() && i < options_.max_records; i++) {
                    const auto& record = parse_result.records[i];
                    oss << "        {\"offset\": " << record.offset 
                        << ", \"heap_no\": " << record.heap_no 
                        << ", \"deleted\": " << (record.is_deleted ? "true" : "false") << "}";
                    if (i < parse_result.records.size() - 1 && i < options_.max_records - 1) {
                        oss << ",";
                    }
                    oss << "\n";
                }
                oss << "      ],\n";
                result.records_exported += parse_result.records.size();
            }
        }
        
        oss << "      \"checksum_hex\": \"" << bytes_to_hex(data.data() + 24, 4) << "\"\n";
        oss << "    }";
    }

    oss << "\n  ]\n";
    oss << "}\n";

    result.output = oss.str();
    result.bytes_exported = result.output.size();
    return result;
}

std::string PageExporter::to_json(const PageParseResult& result) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    oss << "{\n";
    oss << "  \"success\": " << (result.success ? "true" : "false") << ",\n";
    
    if (!result.success) {
        oss << "  \"error\": \"" << escape_json(result.error_message) << "\"\n";
        oss << "}\n";
        return oss.str();
    }

    oss << "  \"page_type\": " << result.page_type << ",\n";
    oss << "  \"page_level\": " << result.page_level << ",\n";
    oss << "  \"page_offset\": " << result.page_offset << ",\n";
    oss << "  \"prev_page\": " << result.prev_page << ",\n";
    oss << "  \"next_page\": " << result.next_page << ",\n";
    
    oss << "  \"row_format\": ";
    switch (result.row_format) {
        case RowFormat::REDUNDANT: oss << "\"REDUNDANT\""; break;
        case RowFormat::COMPACT: oss << "\"COMPACT\""; break;
        case RowFormat::DYNAMIC: oss << "\"DYNAMIC\""; break;
        case RowFormat::COMPRESSED: oss << "\"COMPRESSED\""; break;
        default: oss << "\"UNKNOWN\"";
    }
    oss << ",\n";

    oss << "  \"mysql_version\": ";
    switch (result.version) {
        case MySQLVersion::MYSQL_56: oss << "56"; break;
        case MySQLVersion::MYSQL_57: oss << "57"; break;
        case MySQLVersion::MYSQL_80: oss << "80"; break;
        case MySQLVersion::MYSQL_84: oss << "84"; break;
        default: oss << "0";
    }
    oss << ",\n";

    if (options_.include_header) {
        oss << "  \"header\": {\n";
        oss << "    \"checksum_stored\": " << result.checksum_stored << ",\n";
        oss << "    \"checksum_computed\": " << result.checksum_computed << ",\n";
        oss << "    \"checksum_valid\": " << (result.checksum_valid ? "true" : "false") << "\n";
        oss << "  },\n";
    }

    if (options_.include_records && !result.records.empty()) {
        oss << "  \"records\": [\n";
        size_t count = std::min(result.records.size(), options_.max_records);
        for (size_t i = 0; i < count; i++) {
            const auto& record = result.records[i];
            oss << "    {\n";
            oss << "      \"offset\": " << record.offset << ",\n";
            oss << "      \"heap_no\": " << record.heap_no << ",\n";
            oss << "      \"next_offset\": " << record.next_offset << ",\n";
            oss << "      \"is_deleted\": " << (record.is_deleted ? "true" : "false") << ",\n";
            oss << "      \"info_bits\": 0x" << std::hex << std::setfill('0') 
                << std::setw(2) << static_cast<int>(record.info_bits) << std::dec << "\n";
            oss << "    }";
            if (i < count - 1) oss << ",";
            oss << "\n";
        }
        if (result.records.size() > options_.max_records) {
            oss << "    ...\n  ],\n";
            oss << "  \"record_count_truncated\": true,\n";
        } else {
            oss << "  ],\n";
        }
    }

    if (options_.include_sdi && result.schema.has_value()) {
        const auto& schema = result.schema.value();
        oss << "  \"schema\": {\n";
        oss << "    \"table_name\": \"" << escape_json(schema.table_name) << "\",\n";
        oss << "    \"engine\": \"" << escape_json(schema.engine) << "\",\n";
        oss << "    \"row_format\": " << schema.row_format << ",\n";
        oss << "    \"page_size\": " << schema.page_size << ",\n";
        oss << "    \"column_count\": " << schema.columns.size() << ",\n";
        oss << "    \"index_count\": " << schema.indexes.size() << "\n";
        oss << "  },\n";
    }

    oss << "  \"health\": {\n";
    oss << "    \"checksum_match\": " << (result.checksum_valid ? "true" : "false") << "\n";
    oss << "  }\n";

    oss << "}\n";

    return oss.str();
}

std::string PageExporter::to_csv(const PageParseResult& result) {
    std::ostringstream oss;

    // 表头
    oss << "offset,heap_no,next_offset,is_deleted,info_bits\n";

    // 记录数据
    for (const auto& record : result.records) {
        oss << record.offset << ","
            << record.heap_no << ","
            << record.next_offset << ","
            << (record.is_deleted ? "1" : "0") << ","
            << "0x" << std::hex << std::setfill('0') << std::setw(2) 
            << static_cast<int>(record.info_bits) << std::dec << "\n";
    }

    return oss.str();
}

std::string PageExporter::to_sql(const PageParseResult& result, const std::string& table_name) {
    std::ostringstream oss;

    oss << "-- Recovered page data\n";
    oss << "-- Page type: " << result.page_type << "\n";
    oss << "-- Page level: " << result.page_level << "\n";
    oss << "-- MySQL version: " << static_cast<int>(result.version) << "\n\n";

    oss << "CREATE TABLE IF NOT EXISTS " << table_name << " (\n";
    oss << "  id BIGINT AUTO_INCREMENT PRIMARY KEY,\n";
    oss << "  record_offset INT,\n";
    oss << "  heap_no INT,\n";
    oss << "  next_offset INT,\n";
    oss << "  is_deleted BOOLEAN,\n";
    oss << "  info_bits BINARY(1),\n";
    oss << "  record_data BLOB\n";
    oss << ");\n\n";

    // 插入记录
    for (const auto& record : result.records) {
        oss << "INSERT INTO " << table_name << " (record_offset, heap_no, next_offset, is_deleted, info_bits) VALUES (";
        oss << record.offset << ", " << record.heap_no << ", " << record.next_offset << ", ";
        oss << (record.is_deleted ? "TRUE" : "FALSE") << ", X'" << std::hex << std::setfill('0') 
            << std::setw(2) << static_cast<int>(record.info_bits) << std::dec << "');\n";
    }

    return oss.str();
}

std::vector<uint8_t> PageExporter::to_binary(const PageParseResult& result) {
    std::vector<uint8_t> binary;

    // 添加页头信息（使用 memcpy 避免 const 问题）
    binary.insert(binary.end(), result.records.size());
    
    // 使用 memcpy 复制页类型
    uint16_t page_type = result.page_type;
    binary.insert(binary.end(), 
                  reinterpret_cast<uint8_t*>(&page_type),
                  reinterpret_cast<uint8_t*>(&page_type) + sizeof(page_type));
    
    // 使用 memcpy 复制页级别
    uint16_t page_level = result.page_level;
    binary.insert(binary.end(),
                  reinterpret_cast<uint8_t*>(&page_level),
                  reinterpret_cast<uint8_t*>(&page_level) + sizeof(page_level));

    // 添加记录偏移
    for (const auto& record : result.records) {
        uint16_t offset = record.offset;
        binary.insert(binary.end(),
                      reinterpret_cast<const uint8_t*>(&offset),
                      reinterpret_cast<const uint8_t*>(&offset) + sizeof(offset));
    }

    return binary;
}

// ==================== PageImporter 实现 ====================

std::optional<std::vector<uint8_t>> PageImporter::from_json(const std::string& json_str) {
    // 简化实现：返回空
    // 实际应解析 JSON 并重建页数据
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> PageImporter::from_csv(const std::string& csv_str, size_t page_size) {
    // 简化实现：返回空
    // 实际应解析 CSV 并重建页数据
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> PageImporter::from_sql(const std::string& sql_str, size_t page_size) {
    // 简化实现：返回空
    // 实际应解析 SQL 并重建页数据
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> PageImporter::from_binary_file(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        return std::nullopt;
    }

    return data;
}

// ==================== PageStatisticsCalculator 实现 ====================

PageStatisticsCalculator::PageStatisticsCalculator() {
    stats_ = {0, 0, 0, 0, 0, 0, {}, {}, {}, {}};
}

void PageStatisticsCalculator::add_page(const uint8_t* data, size_t size) {
    stats_.total_pages++;

    PageParserV2 parser(data, size);
    auto result = parser.parse();

    if (result.success) {
        stats_.valid_pages++;
        stats_.total_records += result.records.size();
        
        // 统计页类型
        stats_.page_type_counts[result.page_type]++;
        
        // 统计版本
        stats_.version_counts[result.version]++;
        
        // 统计行格式
        stats_.format_counts[result.row_format]++;
        
        // 统计页状态
        auto state = PageHealthChecker::get_page_state(result.page_type);
        stats_.state_counts[state]++;
        
        stats_.total_bytes += size;
    } else {
        stats_.invalid_pages++;
    }
}

void PageStatisticsCalculator::add_page(const PageParseResult& result) {
    stats_.total_pages++;

    if (result.success) {
        stats_.valid_pages++;
        stats_.total_records += result.records.size();
        
        stats_.page_type_counts[result.page_type]++;
        stats_.version_counts[result.version]++;
        stats_.format_counts[result.row_format]++;
        
        auto state = PageHealthChecker::get_page_state(result.page_type);
        stats_.state_counts[state]++;
    } else {
        stats_.invalid_pages++;
    }
}

PageStatistics PageStatisticsCalculator::calculate() const {
    return stats_;
}

void PageStatisticsCalculator::reset() {
    stats_ = {0, 0, 0, 0, 0, 0, {}, {}, {}, {}};
}

// ==================== PageComparator 实现 ====================

PageComparison PageComparator::compare(const uint8_t* data1, size_t size1,
                                        const uint8_t* data2, size_t size2) {
    PageComparison result;
    result.identical = true;
    result.bytes_different = 0;
    result.records_different = 0;

    // 比较页头
    size_t min_size = std::min(size1, size2);
    for (size_t i = 0; i < min_size; i++) {
        if (data1[i] != data2[i]) {
            result.identical = false;
            result.bytes_different++;
        }
    }

    if (size1 != size2) {
        result.identical = false;
        result.bytes_different += std::abs(static_cast<ptrdiff_t>(size1) - static_cast<ptrdiff_t>(size2));
    }

    return result;
}

PageComparison PageComparator::compare(const PageParseResult& result1,
                                        const PageParseResult& result2) {
    PageComparison result;
    result.identical = true;
    result.bytes_different = 0;
    result.records_different = 0;

    // 比较页信息
    if (result1.page_type != result2.page_type) {
        result.identical = false;
        result.differences.push_back("Page type differs");
    }
    if (result1.page_level != result2.page_level) {
        result.identical = false;
        result.differences.push_back("Page level differs");
    }
    if (result1.page_offset != result2.page_offset) {
        result.identical = false;
        result.differences.push_back("Page offset differs");
    }

    // 比较记录
    if (result1.records.size() != result2.records.size()) {
        result.identical = false;
        result.records_different = std::abs(static_cast<ptrdiff_t>(result1.records.size()) - 
                                           static_cast<ptrdiff_t>(result2.records.size()));
        result.differences.push_back("Record count differs");
    } else {
        for (size_t i = 0; i < result1.records.size(); i++) {
            const auto& r1 = result1.records[i];
            const auto& r2 = result2.records[i];
            if (r1.offset != r2.offset || r1.heap_no != r2.heap_no || r1.is_deleted != r2.is_deleted) {
                result.identical = false;
                result.records_different++;
            }
        }
    }

    return result;
}

} // namespace innodb
} // namespace dbrecovery
