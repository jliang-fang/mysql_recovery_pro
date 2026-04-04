#include "dbrecovery/innodb/data_exporter.hpp"
#include <sstream>
#include <iomanip>

namespace dbrecovery {
namespace innodb {

// ==================== DataExporter ====================

ExportResult DataExporter::export_page(const uint8_t* data, size_t size) {
    IndexPageV2 page;
    if (!page.parse(data, size, RowFormat::COMPACT)) {
        result_.errors++;
        result_.error_message = "Failed to parse page";
        return result_;
    }
    
    return export_index_page(page);
}

ExportResult DataExporter::export_index_page(const IndexPageV2& page) {
    result_.rows_exported = 0;
    result_.bytes_written = 0;
    result_.errors = 0;
    
    for (const auto& rec : page.records) {
        if (rec.compact_row.has_value()) {
            std::string row_data = export_row(rec.compact_row.value());
            result_.bytes_written += row_data.size();
            result_.rows_exported++;
        } else if (rec.dynamic_row.has_value()) {
            std::string row_data = export_row(rec.dynamic_row.value());
            result_.bytes_written += row_data.size();
            result_.rows_exported++;
        } else if (rec.redundant_row.has_value()) {
            std::string row_data = export_row(rec.redundant_row.value());
            result_.bytes_written += row_data.size();
            result_.rows_exported++;
        }
    }
    
    return result_;
}

std::string DataExporter::export_row(const CompactRow& row) {
    switch (config_.format) {
        case ExportFormat::SQL:
            return to_sql(row);
        case ExportFormat::CSV:
            return to_csv(row);
        case ExportFormat::JSON:
            return to_json(row);
        case ExportFormat::XML:
            return to_xml(row);
        case ExportFormat::TEXT:
        default:
            return to_text(row);
    }
}

std::string DataExporter::export_row(const DynamicRow& row) {
    switch (config_.format) {
        case ExportFormat::SQL:
            return to_sql(row);
        case ExportFormat::CSV:
            return to_csv(row);
        case ExportFormat::JSON:
            return to_json(row);
        case ExportFormat::XML:
            return to_xml(row);
        case ExportFormat::TEXT:
        default:
            return to_text(row);
    }
}

std::string DataExporter::export_row(const RedundantRow& row) {
    switch (config_.format) {
        case ExportFormat::SQL:
            return to_sql(row);
        case ExportFormat::CSV:
            return to_csv(row);
        case ExportFormat::JSON:
            return to_json(row);
        case ExportFormat::XML:
            return to_xml(row);
        case ExportFormat::TEXT:
        default:
            return to_text(row);
    }
}

std::string DataExporter::to_sql(const CompactRow& row) {
    std::ostringstream oss;
    
    if (config_.include_header && config_.table_name.empty()) {
        oss << "-- Table: unknown\n";
    }
    
    oss << "INSERT INTO " << config_.table_name << " VALUES (";
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        if (i > 0) oss << ", ";
        oss << format_value(row.columns[i]);
    }
    
    oss << ");\n";
    
    if (config_.include_transaction) {
        oss << "-- Transaction: " << row.transaction_id.to_string() << "\n";
        oss << "-- Roll Pointer: " << std::hex << row.roll_pointer << std::dec << "\n";
    }
    
    return oss.str();
}

std::string DataExporter::to_sql(const DynamicRow& row) {
    std::ostringstream oss;
    
    oss << "INSERT INTO " << config_.table_name << " VALUES (";
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        if (i > 0) oss << ", ";
        oss << format_value(row.columns[i]);
    }
    
    oss << ");\n";
    
    if (config_.include_transaction) {
        oss << "-- Transaction: " << row.transaction_id.to_string() << "\n";
        oss << "-- Roll Pointer: " << std::hex << row.roll_pointer << std::dec << "\n";
    }
    
    return oss.str();
}

std::string DataExporter::to_sql(const RedundantRow& row) {
    std::ostringstream oss;
    
    oss << "INSERT INTO " << config_.table_name << " VALUES (";
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        if (i > 0) oss << ", ";
        oss << format_value(row.columns[i]);
    }
    
    oss << ");\n";
    
    if (config_.include_transaction) {
        oss << "-- Transaction: " << row.transaction_id.to_string() << "\n";
    }
    
    return oss.str();
}

std::string DataExporter::to_csv(const CompactRow& row) {
    std::ostringstream oss;
    
    if (config_.include_header) {
        for (size_t i = 0; i < row.columns.size(); i++) {
            if (i > 0) oss << ",";
            oss << "col" << i;
        }
        oss << "\n";
    }
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        if (i > 0) oss << ",";
        oss << escape_csv(format_value(row.columns[i]));
    }
    oss << "\n";
    
    return oss.str();
}

std::string DataExporter::to_csv(const DynamicRow& row) {
    std::ostringstream oss;
    
    if (config_.include_header) {
        for (size_t i = 0; i < row.columns.size(); i++) {
            if (i > 0) oss << ",";
            oss << "col" << i;
        }
        oss << "\n";
    }
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        if (i > 0) oss << ",";
        oss << escape_csv(format_value(row.columns[i]));
    }
    oss << "\n";
    
    return oss.str();
}

