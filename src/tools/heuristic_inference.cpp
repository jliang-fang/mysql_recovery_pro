#include "dbrecovery/tools/heuristic_inference.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <sstream>

namespace dbrecovery {
namespace tools {

// ==================== HeuristicInference::Impl ====================

class HeuristicInference::Impl {
public:
    Impl()
        : page_data_(nullptr), page_size_(0), file_path_("") {}

    void set_page_data(const uint8_t* data, size_t size) {
        page_data_ = data;
        page_size_ = size;
    }

    void set_file_path(const std::string& path) {
        file_path_ = path;
    }

    std::optional<TableSchema> infer_schema() {
        if (!page_data_ || page_size_ < 16384) {
            return std::nullopt;
        }

        TableSchema schema;

        // Detect version
        schema.version = HeuristicInference::infer_version(page_data_, page_size_);

        // Detect row format
        schema.row_format = HeuristicInference::infer_row_format(page_data_, page_size_);

        // Extract column metadata
        auto col_meta = HeuristicInference::extract_column_metadata(page_data_, page_size_);

        // Find and parse records
        auto records = find_records();
        if (records.empty()) {
            return std::nullopt;
        }

        // Infer columns from first record
        if (!records.empty() && !records[0].data.empty()) {
            schema.columns = infer_columns_impl(
                records[0].data.data(),
                records[0].data.size(),
                col_meta
            );
        }

        // Estimate row count
        schema.estimated_row_count = estimate_row_count_impl(
            static_cast<uint32_t>(page_size_ / 16384), 100
        );

        // Generate CREATE statement
        schema.create_ddl = HeuristicInference::generate_create_statement(schema);

        return schema;
    }

    std::vector<ColumnInfo> infer_columns(
        const uint8_t* record_data,
        size_t record_len,
        const std::vector<uint8_t>& col_meta
    ) {
        std::vector<ColumnInfo> columns;

        for (size_t i = 0; i < col_meta.size() && i < 50; i++) {
            ColumnInfo col;
            col.name = infer_column_name_impl(static_cast<uint32_t>(i));
            col.type = infer_column_type_impl(col_meta[i], col_meta, static_cast<uint32_t>(i));
            col.is_nullable = true;  // Default to nullable
            col.is_primary_key = (i == 0);  // First column is often PK
            col.ordinal_position = static_cast<uint32_t>(i);

            columns.push_back(col);
        }

        return columns;
    }

private:
    std::string infer_column_name_impl(uint32_t column_index) {
        if (column_index < 26) {
            return std::string(1, 'A' + column_index);
        } else if (column_index < 702) {
            char first = 'A' + (column_index / 26) - 1;
            char second = 'A' + (column_index % 26);
            return std::string(1, first) + second;
        } else {
            return "COL_" + std::to_string(column_index);
        }
    }

    std::string infer_column_type_impl(uint8_t type_code, const std::vector<uint8_t>& col_meta, uint32_t col_index) {
        (void)col_meta;
        (void)col_index;

        switch (type_code) {
            case 1:   return "TINYINT";
            case 2:   return "SMALLINT";
            case 3:   return "MEDIUMINT";
            case 4:   return "INT";
            case 8:   return "BIGINT";
            case 5:   return "FLOAT";
            case 6:   return "DOUBLE";
            case 7:   return "TIMESTAMP";
            case 10:  return "DATE";
            case 11:  return "TIME";
            case 12:  return "DATETIME";
            case 13:  return "YEAR";
            case 15:  return "VARCHAR(255)";
            case 16:  return "BIT";
            case 24:  return "VARCHAR(65535)";
            case 25:  return "VARCHAR(65535)";
            case 253: return "VARCHAR(65535)";
            case 252: return "TEXT";
            case 251: return "JSON";
            case 245: return "JSON";
            default:
                if (type_code >= 0 && type_code <= 4) return "INT";
                else if (type_code >= 5 && type_code <= 6) return "DOUBLE";
                else if (type_code >= 10 && type_code <= 13) return "VARCHAR(255)";
                else if (type_code >= 240) return "BLOB";
                else return "BINARY(16)";
        }
    }

