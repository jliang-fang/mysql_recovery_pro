#ifndef DBRECOVERY_INNODB_SCHEMA_INFERRER_HPP
#define DBRECOVERY_INNODB_SCHEMA_INFERRER_HPP

/**
 * @file schema_inferrer.hpp
 * @brief Dynamic schema inference from corrupted IBD files
 *
 * This module automatically infers table schema from recovered records
 * without requiring .frm or SDI metadata. It works by analyzing data patterns
 * in the raw IBD file and inferring column types based on content analysis.
 *
 * Key features:
 * - Pattern-based column type inference (email, number, date, string)
 * - Automatic field count detection
 * - Support for custom formats (user_X, email_X, name_X patterns)
 * - Fallback to generic column names when patterns not found
 * - Compatible with MySQL 5.6/5.7/8.0
 */

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include "dbrecovery/catalog/schema_defs.hpp"
#include "dbrecovery/inference/statistical_classifier.hpp"

namespace dbrecovery {
namespace innodb {

// ==================== Column Type Patterns ====================

/**
 * @brief Pattern match result for column type inference
 */
struct PatternMatch {
    std::string type_name;          // e.g., "VARCHAR", "BIGINT", "DATETIME"
    uint32_t type_id;               // MySQL type ID
    uint32_t max_length;            // Maximum field length observed
    uint32_t min_length;            // Minimum field length observed
    bool is_nullable;               // Whether NULL values were observed
    size_t match_count;             // Number of records matching this pattern
    std::string sample_value;       // Sample value for debugging
};

/**
 * @brief Inferred column information from pattern analysis
 */
struct InferredColumn {
    uint32_t ordinal_position;
    std::string name;
    std::string type_name;
    uint32_t type_id;
    uint32_t max_length;
    bool is_nullable;
    bool is_primary_key;
    bool is_auto_increment;
    bool is_integer_type;
    bool is_string_type;
    bool is_datetime_type;
    uint32_t charset_id;
    
    // Default constructor
    InferredColumn()
        : ordinal_position(0)
        , type_id(0)
        , max_length(255)
        , is_nullable(true)
        , is_primary_key(false)
        , is_auto_increment(false)
        , is_integer_type(false)
        , is_string_type(true)
        , is_datetime_type(false)
        , charset_id(255) {}  // utf8mb4_0900_ai_ci
};

/**
 * @brief Inferred table schema from corrupted IBD file
 */
struct InferredSchema {
    std::string table_name;
    uint32_t row_format;  // 0=COMPACT, 1=DYNAMIC
    uint32_t page_size;
    std::vector<InferredColumn> columns;
    std::vector<InferredColumn> primary_key_columns;
    uint64_t estimated_row_count;
    double confidence;  // 0.0 to 1.0
    std::string inference_method;  // "email_pattern", "numeric_pattern", "generic"
    
    // Create a human-readable DDL
    std::string to_ddl() const {
        std::ostringstream oss;
        oss << "CREATE TABLE `" << table_name << "` (\n";
        
        for (size_t i = 0; i < columns.size(); i++) {
            const auto& col = columns[i];
            oss << "  `" << col.name << "` " << col.type_name;
            
            if (!col.is_string_type && col.max_length > 0) {
                oss << "(" << col.max_length << ")";
            }
            
            if (!col.is_nullable) {
                oss << " NOT NULL";
            }
            
            if (col.is_auto_increment) {
                oss << " AUTO_INCREMENT";
            }
            
            // Add comma if not last column
            if (i < columns.size() - 1) {
                oss << ",";
            }
            oss << "\n";
        }
        
        // Add PRIMARY KEY constraint if we have(pk columns)
        if (!primary_key_columns.empty()) {
            oss << ",\n  PRIMARY KEY (";
            for (size_t i = 0; i < primary_key_columns.size(); i++) {
                if (i > 0) oss << ", ";
                oss << "`" << primary_key_columns[i].name << "`";
            }
            oss << ")";
        }
        
        oss << "\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\n";
        return oss.str();
    }
};

// ==================== Email Pattern Detector ====================

/**
 * @brief Detect email patterns in recovered values
 */
class EmailPatternDetector {
public:
    EmailPatternDetector() {
        // Email pattern: something@something.something
        email_regex_ = std::regex(
            R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})",
            std::regex::optimize
        );
        
