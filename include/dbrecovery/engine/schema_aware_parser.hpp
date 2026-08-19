/**
 * @file schema_aware_parser.hpp
 * @brief Schema-Aware Parser for MySQL 5.6 data recovery
 * 
 * Parses binary tuples from recovered IBD pages and maps them to SQL tuples
 * using schema templates or inferred structures.
 * 
 * Key features:
 * - Binary tuple to field mapping
 * - Schema template matching
 * - Dynamic schema inference
 * - SQL tuples generation
 */

#ifndef DBRECOVERY_ENGINE_SCHEMA_AWARE_PARSER_HPP
#define DBRECOVERY_ENGINE_SCHEMA_AWARE_PARSER_HPP

#include "dbrecovery/engine/raw_disk_engine.hpp"
#include "dbrecovery/engine/schema_template.hpp"
#include "dbrecovery/innodb/heuristic_parser.hpp"
#include "dbrecovery/innodb/binary_reader.hpp"
#include "dbrecovery/catalog/schema_defs.hpp"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <optional>

namespace dbrecovery {
namespace engine {

// ==================== Recovered Record ====================

/**
 * @brief Recovered record with schema-aware metadata
 */
struct RecoveredRecord {
    uint64_t page_offset;
    uint32_t page_number;
    uint32_t space_id;
    
    std::map<std::string, std::string> fields;
    std::vector<uint8_t> raw_data;
    
    // Metadata
    bool is_deleted;
    bool is_infimum;
    bool is_supremum;
    uint64_t transaction_id;
    uint64_t roll_ptr;
    
    // Schema mapping
    uint32_t field_count;
    std::vector<uint32_t> field_offsets;
    std::vector<uint32_t> field_lengths;
    
    // Verification
    bool checksum_valid;
    uint32_t crc32;
    
    // Confidence
    double pattern_confidence;  // Pattern matching confidence
    double schema_match_score;  // Schema template match score
};

// ==================== Schema Match Result ====================

/**
 * @brief Schema matching result
 */
struct SchemaMatchResult {
    bool matches;
    double confidence;
    std::vector<std::string> matched_columns;
    std::vector<std::string> unmatched_columns;
    std::vector<std::string> extra_columns;
    
    // Detailed matching info
    struct ColumnMatch {
        std::string column_name;
        bool matches;
        std::string matched_type;
        std::string expected_type;
        double type_confidence;
    };
    
    std::vector<ColumnMatch> column_matches;
};

// SchemaTemplate is defined in schema_template.hpp (included above)

// ==================== TupleParser ====================

/**
 * @brief Binary tuple parser with schema awareness
 * 
 * Parses InnoDB record tuples and maps them to schema fields.
 */
class TupleParser {
public:
    /// @brief Parse configuration
    struct Config {
        SchemaTemplate schema;
        bool validate_checksums = true;
        bool strict_mode = false;      // Reject unknown formats
        bool use_heuristic = true;     // Fallback to heuristic parsing
        double confidence_threshold = 0.5;
    };

    /**
     * @brief Construct a new TupleParser
     *
     * @param config Parsing configuration
     */
    explicit TupleParser(const Config& config);

    /**
     * @brief Set MySQL version for version-dependent layout decisions
     *
     * Controls DB_ROLL_PTR width: 6 bytes for 5.6/5.7, 7 bytes for 8.0+.
     * Default (0 / unknown) uses 7 bytes as conservative choice.
     *
     * @param version MySQL version number (e.g. 50600, 50700, 80000)
     */
    void set_mysql_version(uint32_t version) { mysql_version_ = version; }

    /// @brief Destroy the TupleParser
    ~TupleParser();

    /**
     * @brief Parse a single record
     * 
     * @param record_data Raw record data
     * @param record_header Record header information
     * @return Parsed record
     */
    RecoveredRecord parse_record(
        const std::vector<uint8_t>& record_data,
        const innodb::Record& record_header);