    std::vector<ColumnInfo> infer_columns_impl(
        const uint8_t* record_data,
        size_t record_len,
        const std::vector<uint8_t>& col_meta
    ) {
        std::vector<ColumnInfo> columns;

        for (size_t i = 0; i < col_meta.size() && i < 50; i++) {
            ColumnInfo col;
            col.name = infer_column_name_impl(static_cast<uint32_t>(i));
            col.type = infer_column_type_impl(col_meta[i], col_meta, static_cast<uint32_t>(i));
            col.is_nullable = true;
            col.is_primary_key = (i == 0);
            col.ordinal_position = static_cast<uint32_t>(i);

            columns.push_back(col);
        }

        return columns;
    }

    uint64_t estimate_row_count_impl(uint32_t page_count, uint32_t avg_row_size) {
        if (page_count == 0 || avg_row_size == 0) {
            return 0;
        }

        const uint32_t page_size = 16384;
        const uint32_t header_overhead = 128;

        uint32_t records_per_page = (page_size - header_overhead) / avg_row_size;
        if (records_per_page == 0) {
            records_per_page = 1;
        }

        return static_cast<uint64_t>(page_count) * records_per_page;
    }

    std::vector<uint8_t> extract_column_metadata(const uint8_t* page_data, size_t page_size) {
        std::vector<uint8_t> col_meta;

        // Look for column metadata in page header or record data
        // In practice, this would be extracted from SDI or BTR_HEADER

        // For now, return a default metadata based on page type
        if (page_size < 16384) {
            return col_meta;
        }

        // Try to detect number of columns from page structure
        uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data, 38);
        if (n_dir_slots > 0 && n_dir_slots < 100) {
            // Estimate columns from directory slots
            uint32_t n_cols = std::min(static_cast<uint32_t>(n_dir_slots / 2), 50u);
            for (uint32_t i = 0; i < n_cols; i++) {
                col_meta.push_back(15);  // Default to VARCHAR
            }
        }

        return col_meta;
    }

    struct Record {
        uint32_t offset;
        std::vector<uint8_t> data;
        mysql_recovery::RowParser::RecordHeader header;
    };