        // User pattern: user_XXXXX or userXXXX
        user_prefix_regex_ = std::regex(
            R"(^user[0-9]+|[a-zA-Z0-9_]+)$)",
            std::regex::optimize
        );
    }
    
    /**
     * @brief Check if value is an email address
     */
    bool is_email(const std::string& value) const {
        if (value.empty() || value.size() < 5) {
            return false;
        }
        
        // Check for @ symbol
        size_t at_pos = value.find('@');
        if (at_pos == std::string::npos || at_pos == 0) {
            return false;
        }
        
        // Check for dot after @
        size_t dot_pos = value.find('.', at_pos);
        if (dot_pos == std::string::npos || dot_pos == at_pos + 1) {
            return false;
        }
        
        // Check for valid TLD (2+ letters)
        std::string tld = value.substr(dot_pos + 1);
        if (tld.size() < 2) {
            return false;
        }
        
        for (char c : tld) {
            if (!std::isalpha(c)) {
                return false;
            }
        }
        
        return true;
    }
    
    /**
     * @brief Check if value is a user pattern (user_XXXXX)
     */
    bool is_user_pattern(const std::string& value) const {
        if (value.empty() || value.size() < 6) {
            return false;
        }
        
        // Check for "user_" prefix
        if (value.substr(0, 5) == "user_") {
            // Ensure rest is digits
            for (size_t i = 5; i < value.size(); i++) {
                if (!std::isdigit(value[i])) {
                    return false;
                }
            }
            return true;
        }
        
        // Check for "user" prefix without underscore (5.6 style)
        if (value.substr(0, 4) == "user") {
            for (size_t i = 4; i < value.size(); i++) {
                if (!std::isdigit(value[i])) {
                    return false;
                }
            }
            return true;
        }
        
        return false;
    }
    
    /**
     * @brief Check if value looks like a name (User_XXXXX)
     */
    bool is_name_pattern(const std::string& value) const {
        if (value.empty() || value.size() < 6) {
            return false;
        }
        
        // Check for "User_" prefix (capital U)
        if (value.substr(0, 5) == "User_") {
            // Ensure rest is digits or letters
            for (size_t i = 5; i < value.size(); i++) {
                if (!std::isalnum(value[i])) {
                    return false;
                }
            }
            return true;
        }
        
        return false;
    }
    
    /**
     * @brief Extract numeric value from pattern
     */
    std::optional<uint64_t> extract_user_id(const std::string& value) const {
        if (value.empty()) {
            return std::nullopt;
        }
        
        // Find first digit sequence
        for (size_t i = 0; i < value.size(); i++) {
            if (std::isdigit(value[i])) {
                uint64_t result = 0;
                while (i < value.size() && std::isdigit(value[i])) {
                    result = result * 10 + (value[i] - '0');
                    i++;
                }
                return result;
            }
        }
        
        return std::nullopt;
    }
    
    /**
     * @brief Analyze field for pattern hints
     */
    struct FieldHints {
        bool is_email;
        bool is_user_id;
        bool is_name;
        bool is_numeric;
        bool is_datetime;
        bool is_url;
        uint64_t numeric_value;
        time_t datetime_value;
        
        FieldHints()
            : is_email(false)
            , is_user_id(false)
            , is_name(false)
            , is_numeric(false)
            , is_datetime(false)
            , is_url(false)
            , numeric_value(0)
            , datetime_value(0) {}
    };
    
    FieldHints analyze_field(const std::string& value) const {
        FieldHints hints;
        
        if (value.empty()) {
            return hints;
        }
        
        // Check for email
        hints.is_email = is_email(value);
        
        // Check for user pattern
        hints.is_user_id = is_user_pattern(value);
        
        // Check for name pattern
        hints.is_name = is_name_pattern(value);
        
        // Check for numeric
        bool all_digits = true;
        bool has_minus = false;
        for (size_t i = 0; i < value.size(); i++) {
            if (i == 0 && value[i] == '-') {
                has_minus = true;
            } else if (!std::isdigit(value[i])) {
                all_digits = false;
                break;
            }
        }
        hints.is_numeric = all_digits && !value.empty();
        
        if (hints.is_numeric) {
            try {
                hints.numeric_value = std::stoull(value);
            } catch (...) {
                hints.is_numeric = false;
            }
        }
        
        // Check for URL
        if (value.find("http://") == 0 || value.find("https://") == 0) {
            hints.is_url = true;
        }
        
        return hints;
    }

private:
    std::regex email_regex_;
    std::regex user_prefix_regex_;
};

