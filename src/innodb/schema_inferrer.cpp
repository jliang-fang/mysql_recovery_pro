#include "dbrecovery/innodb/schema_inferrer.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <set>
#include <map>

namespace dbrecovery {
namespace innodb {

// ==================== SchemaInferrer::infer_from_raw_pages (v2.0) ====================

SchemaInferrer::Result SchemaInferrer::infer_from_raw_pages(
    const std::vector<std::pair<const uint8_t*, size_t>>& pages) {

    Result result;
    result.success = false;

    if (pages.empty()) {
        result.error_message = "No pages provided for inference";
        return result;
    }

    // Step 1: Analyze physical layout to detect column count
    auto layout = inference::PhysicalLayoutAnalyzer::analyze_multiple_pages(pages);
    if (layout.total_columns == 0) {
        result.error_message = "Could not detect column layout from pages";
        return result;
    }

    // Step 2: Extract raw column values
    auto column_values = inference::PhysicalLayoutAnalyzer::extract_column_values(
        pages, layout, 5000);

    if (column_values.empty()) {
        result.error_message = "Could not extract column values from pages";
        return result;
    }

    // Apply column count hint if set
    uint32_t effective_cols = static_cast<uint32_t>(column_values.size());
    if (column_count_hint_ > 0 && column_count_hint_ < effective_cols) {
        effective_cols = column_count_hint_;
    }

    // Step 3: Classify each column statistically
    inference::FeatureExtractor extractor;
    inference::StatisticalTypeClassifier classifier;

    InferredSchema schema;
    schema.table_name = table_name_hint_.empty() ? "recovered_table" : table_name_hint_;
    schema.row_format = layout.row_format;
    schema.page_size = 16384;
    schema.inference_method = "statistical_classifier_v2";
    schema.estimated_row_count = 0;

    double total_confidence = 0.0;
    std::vector<std::string> column_names;

    for (uint32_t i = 0; i < effective_cols && i < column_values.size(); i++) {
        // Skip system columns if we have enough: last 3 are DB_TRX_ID, DB_ROLL_PTR, DB_ROW_ID
        bool is_system_col = false;
        if (effective_cols > 3 && i >= effective_cols - 3) {
            is_system_col = true;
        }

        auto features = inference::FeatureExtractor::extract(column_values[i]);
        auto candidates = classifier.classify(features, 3);

        InferredColumn col;
        col.ordinal_position = i + 1;
        col.is_nullable = features.null_ratio > 0.01;

        if (is_system_col) {
            col.name = (i == effective_cols - 3) ? "DB_TRX_ID" :
                       (i == effective_cols - 2) ? "DB_ROLL_PTR" : "DB_ROW_ID";
            col.type_name = (i == effective_cols - 3) ? "BIGINT" : "VARBINARY";
            col.max_length = (i == effective_cols - 3) ? 6 : 7;
            col.is_string_type = false;
            col.is_integer_type = (i == effective_cols - 3);
            col.type_id = 7;
        } else if (!candidates.empty()) {
            const auto& best = candidates[0];
            col.type_name = best.type_name;
            col.type_id = best.innodb_type_id;
            col.max_length = static_cast<uint32_t>(features.len_max);
            col.is_string_type = (best.type_name.find("CHAR") != std::string::npos ||
                                 best.type_name.find("TEXT") != std::string::npos ||
                                 best.type_name == "ENUM" || best.type_name == "SET" ||
                                 best.type_name == "JSON");
            col.is_integer_type = (best.type_name.find("INT") != std::string::npos ||
                                  best.type_name == "YEAR" || best.type_name == "BIT");
            col.is_datetime_type = (best.type_name == "DATETIME" || best.type_name == "DATE" ||
                                   best.type_name == "TIME" || best.type_name == "TIMESTAMP");

            // Detect auto-increment: integer type with sequential values and no nulls
            if (col.is_integer_type && features.null_ratio < 0.01 &&
                features.integer_ratio > 0.9 && features.distinct_count > 10) {
                col.is_auto_increment = true;
            }
        } else {
            col.name = "col" + std::to_string(i + 1);
            col.type_name = "VARCHAR";
            col.max_length = std::max(static_cast<uint32_t>(features.len_max), 255u);
            col.is_string_type = true;
            col.type_id = 253;
        }

        // Infer column name
        if (!is_system_col) {
            col.name = PatternInferenceEngine::infer_column_name(i + 1, {});
            // Use feature-based naming
            if (col.is_integer_type && col.is_auto_increment) {
                col.name = "id";
            } else if (col.type_name == "VARCHAR" && features.has_at_ratio > 0.3) {
                col.name = "email";
            } else if (col.is_datetime_type) {
                col.name = (i == 0) ? "created_at" : "updated_at";
            }
        }

        // Detect PRIMARY KEY candidates: first auto-increment or first integer column
        if (!is_system_col && col.is_integer_type && schema.primary_key_columns.empty()) {
            col.is_primary_key = true;
            schema.primary_key_columns.push_back(col);
        }

        col.charset_id = col.is_string_type ? 255 : 0;
        schema.columns.push_back(col);
        total_confidence += candidates.empty() ? 0.3 : candidates[0].confidence;
    }

    // Calculate overall confidence
    uint32_t user_col_count = effective_cols > 3 ? effective_cols - 3 : effective_cols;
    if (user_col_count > 0) {
        schema.confidence = total_confidence / user_col_count;
    }
    schema.confidence = std::min(std::max(schema.confidence, 0.3), 1.0);

    schema_ = schema;
    confidence_ = schema.confidence;

    result.success = true;
    result.schema = schema_;

    // Add layout info as warning
    std::ostringstream layout_info;
    layout_info << "Detected " << layout.total_columns << " columns ("
                << (layout.total_columns > 3 ? layout.total_columns - 3 : layout.total_columns)
                << " user + " << (layout.total_columns > 3 ? 3 : 0) << " system), "
                << (layout.row_format == 0 ? "REDUNDANT" : "COMPACT") << " format";
    warnings_.push_back(layout_info.str());
    result.warnings = warnings_;

    return result;
}

// ==================== PatternInferenceEngine Implementation ====================

std::optional<InferredSchema> PatternInferenceEngine::infer_schema(
    const std::vector<std::string>& records,
    const std::string& table_name) {
    
    if (records.empty()) {
        return std::nullopt;
    }
    
    // First, try to infer column count from first record
    // For CSV format, count comma-separated values
    std::vector<std::string> first_record_fields;
    std::string first_line = records[0];
    
    // Parse CSV line
    std::stringstream ss(first_line);
    std::string field;
    while (std::getline(ss, field, ',')) {
        first_record_fields.push_back(field);
    }
    
    // If no commas, try space-separated
    if (first_record_fields.size() <= 1) {
        std::stringstream ss2(first_line);
        while (ss2 >> field) {
            first_record_fields.push_back(field);
        }
    }
    
    uint32_t col_count = static_cast<uint32_t>(first_record_fields.size());
    if (col_count == 0 || col_count > 100) {
        // Default to 4 columns for email pattern
        col_count = 4;
    }
    
    // Extract all values for each column
    std::vector<std::vector<std::string>> column_values(col_count);
    
    for (const auto& record : records) {
        std::vector<std::string> fields;
        std::stringstream ss(record);
        std::string field;
        while (std::getline(ss, field, ',')) {
            fields.push_back(field);
        }
        
        // Also try space-separated if CSV didn't work well
        if (fields.size() < col_count) {
            fields.clear();
            std::stringstream ss2(record);
            while (ss2 >> field) {
                fields.push_back(field);
            }
        }
        
        // Pad with empty strings if needed
        while (fields.size() < col_count) {
            fields.push_back("");
        }
        
        for (uint32_t i = 0; i < col_count; i++) {
            column_values[i].push_back(fields[i]);
        }
    }
    
    // Infer each column type
    InferredSchema schema;
    schema.table_name = table_name;
    schema.row_format = 0;  // Default to COMPACT
    schema.page_size = 16384;
    schema.confidence = 0.0;
    schema.estimated_row_count = records.size();
    schema.inference_method = "generic";
    
    bool found_email_pattern = false;
    bool found_user_pattern = false;
    
    for (uint32_t i = 0; i < col_count; i++) {
        auto col = infer_column_type(column_values[i], i + 1);
        schema.columns.push_back(col);
        
        // Check for email pattern hints
        bool is_email = false;
        bool is_user = false;
        
        for (const auto& val : column_values[i]) {
            if (email_detector_.is_email(val)) {
                is_email = true;
            }
            if (email_detector_.is_user_pattern(val)) {
                is_user = true;
            }
        }
        
        if (is_email) {
            found_email_pattern = true;
            schema.columns[i].is_string_type = true;  // Email is always string
        }
        if (is_user) {
            found_user_pattern = true;
            schema.columns[i].is_integer_type = true;  // User ID is numeric
        }
    }
    
    // Build confidence based on pattern matches
    schema.confidence = 0.6;  // Base confidence
    
    if (found_email_pattern) {
        schema.confidence += 0.2;
        schema.inference_method = "email_pattern";
    }
    if (found_user_pattern) {
        schema.confidence += 0.1;
    }
    
    // Determine primary key column (numeric)
    for (auto& col : schema.columns) {
        if (col.is_integer_type) {
            col.is_primary_key = true;
            schema.primary_key_columns.push_back(col);
            break;
        }
    }
    
    // If no obvious PK, use first column
    if (schema.primary_key_columns.empty() && !schema.columns.empty()) {
        schema.columns[0].is_primary_key = true;
        schema.primary_key_columns.push_back(schema.columns[0]);
    }
    
    schema.confidence = std::min(schema.confidence, 1.0);
    
    return schema;
}

InferredColumn PatternInferenceEngine::infer_column_type(
    const std::vector<std::string>& values,
    uint32_t ordinal) {
    
    InferredColumn col;
    col.ordinal_position = ordinal;
    
    if (values.empty()) {
        // Default empty column
        col.name = "col" + std::to_string(ordinal);
        col.type_name = "VARCHAR";
        col.max_length = 255;
        return col;
    }
    
    // Skip null/empty values for analysis
    std::vector<std::string> non_empty_values;
    for (const auto& v : values) {
        if (!v.empty() && v != "NULL") {
            non_empty_values.push_back(v);
        }
    }
    
    if (non_empty_values.empty()) {
        col.name = "col" + std::to_string(ordinal);
        col.type_name = "VARCHAR";
        col.max_length = 255;
        return col;
    }
    
    // Analyze patterns in this column using a single instance
    EmailPatternDetector email_detector;
    DateTimePatternDetector datetime_detector;
    
    int email_count = 0;
    int user_count = 0;
    int name_count = 0;
    int numeric_count = 0;
    int datetime_count = 0;
    int date_count = 0;
    
    uint32_t min_len = ~0U;
    uint32_t max_len = 0;
    uint64_t numeric_max = 0;
    int64_t numeric_min = 0;
    bool has_signed = false;
    
    for (const auto& val : non_empty_values) {
        min_len = std::min(min_len, static_cast<uint32_t>(val.size()));
        max_len = std::max(max_len, static_cast<uint32_t>(val.size()));
        
        // Analyze field
        bool is_email = email_detector.is_email(val);
        bool is_user = email_detector.is_user_pattern(val);
        bool is_name = email_detector.is_name_pattern(val);
        
        bool is_numeric = true;
        bool has_minus = false;
        for (size_t i = 0; i < val.size(); i++) {
            if (i == 0 && val[i] == '-') {
                has_minus = true;
            } else if (!std::isdigit(val[i])) {
                is_numeric = false;
                break;
            }
        }
        
        if (is_numeric) {
            try {
                uint64_t uval = std::stoull(val);
                numeric_max = std::max(numeric_max, uval);
                if (numeric_min == 0 || -static_cast<int64_t>(numeric_max) < numeric_min) {
                    numeric_min = -static_cast<int64_t>(numeric_max);
                    has_signed = uval > numeric_max;
                }
            } catch (...) {
                is_numeric = false;
            }
        }
        
        bool is_dt = datetime_detector.is_datetime(val);
        bool is_date = datetime_detector.is_date(val);
        
        if (is_email) email_count++;
        if (is_user) user_count++;
        if (is_name) name_count++;
        if (is_numeric) numeric_count++;
        if (is_dt) datetime_count++;
        if (is_date) date_count++;
    }
    
    // Calculate pattern confidence
    double email_conf = static_cast<double>(email_count) / non_empty_values.size();
    double user_conf = static_cast<double>(user_count) / non_empty_values.size();
    double name_conf = static_cast<double>(name_count) / non_empty_values.size();
    double numeric_conf = static_cast<double>(numeric_count) / non_empty_values.size();
    double datetime_conf = static_cast<double>(datetime_count) / non_empty_values.size();
    double date_conf = static_cast<double>(date_count) / non_empty_values.size();
    
    // Determine column name
    if (email_conf > 0.3) {
        col.name = "email";
        col.type_name = "VARCHAR";
        col.max_length = 255;
        col.is_string_type = true;
    } else if (user_conf > 0.3) {
        col.name = "user_id";
        col.type_name = "BIGINT";
        col.max_length = 20;
        col.is_integer_type = true;
        col.is_string_type = false;
    } else if (name_conf > 0.3) {
        col.name = "name";
        col.type_name = "VARCHAR";
        col.max_length = std::max(max_len, static_cast<uint32_t>(100));
        col.is_string_type = true;
    } else if (datetime_conf > 0.3) {
        col.name = "created_at";
        col.type_name = "DATETIME";
        col.max_length = 19;
        col.is_datetime_type = true;
        col.is_string_type = false;
    } else if (date_conf > 0.3) {
        col.name = "created_date";
        col.type_name = "DATE";
        col.max_length = 10;
        col.is_datetime_type = true;
        col.is_string_type = false;
    } else if (numeric_conf > 0.5) {
        col.name = "id";
        col.type_name = "BIGINT";
        col.max_length = 20;
        col.is_integer_type = true;
        col.is_string_type = false;
    } else if (max_len <= 255) {
        col.name = "column_" + std::to_string(ordinal);
        col.type_name = "VARCHAR";
        col.max_length = std::max(max_len, static_cast<uint32_t>(50));
        col.is_string_type = true;
    } else if (max_len <= 65535) {
        col.name = "column_" + std::to_string(ordinal);
        col.type_name = "VARCHAR";
        col.max_length = 65535;
        col.is_string_type = true;
    } else {
        col.name = "column_" + std::to_string(ordinal);
        col.type_name = "TEXT";
        col.max_length = 65535;
        col.is_string_type = true;
    }
    
    // Set NULLability based on data
    col.is_nullable = static_cast<double>(non_empty_values.size()) / values.size() < 0.95;
    
    // Set charset (default utf8mb4)
    col.charset_id = 255;
    
    return col;
}

std::string PatternInferenceEngine::infer_column_name(uint32_t ordinal, const std::vector<std::string>& values) {
    // Common column name patterns
    static const char* common_names[] = {
        "id", "user_id", "email", "name", "username", "title", "description",
        "created_at", "updated_at", "created_date", "updated_date", "status",
        "type", "category", "content", "message", "value", "data", "json"
    };
    
    if (ordinal < sizeof(common_names) / sizeof(common_names[0])) {
        return common_names[ordinal];
    }
    
    return "column_" + std::to_string(ordinal);
}

// ==================== FrmParser Implementation ====================

std::optional<InferredSchema> FrmParser::parse_frm(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    
    // Read file into buffer
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    if (file_size < 100) {
        return std::nullopt;
    }
    
    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    
    // Parse minimal header
    // Offset 0-1: frm_type (0x01fe for table)
    uint16_t frm_type = buffer[0] | (buffer[1] << 8);
    if (frm_type != 0x01fe) {
        return std::nullopt;
    }
    
    // Offset 2: frm_version
    uint8_t frm_version = buffer[2];
    if (frm_version < 9) {
        return std::nullopt;
    }
    
    // Offset 3: legacy_db_type (12 = InnoDB)
    uint8_t legacy_db_type = buffer[3];
    if (legacy_db_type != 12) {
        return std::nullopt;
    }
    
    // Offset 4-7: length
    uint32_t header_length = buffer[4] | (buffer[5] << 8) | 
                             (buffer[6] << 16) | (buffer[7] << 24);
    
    // Offset 12-13: rec_length (record length)
    uint16_t rec_length = buffer[12] | (buffer[13] << 8);
    
    // Offset 24-27: mysql_version_id
    uint32_t mysql_version = buffer[24] | (buffer[25] << 8) | 
                             (buffer[26] << 16) | (buffer[27] << 24);
    
    // Offset 28-31: extra_size
    uint32_t extra_size = buffer[28] | (buffer[29] << 8) | 
                          (buffer[30] << 16) | (buffer[31] << 24);
    
    // Row format detection
    uint32_t row_format = 0;  // Default COMPACT
    if (mysql_version >= 50700) {
        // Offset 41: row_type
        uint8_t row_type = buffer[41];
        // 0 = DEFAULT, 1 = REDUNDANT, 2 = COMPACT, 3 = COMPRESSED, 4 = DYNAMIC
        if (row_type >= 2) {
            row_format = row_type - 2;  // 0=COMPACT, 1=DYNAMIC
        }
    }
    
    InferredSchema schema;
    schema.table_name = extract_table_name(file_path);
    schema.row_format = row_format;
    schema.page_size = 16384;
    schema.confidence = 0.5;  // Lower confidence for binary-parsed .frm
    schema.inference_method = "frm_parser";

    // Try to locate and parse the column definition area
    // MySQL .frm format reference:
    //   - Offset 0-1: magic (0xFE01 or 0x01FE)
    //   - Offset 2: version
    //   - Offset 3: legacy_db_type
    //   - Offset 6-7: key_info_length
    //   - Offset 8-9: db_create_pack
    //   - Offset 10-11: db_pack_length (record pack length)
    //   - Offset 12-13: rec_length (unused)
    //   - Offset 14-15: db_type_offset
    //   - Offset 28331+: column definitions (in MySQL 5.6/5.7)
    //
    // The column definition section is after the key info and default values.
    // In MySQL 5.6+, columns start at offset: 64 + key_info_length + extra_section

    // Read key_info_length from offset 6-7
    uint16_t key_info_length = 0;
    if (buffer.size() > 8) {
        key_info_length = buffer[6] | (buffer[7] << 8);
    }

    // Read column count from offset 14-15 (for MySQL 5.6+)
    // Actually, the number of columns is at offset 25-28 as a 4-byte LE value
    // But it's simpler to calculate from rec_length and individual field sizes

    // The column definitions section starts at a variable offset.
    // For MySQL 5.6+, the layout after the 64-byte header is:
    //   - key_info (key_info_length bytes)
    //   - default values section (rec_length bytes, one row of defaults)
    //   - then: column names and types
    //
    // However, the exact layout varies by MySQL version.
    // A practical approach: scan for the column definition section

    // In MySQL 5.6/5.7 .frm files, column definitions start at approximately
    // offset 64 + key_info_length. Each column has a type byte followed by
    // metadata, and column names are stored as null-terminated strings later.

    // For a reasonable parser, we'll:
    // 1. Skip past key info and default values
    // 2. Scan for readable column names (null-terminated ASCII strings)
    // 3. Try to associate type bytes with column names

    size_t col_defs_offset = 64 + key_info_length;
    if (col_defs_offset >= buffer.size()) {
        col_defs_offset = 64;  // Fallback
    }

    // Try to find column names by scanning for sequences of printable ASCII
    // followed by null bytes. Column names in .frm are null-terminated strings.
    std::vector<std::string> col_names;
    std::vector<uint8_t> col_types;

    // Scan the buffer from col_defs_offset looking for column definitions
    // Each column in MySQL .frm has a type byte first, then varying metadata
    // Column names are stored in a separate section after the type bytes
    //
    // MySQL type constants:
    //   1 = BOOL/TINYINT, 2 = SMALLINT, 3 = INT, 4 = FLOAT,
    //   5 = DOUBLE, 6 = NULL, 7 = TIMESTAMP, 8 = BIGINT,
    //   9 = MEDIUMINT, 10 = DATE, 11 = TIME, 12 = DATETIME,
    //   13 = YEAR, 16 = BIT, 245 = JSON, 246 = DECIMAL/NEWDECIMAL,
    //   247 = ENUM, 248 = SET, 249 = TINY_BLOB, 250 = MEDIUM_BLOB,
    //   251 = LONG_BLOB, 252 = BLOB, 253 = VAR_STRING, 254 = STRING, 255 = GEOMETRY

    // Read column type bytes starting from col_defs_offset
    // Each column entry in the type section has: type (1 byte), then metadata
    // The metadata size depends on the type

    // First pass: try to find and parse column type bytes
    size_t pos = col_defs_offset;
    while (pos < buffer.size() - 1 && col_types.size() < 200) {
        uint8_t type_byte = buffer[pos];

        // Check if this looks like a valid MySQL column type
        bool is_valid_type = (type_byte == 1 || type_byte == 2 || type_byte == 3 ||
                              type_byte == 4 || type_byte == 5 || type_byte == 7 ||
                              type_byte == 8 || type_byte == 9 || type_byte == 10 ||
                              type_byte == 11 || type_byte == 12 || type_byte == 13 ||
                              type_byte == 16 || type_byte == 245 || type_byte == 246 ||
                              type_byte == 247 || type_byte == 248 || type_byte == 249 ||
                              type_byte == 250 || type_byte == 251 || type_byte == 252 ||
                              type_byte == 253 || type_byte == 254 || type_byte == 255);

        if (!is_valid_type) {
            pos++;
            continue;
        }

        // Found a valid type byte — record it and skip metadata
        col_types.push_back(type_byte);

        // Skip metadata bytes depending on type
        // Variable-length types (VARCHAR, BLOB, etc.) have 1-2 bytes of length metadata
        // Numeric types have 0-1 bytes of metadata
        if (type_byte == 253 || type_byte == 254) {
            // VAR_STRING / STRING: 1 or 2 bytes of length metadata
            pos += 2;  // Skip type byte + 1 byte metadata
        } else if (type_byte >= 249 && type_byte <= 252) {
            // BLOB types: 1 byte metadata
            pos += 2;
        } else if (type_byte == 246) {
            // DECIMAL: 2 bytes metadata (precision, scale)
            pos += 3;
        } else if (type_byte == 247 || type_byte == 248) {
            // ENUM/SET: variable metadata
            pos += 2;
        } else {
            // Numeric and date types: minimal metadata
            pos += 1;  // Just the type byte
        }

        // Safety: stop if we're reading too many columns or going out of bounds
        if (pos >= buffer.size() - 4) break;
    }

    // Second pass: find column names
    // Column names are typically stored as null-terminated strings
    // after the type definition section.
    // Scan from a point after the type definitions for readable strings.
    size_t names_search_start = col_defs_offset + col_types.size() * 3;
    if (names_search_start > buffer.size() - 10) {
        names_search_start = col_defs_offset + 64;
    }

    pos = names_search_start;
    while (pos < buffer.size() - 2 && col_names.size() < col_types.size() + 10) {
        // Look for sequences of printable ASCII characters (2+ chars) followed by \0
        if (buffer[pos] >= 32 && buffer[pos] < 127 && buffer[pos] != ' ') {
            size_t name_start = pos;
            size_t name_end = pos;
            while (name_end < buffer.size() && buffer[name_end] >= 32 && buffer[name_end] < 127) {
                name_end++;
            }
            // Must be at least 2 characters and followed by a null or non-printable
            if (name_end - name_start >= 2 && name_end < buffer.size() &&
                (buffer[name_end] == 0 || buffer[name_end] < 32)) {
                std::string name(reinterpret_cast<const char*>(&buffer[name_start]),
                                name_end - name_start);
                // Filter out obviously non-column-name strings
                bool is_valid_name = true;
                for (char c : name) {
                    if (c == ' ' && name.find(' ') == 0) { is_valid_name = false; break; }
                }
                if (is_valid_name && name.size() <= 64) {
                    col_names.push_back(name);
                }
                pos = name_end + 1;
                continue;
            }
        }
        pos++;
    }

    // Build schema from parsed data
    size_t num_cols = std::max(col_types.size(), col_names.size());
    if (num_cols == 0) {
        // Parsing failed — fall back to PatternInferenceEngine
        return std::nullopt;
    }

    for (size_t i = 0; i < num_cols; ++i) {
        InferredColumn col;
        col.ordinal_position = static_cast<uint32_t>(i) + 1;

        // Set column name
        if (i < col_names.size()) {
            col.name = col_names[i];
        } else {
            col.name = "col" + std::to_string(i + 1);
        }

        // Set column type from type byte
        if (i < col_types.size()) {
            uint8_t type_byte = col_types[i];
            switch (type_byte) {
                case 1:  // TINYINT/BOOL
                    col.type_name = "TINYINT";
                    col.max_length = 4;
                    col.is_integer_type = true;
                    col.is_string_type = false;
                    break;
                case 2:  // SMALLINT
                    col.type_name = "SMALLINT";
                    col.max_length = 6;
                    col.is_integer_type = true;
                    col.is_string_type = false;
                    break;
                case 3:  // INT
                    col.type_name = "INT";
                    col.max_length = 11;
                    col.is_integer_type = true;
                    col.is_string_type = false;
                    break;
                case 4:  // FLOAT
                    col.type_name = "FLOAT";
                    col.max_length = 12;
                    col.is_string_type = false;
                    break;
                case 5:  // DOUBLE
                    col.type_name = "DOUBLE";
                    col.max_length = 22;
                    col.is_string_type = false;
                    break;
                case 7:  // TIMESTAMP
                    col.type_name = "TIMESTAMP";
                    col.max_length = 19;
                    col.is_datetime_type = true;
                    col.is_string_type = false;
                    break;
                case 8:  // BIGINT
                    col.type_name = "BIGINT";
                    col.max_length = 20;
                    col.is_integer_type = true;
                    col.is_string_type = false;
                    break;
                case 9:  // MEDIUMINT
                    col.type_name = "MEDIUMINT";
                    col.max_length = 9;
                    col.is_integer_type = true;
                    col.is_string_type = false;
                    break;
                case 10: // DATE
                    col.type_name = "DATE";
                    col.max_length = 10;
                    col.is_datetime_type = true;
                    col.is_string_type = false;
                    break;
                case 11: // TIME
                    col.type_name = "TIME";
                    col.max_length = 10;
                    col.is_string_type = false;
                    break;
                case 12: // DATETIME
                    col.type_name = "DATETIME";
                    col.max_length = 19;
                    col.is_datetime_type = true;
                    col.is_string_type = false;
                    break;
                case 13: // YEAR
                    col.type_name = "YEAR";
                    col.max_length = 4;
                    col.is_integer_type = true;
                    col.is_string_type = false;
                    break;
                case 16: // BIT
                    col.type_name = "BIT";
                    col.max_length = 8;
                    col.is_string_type = false;
                    break;
                case 246: // DECIMAL/NEWDECIMAL
                    col.type_name = "DECIMAL";
                    col.max_length = 20;
                    col.is_string_type = false;
                    break;
                case 247: // ENUM
                    col.type_name = "ENUM";
                    col.max_length = 255;
                    col.is_string_type = true;
                    break;
                case 248: // SET
                    col.type_name = "SET";
                    col.max_length = 255;
                    col.is_string_type = true;
                    break;
                case 249: // TINY_BLOB
                case 250: // MEDIUM_BLOB
                case 251: // LONG_BLOB
                case 252: // BLOB
                    col.type_name = "BLOB";
                    col.max_length = 65535;
                    break;
                case 253: // VAR_STRING (VARCHAR)
                    col.type_name = "VARCHAR";
                    col.max_length = 255;
                    col.is_string_type = true;
                    break;
                case 254: // STRING (CHAR)
                    col.type_name = "CHAR";
                    col.max_length = 255;
                    col.is_string_type = true;
                    break;
                case 245: // JSON
                    col.type_name = "JSON";
                    col.max_length = 65535;
                    col.is_string_type = true;
                    break;
                case 255: // GEOMETRY
                    col.type_name = "GEOMETRY";
                    col.max_length = 65535;
                    break;
                default:
                    col.type_name = "VARCHAR";
                    col.max_length = 255;
                    col.is_string_type = true;
                    break;
            }
            col.type_id = type_byte;
        } else {
            // Default to VARCHAR if no type byte
            col.type_name = "VARCHAR";
            col.max_length = 255;
            col.is_string_type = true;
        }

        col.is_nullable = true;  // Default nullable
        col.charset_id = 255;    // Default utf8mb4
        schema.columns.push_back(col);
    }

    // Mark the first integer column as primary key if none specified
    if (!schema.columns.empty()) {
        bool found_pk = false;
        for (auto& col : schema.columns) {
            if (col.is_integer_type && !found_pk) {
                col.is_primary_key = true;
                found_pk = true;
                schema.primary_key_columns.push_back(col);
                break;
            }
        }
        if (!found_pk) {
            schema.columns[0].is_primary_key = true;
            schema.primary_key_columns.push_back(schema.columns[0]);
        }
    }

    // If we parsed some columns, increase confidence
    if (!schema.columns.empty()) {
        schema.confidence = 0.7;
    }

    return schema;
}

std::string FrmParser::extract_table_name(const std::string& frm_path) {
    // Extract filename from path
    size_t pos = frm_path.find_last_of("/\\");
    std::string filename = (pos != std::string::npos) ? 
                           frm_path.substr(pos + 1) : frm_path;
    
    // Remove .frm extension
    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".frm") {
        filename = filename.substr(0, filename.size() - 4);
    }
    