    std::vector<Record> find_records() {
        std::vector<Record> records;

        if (!page_data_ || page_size_ < 100) {
            return records;
        }

        // Find data start offset
        uint16_t data_start = dbrecovery::innodb::read_le<uint16_t>(page_data_, 38);
        if (data_start < 100 || data_start > page_size_ - 10) {
            data_start = 96;  // Default
        }

        // Scan for records starting from data start
        uint16_t heap_top = dbrecovery::innodb::read_le<uint16_t>(page_data_, 40);
        if (heap_top < data_start + 10 || heap_top > page_size_) {
            heap_top = static_cast<uint16_t>(page_size_);
        }

        uint16_t current_offset = data_start;
        while (current_offset < heap_top - 10) {
            // Check if this looks like a record
            if (current_offset + 5 >= page_size_) {
                break;
            }

            Record rec;
            rec.offset = current_offset;
            rec.header = HeuristicInference::parse_record_header(
                page_data_ + current_offset,
                mysql_recovery::RowFormat::COMPACT
            );

            // Calculate record length
            uint16_t rec_len = 5;  // Minimum header size
            if (rec.header.next_record_offset > current_offset) {
                rec_len = rec.header.next_record_offset - current_offset;
            }

            if (rec_len < 5 || rec_len > 65536) {
                break;
            }

            if (current_offset + rec_len > page_size_) {
                break;
            }

            // Extract record data
            rec.data.assign(page_data_ + current_offset, page_data_ + current_offset + rec_len);
            records.push_back(rec);

            // Move to next record
            if (rec.header.next_record_offset <= current_offset) {
                break;
            }
            current_offset = rec.header.next_record_offset;
        }

        return records;
    }

private:
    const uint8_t* page_data_;
    size_t page_size_;
    std::string file_path_;
};

// ==================== HeuristicInference 公共接口 ====================

HeuristicInference::HeuristicInference()
    : impl_(std::make_unique<Impl>()),
      max_columns_(50),
      min_records_(1) {
}

HeuristicInference::~HeuristicInference() = default;

void HeuristicInference::set_page_data(const uint8_t* page_data, size_t page_size) {
    if (impl_) {
        impl_->set_page_data(page_data, page_size);
    }
}

void HeuristicInference::set_file_path(const std::string& file_path) {
    if (impl_) {
        impl_->set_file_path(file_path);
    }
}

std::optional<HeuristicInference::TableSchema> HeuristicInference::infer_schema() {
    if (impl_) {
        return impl_->infer_schema();
    }
    return std::nullopt;
}

std::vector<HeuristicInference::ColumnInfo> HeuristicInference::infer_columns(
    const uint8_t* record_data,
    size_t record_len,
    const std::vector<uint8_t>& col_meta
) {
    if (impl_) {
        return impl_->infer_columns(record_data, record_len, col_meta);
    }
    return {};
}

std::string HeuristicInference::infer_column_name(uint32_t column_index) {
    if (column_index < 26) {
        // Use single letter for first 26 columns
        return std::string(1, 'A' + column_index);
    } else if (column_index < 702) {
        // Use two letters for next 676 columns
        char first = 'A' + (column_index / 26) - 1;
        char second = 'A' + (column_index % 26);
        return std::string(1, first) + second;
    } else {
        return "COL_" + std::to_string(column_index);
    }
}

std::string HeuristicInference::infer_column_type(uint8_t type_code, const std::vector<uint8_t>& col_meta, uint32_t col_index) {
    (void)col_meta;  // Unused
    (void)col_index;  // Unused

    switch (type_code) {
        case 1:   // TINYINT
            return "TINYINT";
        case 2:   // SMALLINT
            return "SMALLINT";
        case 3:   // MEDIUMINT
            return "MEDIUMINT";
        case 4:   // INT
            return "INT";
        case 8:   // BIGINT
            return "BIGINT";
        case 5:   // FLOAT
            return "FLOAT";
        case 6:   // DOUBLE
            return "DOUBLE";
        case 7:   // TIMESTAMP (old format)
            return "TIMESTAMP";
        case 10:  // DATE
            return "DATE";
        case 11:  // TIME
            return "TIME";
        case 12:  // DATETIME
            return "DATETIME";
        case 13:  // YEAR
            return "YEAR";
        case 15:  // VARCHAR
            return "VARCHAR(255)";
        case 16:  // BIT
            return "BIT";
        case 24:  // VARCHAR (new format)
            return "VARCHAR(65535)";
        case 25:  // VAR_STRING
            return "VARCHAR(65535)";
        case 253: // VAR_STRING (new format)
            return "VARCHAR(65535)";
        case 252: // BLOB
            return "TEXT";
        case 251: // JSON
            return "JSON";
        case 245: // JSON (new format)
            return "JSON";
        default:
            // Fallback based on type code range
            if (type_code >= 0 && type_code <= 4) {
                return "INT";
            } else if (type_code >= 5 && type_code <= 6) {
                return "DOUBLE";
            } else if (type_code >= 10 && type_code <= 13) {
                return "VARCHAR(255)";
            } else if (type_code >= 240) {
                return "BLOB";
            } else {
                return "BINARY(16)";
            }
    }
}

uint64_t HeuristicInference::estimate_row_count(uint32_t page_count, uint32_t avg_row_size) {
    if (page_count == 0 || avg_row_size == 0) {
        return 0;
    }

    // Each page can hold approximately (page_size - header_overhead) / avg_row_size records
    const uint32_t page_size = 16384;
    const uint32_t header_overhead = 128;  // Page header + BTR header

    uint32_t records_per_page = (page_size - header_overhead) / avg_row_size;
    if (records_per_page == 0) {
        records_per_page = 1;
    }

    return static_cast<uint64_t>(page_count) * records_per_page;
}

mysql_recovery::RowFormat HeuristicInference::infer_row_format(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 100) {
        return mysql_recovery::RowFormat::UNKNOWN;
    }