// ==================== DateTime Pattern Detector ====================

/**
 * @brief Detect datetime patterns in recovered values
 */
class DateTimePatternDetector {
public:
    DateTimePatternDetector() {
        // ISO datetime pattern: YYYY-MM-DD HH:MM:SS
        datetime_iso_regex_ = std::regex(
            R"(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})",
            std::regex::optimize
        );
        
        // Date-only pattern: YYYY-MM-DD
        date_regex_ = std::regex(
            R"(\d{4}-\d{2}-\d{2})",
            std::regex::optimize
        );
    }
    
    bool is_datetime(const std::string& value) const {
        return std::regex_match(value, datetime_iso_regex_);
    }
    
    bool is_date(const std::string& value) const {
        if (is_datetime(value)) {
            return false;  // Prefer datetime over date
        }
        return std::regex_match(value, date_regex_);
    }
    
    std::optional<time_t> parse_datetime(const std::string& value) const {
        if (value.size() < 19) {
            return std::nullopt;
        }
        
        // Format: YYYY-MM-DD HH:MM:SS
        int year, month, day, hour, minute, second;
        try {
            year = std::stoi(value.substr(0, 4));
            month = std::stoi(value.substr(5, 2));
            day = std::stoi(value.substr(8, 2));
            hour = std::stoi(value.substr(11, 2));
            minute = std::stoi(value.substr(14, 2));
            second = std::stoi(value.substr(17, 2));
        } catch (...) {
            return std::nullopt;
        }
        
        // Simple validation
        if (month < 1 || month > 12 || day < 1 || day > 31 ||
            hour > 23 || minute > 59 || second > 59) {
            return std::nullopt;
        }
        
        // Return a simple epoch-like value (not exact leap second handling)
        return static_cast<time_t>(year * 31536000LL + month * 2592000LL + day * 86400LL +
                                    hour * 3600LL + minute * 60LL + second);
    }

private:
    std::regex datetime_iso_regex_;
    std::regex date_regex_;
};

// ==================== Numeric Pattern Detector ====================

/**
 * @brief Detect numeric patterns in recovered values
 */
class NumericPatternDetector {
public:
    NumericPatternDetector() {}
    
    /**
     * @brief Determine if value is numeric and what type
     */
    struct NumericInfo {
        bool is_integer;
        bool is_signed;
        bool fits_tiny;
        bool fits_small;
        bool fits_medium;
        bool fits_int;
        bool fits_bigint;
        uint64_t max_value;
        int64_t min_value;
        uint32_t length;
        
        NumericInfo()
            : is_integer(false)
            , is_signed(false)
            , fits_tiny(false)
            , fits_small(false)
            , fits_medium(false)
            , fits_int(false)
            , fits_bigint(false)
            , max_value(0)
            , min_value(0)
            , length(0) {}
    };
    
    NumericInfo analyze_numeric(const std::string& value) const {
        NumericInfo info;
        
        if (value.empty()) {
            return info;
        }
        
        info.length = static_cast<uint32_t>(value.size());
        
        // Check if all digits
        bool all_digits = true;
        bool has_minus = false;
        for (size_t i = 0; i < value.size(); i++) {
            if (i == 0 && value[i] == '-') {
                has_minus = true;
            } else if (!std::isdigit(value[i])) {
                all_digits = false;
                break;
            }
        }
        
        if (!all_digits) {
            return info;
        }
        
        info.is_integer = true;
        info.is_signed = has_minus;
        
        // Parse value
        try {
            uint64_t uval = std::stoull(value);
            info.max_value = uval;
            if (has_minus) {
                info.min_value = -static_cast<int64_t>(uval);
            } else {
                info.min_value = 0;
            }
        } catch (...) {
            return info;
        }
        
        // Determine fit range
        info.fits_tiny = info.max_value <= 255 ||
                        (info.is_signed && info.max_value <= 127);
        info.fits_small = info.max_value <= 65535 ||
                         (info.is_signed && info.max_value <= 32767);
        info.fits_medium = info.max_value <= 16777215 ||
                          (info.is_signed && info.max_value <= 8388607);
        info.fits_int = info.max_value <= 4294967295ULL ||
                       (info.is_signed && info.max_value <= 2147483647ULL);
        info.fits_bigint = true;  // If fits smaller, also fits bigint
        
        return info;
    }
};