    /**
     * @brief Parse records from page data
     * 
     * @param page_data Page data
     * @param schema Schema template
     * @param callback Callback for each parsed record
     * @return Number of records parsed
     */
    uint32_t parse_page(
        const std::vector<uint8_t>& page_data,
        const SchemaTemplate& schema,
        std::function<void(const RecoveredRecord&)> callback);

    /**
     * @brief Match schema to recovered data
     * 
     * @param records Recovered records
     * @param template_ Schema template
     * @return Match result
     */
    static SchemaMatchResult match_schema(
        const std::vector<RecoveredRecord>& records,
        const SchemaTemplate& template_);

    /**
     * @brief Infer column type from data
     * 
     * @param values Column values
     * @return Inferred column type
     */
    static SchemaTemplate::Column infer_column_type(
        const std::vector<std::string>& values);

    /**
     * @brief Generate SQL INSERT statement
     * 
     * @param record Recovered record
     * @param template_ Schema template
     * @return SQL INSERT statement
     */
    static std::string generate_sql_insert(
        const RecoveredRecord& record,
        const SchemaTemplate& template_);

    /**
     * @brief Generate SQL DDL for table
     * 
     * @param template_ Schema template
     * @return SQL DDL statement
     */
    static std::string generate_sql_ddl(const SchemaTemplate& template_);

    /**
     * @brief Convert value to SQL literal
     * 
     * @param value Field value
     * @param column Column definition
     * @return SQL literal
     */
    static std::string to_sql_literal(
        const std::string& value,
        const SchemaTemplate::Column& column);

    /**
     * @brief Escape string for SQL
     * 
     * @param str String to escape
     * @return Escaped string
     */
    static std::string escape_sql_string(const std::string& str);

    /**
     * @brief Parse record header from data
     * 
     * @param data Data buffer
     * @param offset Offset to header
     * @return Parsed header
     */
    static std::optional<innodb::Record> parse_record_header(
        const uint8_t* data,
        uint32_t offset);

    /**
     * @brief Get schema match confidence
     * 
     * @param records Recovered records
     * @param template_ Schema template
     * @return Confidence score (0-1)
     */
    static double get_schema_confidence(
        const std::vector<RecoveredRecord>& records,
        const SchemaTemplate& template_);

private:
    Config config_;
    uint32_t mysql_version_ = 0;  // Not set → conservative 7-byte DB_ROLL_PTR

    /// @brief DB_ROLL_PTR width in bytes for resolved version
    uint32_t roll_ptr_bytes() const {
        return (mysql_version_ >= 80000 || mysql_version_ == 0) ? 7 : 6;
    }

    /**
     * @brief Get fixed-size field byte length for a column type
     *
     * @param column Column definition
     * @return Byte size of the field in InnoDB storage
     */
    uint32_t get_fixed_field_size(const SchemaTemplate::Column& column) const;

    /**
     * @brief Decode field data from raw bytes with known size
     *
     * @param data Data buffer
     * @param offset Current offset (modified)
     * @param column Column definition
     * @param data_size Number of bytes available for this field
     * @return Decoded value string
     */
    std::string decode_field_data(
        const uint8_t* data,
        uint32_t& offset,
        const SchemaTemplate::Column& column,
        uint32_t data_size) const;

    /**
     * @brief Decode PK field in InnoDB internal format (big-endian, sign bit inverted)
     *
     * @param data Data buffer pointing to field start
     * @param data_size Number of bytes for this field
     * @param column Column definition
     * @return Decoded value string
     */
    std::string decode_innodb_pk_field(
        const uint8_t* data,
        uint32_t data_size,
        const SchemaTemplate::Column& column) const;

    /**
     * @brief Parse field from record data
     *
     * @param data Data buffer
     * @param offset Current offset
     * @param column Column definition
     * @param value Output value
     * @return New offset
     */
    uint32_t parse_field(
        const uint8_t* data,
        uint32_t offset,
        const SchemaTemplate::Column& column,
        std::string& value);