    // Check page header version (8.0+ has header_version field)
    if (page_size >= 56) {
        uint16_t header_version = dbrecovery::innodb::read_le<uint16_t>(page_data, 54);
        if (header_version > 0) {
            return mysql_recovery::RowFormat::DYNAMIC;
        }
    }

    // Check for SDI page (8.0+ only)
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data, 16);
    if (page_type == static_cast<uint16_t>(mysql_recovery::PageType::SDI)) {
        return mysql_recovery::RowFormat::DYNAMIC;
    }

    // Default based on version detection
    auto version = infer_version(page_data, page_size);
    switch (version) {
        case mysql_recovery::MySQLVersion::MYSQL_56:
        case mysql_recovery::MySQLVersion::MYSQL_57:
            return mysql_recovery::RowFormat::COMPACT;
        case mysql_recovery::MySQLVersion::MYSQL_80:
        case mysql_recovery::MySQLVersion::MYSQL_84:
            return mysql_recovery::RowFormat::DYNAMIC;
        default:
            return mysql_recovery::RowFormat::COMPACT;
    }
}

mysql_recovery::MySQLVersion HeuristicInference::infer_version(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 100) {
        return mysql_recovery::MySQLVersion::UNKNOWN;
    }

    // Check for SDI page (8.0+ only)
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data, 16);
    if (page_type == static_cast<uint16_t>(mysql_recovery::PageType::SDI)) {
        return mysql_recovery::MySQLVersion::MYSQL_80;
    }

    // Check header version (8.0+ has header_version at offset 54)
    if (page_size >= 56) {
        uint16_t header_version = dbrecovery::innodb::read_le<uint16_t>(page_data, 54);
        if (header_version > 0) {
            return mysql_recovery::MySQLVersion::MYSQL_80;
        }
    }

    // Check for 8-byte file_flush_lsn (8.0+)
    if (page_size >= 32) {
        uint64_t flush_lsn = dbrecovery::innodb::read_le<uint64_t>(page_data, 24);
        if (flush_lsn > 0xFFFFFFFF) {
            return mysql_recovery::MySQLVersion::MYSQL_80;
        }
    }

    // No unique version evidence — fail closed. FSP flags and header heuristics
    // only provide lower bounds and can misclassify 8.0+ pages as older.
    return mysql_recovery::MySQLVersion::UNKNOWN;
}

std::vector<uint8_t> HeuristicInference::extract_column_metadata(const uint8_t* page_data, size_t page_size) {
    // Static helper - use a reasonable default
    std::vector<uint8_t> col_meta;

    if (page_size < 16384) {
        return col_meta;
    }

    // Try to detect number of columns from page structure
    uint16_t n_dir_slots = dbrecovery::innodb::read_le<uint16_t>(page_data, 38);
    if (n_dir_slots > 0 && n_dir_slots < 100) {
        // Estimate columns from directory slots
        uint32_t n_cols = std::min(static_cast<uint32_t>(n_dir_slots / 2), 50u);
        for (uint32_t i = 0; i < n_cols; i++) {
            col_meta.push_back(15);  // Default to VARCHAR
        }
    }

    return col_meta;
}

mysql_recovery::RowParser::RecordHeader HeuristicInference::parse_record_header(
    const uint8_t* record_data,
    mysql_recovery::RowFormat format
) {
    mysql_recovery::CompactRowParser parser;
    return parser.parse_record_header(record_data, format);
}