std::string DataExporter::to_csv(const RedundantRow& row) {
    std::ostringstream oss;
    
    if (config_.include_header) {
        for (size_t i = 0; i < row.columns.size(); i++) {
            if (i > 0) oss << ",";
            oss << "col" << i;
        }
        oss << "\n";
    }
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        if (i > 0) oss << ",";
        oss << escape_csv(format_value(row.columns[i]));
    }
    oss << "\n";
    
    return oss.str();
}

std::string DataExporter::to_json(const CompactRow& row) {
    std::ostringstream oss;
    oss << "{\n";
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        oss << "  \"col" << i << "\": " << format_value(row.columns[i]);
        if (i < row.columns.size() - 1) oss << ",";
        oss << "\n";
    }
    
    if (config_.include_transaction) {
        oss << "  \"_transaction\": \"" << row.transaction_id.to_string() << "\",\n";
        oss << "  \"_roll_pointer\": " << std::hex << row.roll_pointer << std::dec << "\n";
    }
    
    oss << "}\n";
    
    return oss.str();
}

std::string DataExporter::to_json(const DynamicRow& row) {
    std::ostringstream oss;
    oss << "{\n";
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        oss << "  \"col" << i << "\": " << format_value(row.columns[i]);
        if (i < row.columns.size() - 1) oss << ",";
        oss << "\n";
    }
    
    if (config_.include_transaction) {
        oss << "  \"_transaction\": \"" << row.transaction_id.to_string() << "\",\n";
        oss << "  \"_roll_pointer\": " << std::hex << row.roll_pointer << std::dec << "\n";
    }
    
    oss << "}\n";
    
    return oss.str();
}

std::string DataExporter::to_json(const RedundantRow& row) {
    std::ostringstream oss;
    oss << "{\n";
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        oss << "  \"col" << i << "\": " << format_value(row.columns[i]);
        if (i < row.columns.size() - 1) oss << ",";
        oss << "\n";
    }
    
    if (config_.include_transaction) {
        oss << "  \"_transaction\": \"" << row.transaction_id.to_string() << "\"\n";
    }
    
    oss << "}\n";
    
    return oss.str();
}

std::string DataExporter::to_xml(const CompactRow& row) {
    std::ostringstream oss;
    oss << "<row>\n";
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        oss << "  <col" << i << ">" << escape_xml(format_value(row.columns[i])) << "</col" << i << ">\n";
    }
    
    if (config_.include_transaction) {
        oss << "  <_transaction>" << escape_xml(row.transaction_id.to_string()) << "</_transaction>\n";
        oss << "  <_roll_pointer>" << std::hex << row.roll_pointer << std::dec << "</_roll_pointer>\n";
    }
    
    oss << "</row>\n";
    
    return oss.str();
}

std::string DataExporter::to_xml(const DynamicRow& row) {
    std::ostringstream oss;
    oss << "<row>\n";
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        oss << "  <col" << i << ">" << escape_xml(format_value(row.columns[i])) << "</col" << i << ">\n";
    }
    
    if (config_.include_transaction) {
        oss << "  <_transaction>" << escape_xml(row.transaction_id.to_string()) << "</_transaction>\n";
        oss << "  <_roll_pointer>" << std::hex << row.roll_pointer << std::dec << "</_roll_pointer>\n";
    }
    
    oss << "</row>\n";
    
    return oss.str();
}

std::string DataExporter::to_xml(const RedundantRow& row) {
    std::ostringstream oss;
    oss << "<row>\n";
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        oss << "  <col" << i << ">" << escape_xml(format_value(row.columns[i])) << "</col" << i << ">\n";
    }
    
    if (config_.include_transaction) {
        oss << "  <_transaction>" << escape_xml(row.transaction_id.to_string()) << "</_transaction>\n";
    }
    
    oss << "</row>\n";
    
    return oss.str();
}

std::string DataExporter::to_text(const CompactRow& row) {
    std::ostringstream oss;
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        if (i > 0) oss << " | ";
        oss << format_value(row.columns[i]);
    }
    oss << "\n";
    
    if (config_.include_transaction) {
        oss << "Transaction: " << row.transaction_id.to_string() << "\n";
        oss << "Roll Pointer: " << std::hex << row.roll_pointer << std::dec << "\n";
    }
    
    return oss.str();
}

std::string DataExporter::to_text(const DynamicRow& row) {
    std::ostringstream oss;
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        if (i > 0) oss << " | ";
        oss << format_value(row.columns[i]);
    }
    oss << "\n";
    
    if (config_.include_transaction) {
        oss << "Transaction: " << row.transaction_id.to_string() << "\n";
        oss << "Roll Pointer: " << std::hex << row.roll_pointer << std::dec << "\n";
    }
    
    return oss.str();
}

std::string DataExporter::to_text(const RedundantRow& row) {
    std::ostringstream oss;
    
    for (size_t i = 0; i < row.columns.size(); i++) {
        if (i > 0) oss << " | ";
        oss << format_value(row.columns[i]);
    }
    oss << "\n";
    
    if (config_.include_transaction) {
        oss << "Transaction: " << row.transaction_id.to_string() << "\n";
    }
    
    return oss.str();
}

} // namespace innodb
} // namespace dbrecovery