    /**
     * @brief Detect column pattern
     *
     * @param values Column values
     * @return Detected pattern
     */
    SchemaTemplate::Column detect_column_pattern(
        const std::vector<std::string>& values);
};

// ==================== SchemaTemplateMatchEngine ====================

/**
 * @brief Schema template matching engine
 * 
 * Matches recovered data against schema templates for verification.
 */
class SchemaTemplateMatchEngine {
public:
    /// @brief Match configuration
    struct Config {
        double min_confidence = 0.5;
        bool strict_type_check = true;
        bool allow_partial_match = true;
    };

    /**
     * @brief Match records against template
     * 
     * @param records Recovered records
     * @param template_ Schema template
     * @param config Match configuration
     * @return Match result
     */
    SchemaMatchResult match(
        const std::vector<RecoveredRecord>& records,
        const SchemaTemplate& template_,
        const Config& config);

    /**
     * @brief Find best matching template
     * 
     * @param records Recovered records
     * @param templates Schema templates
     * @param config Match configuration
     * @return Best matching template (or null)
     */
    std::optional<SchemaTemplate> find_best_template(
        const std::vector<RecoveredRecord>& records,
        const std::vector<SchemaTemplate>& templates,
        const Config& config);

    /**
     * @brief Calculate match score
     * 
     * @param records Recovered records
     * @param template_ Schema template
     * @return Match score (0-1)
     */
    double calculate_score(
        const std::vector<RecoveredRecord>& records,
        const SchemaTemplate& template_);

private:
    /**
     * @brief Check column type compatibility
     * 
     * @param actual_type Actual type from data
     * @param expected_type Expected type from template
     * @return True if compatible
     */
    static bool is_type_compatible(
        const std::string& actual_type,
        const std::string& expected_type);

    /**
     * @brief Calculate pattern confidence
     * 
     * @param value Field value
     * @param column Column definition
     * @return Confidence (0-1)
     */
    double calculate_pattern_confidence(
        const std::string& value,
        const SchemaTemplate::Column& column);
};

// ==================== SqlTupleGenerator ====================

/**
 * @brief SQL tuples generator
 * 
 * Generates SQL tuples from recovered records.
 */
class SqlTupleGenerator {
public:
    /// @brieftuple generation configuration
    struct Config {
        std::string table_prefix = "";      // e.g., "recovered."
        bool include_create_table = true;
        bool batch_insert = true;
        uint32_t batch_size = 100;
        bool escape_strings = true;
        std::string line_ending = "\n";
    };

    /**
     * @brief Construct a new SqlTupleGenerator
     * 
     * @param configtuple generation configuration
     */
    explicit SqlTupleGenerator(const Config& config);

    /**
     * @brief Generate tuple from records
     * 
     * @param records Recovered records
     * @param template_ Schema template
     * @return SQL tuples string
     */
    std::string generate(
        const std::vector<RecoveredRecord>& records,
        const SchemaTemplate& template_);

    /**
     * @brief Generate CREATE TABLE statement
     * 
     * @param template_ Schema template
     * @return SQL DDL statement
     */
    std::string generate_create_table(const SchemaTemplate& template_);

    /**
     * @brief Generate INSERT statements
     * 
     * @param records Recovered records
     * @param template_ Schema template
     * @return SQL INSERT statements
     */
    std::string generate_inserts(
        const std::vector<RecoveredRecord>& records,
        const SchemaTemplate& template_);

    /**
     * @brief Generate single INSERT statement
     * 
     * @param record Recovered record
     * @param template_ Schema template
     * @return SQL INSERT statement
     */
    std::string generate_insert(
        const RecoveredRecord& record,
        const SchemaTemplate& template_);