std::string HeuristicInference::generate_create_statement(const TableSchema& schema) {
    std::ostringstream oss;

    oss << "CREATE TABLE `" << schema.table_name << "` (\n";

    for (size_t i = 0; i < schema.columns.size(); i++) {
        const auto& col = schema.columns[i];

        oss << "  `" << col.name << "` " << col.type;

        if (!col.is_nullable) {
            oss << " NOT NULL";
        }

        if (col.is_auto_increment) {
            oss << " AUTO_INCREMENT";
        }

        if (col.is_primary_key) {
            oss << " PRIMARY KEY";
        }

        if (i < schema.columns.size() - 1) {
            oss << ",";
        }
        oss << "\n";
    }

    oss << ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\n";

    return oss.str();
}

std::vector<HeuristicInference::IndexInfo> HeuristicInference::get_indexes() const {
    return {};  // Not implemented in this version
}

void HeuristicInference::reset() {
    if (impl_) {
        // Reset implementation state
    }
}

// ==================== SimpleTypeInferer ====================

std::string SimpleTypeInferer::infer_type_from_value(const std::string& value) {
    if (value.empty() || value == "NULL") {
        return "VARCHAR";
    }

    if (is_integer(value)) {
        return "BIGINT";
    }

    if (is_float(value)) {
        return "DOUBLE";
    }

    if (is_date(value)) {
        return "DATETIME";
    }

    return "VARCHAR";
}

bool SimpleTypeInferer::is_integer(const std::string& value) {
    if (value.empty()) return false;

    size_t start = 0;
    if (value[0] == '-' || value[0] == '+') {
        start = 1;
    }

    if (start >= value.size()) return false;

    for (size_t i = start; i < value.size(); i++) {
        if (!std::isdigit(value[i])) {
            return false;
        }
    }

    return true;
}

bool SimpleTypeInferer::is_float(const std::string& value) {
    if (value.empty()) return false;

    size_t start = 0;
    if (value[0] == '-' || value[0] == '+') {
        start = 1;
    }

    if (start >= value.size()) return false;

    bool has_dot = false;
    bool has_digit = false;

    for (size_t i = start; i < value.size(); i++) {
        if (value[i] == '.') {
            if (has_dot) return false;
            has_dot = true;
        } else if (std::isdigit(value[i])) {
            has_digit = true;
        } else if (value[i] != 'e' && value[i] != 'E' && value[i] != '+' && value[i] != '-') {
            return false;
        }
    }

    return has_digit && has_dot;
}

bool SimpleTypeInferer::is_date(const std::string& value) {
    if (value.size() < 8) return false;

    // Check for YYYY-MM-DD or YYYY-MM-DD HH:MM:SS format
    bool has_dash = value.find('-') != std::string::npos;
    bool has_colon = value.find(':') != std::string::npos;
    bool has_space = value.find(' ') != std::string::npos;

    if (has_dash) {
        // Basic date format check
        if (value.size() >= 10 && value[4] == '-' && value[7] == '-') {
            return true;
        }
        if (has_space && has_colon && value.size() >= 19) {
            return true;
        }
    }

    return false;
}

bool SimpleTypeInferer::is_string(const std::string& value) {
    // If it doesn't match other types, it's a string
    return !is_integer(value) && !is_float(value) && !is_date(value);
}

int SimpleTypeInferer::get_type_priority(const std::string& type) {
    static const std::unordered_map<std::string, int> priorities = {
        {"BIGINT", 10},
        {"INT", 9},
        {"SMALLINT", 8},
        {"TINYINT", 7},
        {"DOUBLE", 6},
        {"FLOAT", 5},
        {"DATETIME", 4},
        {"TIMESTAMP", 4},
        {"DATE", 4},
        {"VARCHAR", 3},
        {"TEXT", 2},
        {"BLOB", 1},
    };

    auto it = priorities.find(type);
    return (it != priorities.end()) ? it->second : 0;
}

// ==================== InferenceStats ====================

void InferenceStats::start_timer() {
    auto now = std::chrono::high_resolution_clock::now();
    start_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
}

void InferenceStats::stop_timer() {
    auto now = std::chrono::high_resolution_clock::now();
    auto end_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    stats_.inference_time_ms += (end_time - start_time_ms_);
}

}  // namespace tools
}  // namespace dbrecovery