    return filename;
}

// ==================== SchemaInferrer Implementation ====================

SchemaInferrer::SchemaInferrer()
    : column_count_hint_(0)
    , table_name_hint_("")
    , frm_path_("")
    , schema_()
    , confidence_(0.0)
    , warnings_()
{}

SchemaInferrer::~SchemaInferrer() = default;

void SchemaInferrer::set_column_count_hint(uint32_t count) {
    column_count_hint_ = count;
}

void SchemaInferrer::set_table_name(const std::string& name) {
    table_name_hint_ = name;
}

void SchemaInferrer::set_frm_path(const std::string& path) {
    frm_path_ = path;
}

SchemaInferrer::Result SchemaInferrer::infer_from_records(const std::vector<std::string>& records) {
    Result result;
    result.success = false;
    result.warnings = warnings_;
    
    if (records.empty()) {
        result.error_message = "No records provided for inference";
        return result;
    }
    
    // Try .frm file first if available
    if (!frm_path_.empty()) {
        auto frm_schema = frm_parser_.parse_frm(frm_path_);
        if (frm_schema) {
            schema_ = *frm_schema;
            schema_.confidence = std::min(schema_.confidence + 0.1, 1.0);
            schema_.inference_method = "frm_pattern_hybrid";
            
            // Refine with pattern analysis
            auto refined = pattern_engine_.infer_schema(records, table_name_hint_.empty() ? schema_.table_name : table_name_hint_);
            if (refined) {
                schema_ = *refined;
            }
            
            result.success = true;
            result.schema = schema_;
            result.warnings = warnings_;
            confidence_ = schema_.confidence;
            return result;
        }
    }
    
    // Fall back to pure pattern inference
    auto inferred = pattern_engine_.infer_schema(records, table_name_hint_);
    if (inferred) {
        schema_ = *inferred;
        result.success = true;
        result.schema = schema_;
        result.warnings = warnings_;
        confidence_ = schema_.confidence;
        return result;
    }
    
    result.error_message = "Failed to infer schema from records";
    return result;
}

SchemaInferrer::Result SchemaInferrer::infer_from_frm() {
    Result result;
    result.success = false;
    result.warnings = warnings_;
    
    if (frm_path_.empty()) {
        result.error_message = "No .frm file path provided";
        return result;
    }
    
    auto schema = frm_parser_.parse_frm(frm_path_);
    if (schema) {
        schema_ = *schema;
        result.success = true;
        result.schema = schema_;
        result.warnings = warnings_;
        confidence_ = schema_.confidence;
        return result;
    }
    
    result.error_message = "Failed to parse .frm file";
    return result;
}

} // namespace innodb
} // namespace dbrecovery