    /**
     * @brief Set the schema template
     * 
     * @param template_ Schema template
     */
    void set_schema_template(const SchemaTemplate& template_) {
        schema_template_ = template_;
    }

private:
    Config config_;
    SchemaTemplate schema_template_;
};

// ==================== SchemaTemplateInference ====================

/**
 * @brief Schema template inference from data
 * 
 * Automatically infers schema from recovered data patterns.
 */
class SchemaTemplateInference {
public:
    /// @brief Inference result
    struct Result {
        SchemaTemplate schema;
        double confidence;
        std::vector<std::string> warnings;
        
        // Inference details
        struct Details {
            uint32_t columns_inferred;
            uint32_t columns_matched;
            uint32_t pattern_detections;
            uint32_t type_validations;
        } details;
    };

    /**
     * @brief Infer schema from records
     * 
     * @param records Recovered records
     * @param table_name Expected table name
     * @return Inference result
     */
    static Result infer(
        const std::vector<RecoveredRecord>& records,
        const std::string& table_name = "recovered_table");

    /**
     * @brief Infer schema from CSV records
     * 
     * @param csv_records CSV records
     * @param table_name Expected table name
     * @return Inference result
     */
    static Result infer_from_csv(
        const std::vector<std::vector<std::string>>& csv_records,
        const std::string& table_name = "recovered_table");

    /**
     * @brief Infer column type
     * 
     * @param values Column values
     * @return Inferred column
     */
    static SchemaTemplate::Column infer_column(
        const std::vector<std::string>& values);

    /**
     * @brief Infer column name
     * 
     * @param ordinal Column ordinal
     * @param values Column values
     * @return Inferred name
     */
    static std::string infer_column_name(
        uint32_t ordinal,
        const std::vector<std::string>& values);

private:
    static SchemaTemplate::Column infer_numeric_column(
        const std::vector<std::string>& values);

    static SchemaTemplate::Column infer_string_column(
        const std::vector<std::string>& values);

    static SchemaTemplate::Column infer_datetime_column(
        const std::string& value);
};

// ==================== BinaryTupleParser ====================

/**
 * @brief Low-level binary tuple parser
 * 
 * Parses MySQL binary format to field values.
 */
class BinaryTupleParser {
public:
    /// @brief Parse result
    struct ParseResult {
        bool success;
        std::vector<std::string> fields;
        std::vector<uint8_t> null_bitmap;
        std::vector<uint32_t> field_lengths;
        
        std::string error_message;
    };

    /**
     * @brief Parse tuple from data
     * 
     * @param data Data buffer
     * @param schema Schema template
     * @return Parse result
     */
    static ParseResult parse(
        const uint8_t* data,
        uint32_t data_size,
        const SchemaTemplate& schema);

    /**
     * @brief Parse field value
     * 
     * @param data Data buffer
     * @param offset Current offset
     * @param column Column definition
     * @param value Output value
     * @return New offset
     */
    static uint32_t parse_field_value(
        const uint8_t* data,
        uint32_t offset,
        const SchemaTemplate::Column& column,
        std::string& value);

    /**
     * @brief Read variable-length integer
     * 
     * @param data Data buffer
     * @param offset Current offset
     * @return Parsed value
     */
    static uint64_t read_varint(
        const uint8_t* data,
        uint32_t& offset);

    /**
     * @brief Read length-encoded string
     * 
     * @param data Data buffer
     * @param offset Current offset
     * @return Parsed string
     */
    static std::string read_length_encoded_string(
        const uint8_t* data,
        uint32_t& offset);
};

// ==================== SchemaTemplateValidator ====================

/**
 * @brief Schema template validator
 * 
 * Validates that recovered data matches expected schema.
 */
class SchemaTemplateValidator {
public:
    /// @brief Validation result
    struct ValidationResult {
        bool is_valid;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        
        // Statistics
        uint32_t total_records;
        uint32_t valid_records;
        uint32_t invalid_records;
        uint32_t partial_records;
    };