// ==================== PatternInferenceEngine ====================

/**
 * @brief Main pattern inference engine
 */
class PatternInferenceEngine {
public:
    PatternInferenceEngine() {}
    
    /**
     * @brief Infer schema from a set of recovered records
     * @param records Vector of recovered record strings
     * @param table_name Base table name
     * @return Inferred schema or nullopt
     */
    std::optional<InferredSchema> infer_schema(
        const std::vector<std::string>& records,
        const std::string& table_name = "recovered_table");
    
    /**
     * @brief Infer column type from pattern analysis
     */
    static InferredColumn infer_column_type(const std::vector<std::string>& values, uint32_t ordinal);
    
    /**
     * @brief Infer column name from pattern analysis
     */
    static std::string infer_column_name(uint32_t ordinal, const std::vector<std::string>& values);

private:
    EmailPatternDetector email_detector_;
    DateTimePatternDetector datetime_detector_;
    NumericPatternDetector numeric_detector_;
};

// ==================== .frm Parser (for schema from backup) ====================

/**
 * @brief Parse MySQL .frm file to extract schema
 */
class FrmParser {
public:
    FrmParser() {}
    
    /**
     * @brief Parse .frm file
     * @param file_path Path to .frm file
     * @return Inferred schema or nullopt if parsing failed
     */
    std::optional<InferredSchema> parse_frm(const std::string& file_path);
    
    /**
     * @brief Extract table name from .frm filename
     */
    static std::string extract_table_name(const std::string& frm_path);

private:
    // Minimal .frm header structure
    struct FrmHeader {
        uint16_t frm_type;
        uint8_t frm_version;
        uint8_t legacy_db_type;
        uint32_t length;
        uint16_t rec_length;
        uint16_t key_info_length;
        uint16_t mysql_version_id;
        uint32_t extra_size;
    };
};

// ==================== Schema Inferrer (Public API) ====================

/**
 * @brief Main schema inference interface
 */
class SchemaInferrer {
public:
    SchemaInferrer();
    ~SchemaInferrer();
    
    /**
     * @brief Set column count hint (useful when pattern detection is ambiguous)
     */
    void set_column_count_hint(uint32_t count);
    
    /**
     * @brief Set table name hint
     */
    void set_table_name(const std::string& name);
    
    /**
     * @brief Enable .frm file parsing
     */
    void set_frm_path(const std::string& path);
    
    /**
     * @brief Inference result
     */
    struct Result {
        bool success;
        InferredSchema schema;
        std::vector<std::string> warnings;
        std::string error_message;
    };
    
    /**
     * @brief Infer schema from recovered records
     * @param records Vector of CSV/JSON records
     * @return Inference result
     */
    Result infer_from_records(const std::vector<std::string>& records);
    
    /**
     * @brief Infer schema from .frm file only
     * @return Inference result
     */
    Result infer_from_frm();

    /**
     * @brief v2.0: Infer schema from raw InnoDB pages using statistical classification
     *
     * Uses PhysicalLayoutAnalyzer to detect column count from page layout,
     * extracts raw column values, then classifies each column's type using
     * StatisticalTypeClassifier with 20+ likelihood functions.
     *
     * @param pages Vector of (page_data, page_size) pairs from INDEX pages
     * @return Inference result with detailed type candidates per column
     */
    Result infer_from_raw_pages(
        const std::vector<std::pair<const uint8_t*, size_t>>& pages);
    
    /**
     * @brief Get inferred schema
     */
    const InferredSchema& get_schema() const { return schema_; }
    
    /**
     * @brief Get inference confidence
     */
    double get_confidence() const { return confidence_; }

private:
    uint32_t column_count_hint_;
    std::string table_name_hint_;
    std::string frm_path_;
    InferredSchema schema_;
    double confidence_;
    std::vector<std::string> warnings_;
    
    PatternInferenceEngine pattern_engine_;
    FrmParser frm_parser_;
};

// ==================== Inline Implementations ====================

} // namespace innodb
} // namespace dbrecovery

#endif // DBRECOVERY_INNODB_SCHEMA_INFERRER_HPP