    /**
     * @brief Validate records against schema
     * 
     * @param records Recovered records
     * @param template_ Schema template
     * @return Validation result
     */
    static ValidationResult validate(
        const std::vector<RecoveredRecord>& records,
        const SchemaTemplate& template_);

    /**
     * @brief Validate single record
     * 
     * @param record Recovered record
     * @param template_ Schema template
     * @return True if valid
     */
    static bool validate_record(
        const RecoveredRecord& record,
        const SchemaTemplate& template_);

    /**
     * @brief Validate field value
     * 
     * @param value Field value
     * @param column Column definition
     * @return True if valid
     */
    static bool validate_field(
        const std::string& value,
        const SchemaTemplate::Column& column);

private:
    static bool validate_integer(
        const std::string& value,
        const SchemaTemplate::Column& column);

    static bool validate_datetime(
        const std::string& value,
        const SchemaTemplate::Column& column);

    static bool validate_string(
        const std::string& value,
        const SchemaTemplate::Column& column);

    static bool validate_email(
        const std::string& value,
        const SchemaTemplate::Column& column);
};

// ==================== SchemaTemplate recovery from SDI ====================

/**
 * @brief Schema recovery from SDI metadata
 * 
 * Extracts schema from InnoDB SDI pages.
 */
class SchemaSdiRecovery {
public:
    /// @brief Recovery result
    struct Result {
        SchemaTemplate schema;
        bool found;
        std::string error_message;
    };

    /**
     * @brief Recover schema from SDI
     * 
     * @param sdi_data SDI page data
     * @return Recovery result
     */
    static Result recover_from_sdi(const std::vector<uint8_t>& sdi_data);

    /**
     * @brief Extract schema from SDI JSON
     * 
     * @param sdi_json SDI JSON string
     * @return Recovered schema
     */
    static SchemaTemplate::Column recover_from_sdi_json(
        const std::string& sdi_json);

    /**
     * @brief Parse SDI payload
     * 
     * @param data SDI data
     * @param payload_offset Offset to payload
     * @param payload_size Payload size
     * @return SDI JSON
     */
    static std::optional<std::string> extract_sdi_json(
        const uint8_t* data,
        uint32_t payload_offset,
        uint32_t payload_size);
};

// ==================== SchemaTemplate recovery from .frm ====================

/**
 * @brief Schema recovery from .frm file
 * 
 * Parses MySQL .frm files to extract schema.
 */
class SchemaFrmRecovery {
public:
    /// @brief Header structure
    struct FrmHeader {
        uint16_t type;           // 0 = table, 1 = view
        uint8_t version;         // File format version
        uint8_t db_type;         // Database type (3 = InnoDB)
        uint32_t length;         // Length of file definition
        uint16_t record_length;  // Record length
        uint16_t key_info_length; // Key info length
        uint16_t mysql_version;  // MySQL version
        uint32_t extra_size;     // Extra info size
    };

    /// @brief Key information
    struct KeyInfo {
        uint32_t flags;
        uint32_t key_length;
        uint32_t extra_length;
        uint32_t key_parts;
    };

    /// @brief Recovery result
    struct Result {
        SchemaTemplate schema;
        bool found;
        std::string error_message;
    };

    /**
     * @brief Recover schema from .frm file
     * 
     * @param frm_path Path to .frm file
     * @return Recovery result
     */
    static Result recover(const std::string& frm_path);

    /**
     * @brief Parse .frm header
     * 
     * @param data .frm data
     * @param header Output header
     * @return True if successful
     */
    static bool parse_header(
        const uint8_t* data,
        uint32_t size,
        FrmHeader& header);

    /**
     * @brief Parse InnoDB extra info
     * 
     * @param data .frm data
     * @param extra_size Extra info size
     * @param schema Output schema
     * @return True if successful
     */
    static bool parse_innodb_info(
        const uint8_t* data,
        uint32_t extra_size,
        SchemaTemplate& schema);
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_SCHEMA_AWARE_PARSER_HPP
