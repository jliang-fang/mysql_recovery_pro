/**
 * @file schema_template.hpp
 * @brief Schema template definition for recovery modules
 *
 * Standalone definition that does not pull in nlohmann/json or other
 * heavy dependencies. Shared between signature_scraper and schema_aware_parser.
 */

#ifndef DBRECOVERY_ENGINE_SCHEMA_TEMPLATE_HPP
#define DBRECOVERY_ENGINE_SCHEMA_TEMPLATE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace dbrecovery {
namespace engine {

// ==================== SchemaTemplate ====================

/**
 * @brief Schema template for recovery
 *
 * Used to verify and parse recovered data.
 */
struct SchemaTemplate {
    std::string table_name;
    std::string database_name;
    uint32_t page_size = 16384;                  ///< Expected page size
    uint32_t expected_column_count = 0;          ///< Number of expected columns
    uint64_t expected_row_count = 0;             ///< Expected row count (optional)
    uint64_t expected_data_size = 0;             ///< Expected data size (optional)

    // Column definitions
    struct Column {
        std::string name;
        uint8_t mysql_type = 0;
        std::string type_name;
        uint32_t max_length = 0;          ///< Byte storage size or max char count
        uint32_t char_length = 0;          ///< Character length (for VARCHAR: N in VARCHAR(N))
        uint32_t mbmaxlen = 1;             ///< Max bytes per character (1=latin1, 4=utf8mb4)
        bool is_nullable = true;
        bool is_primary_key = false;
        bool is_auto_increment = false;
        uint32_t charset_id = 0;
        std::string collation;
        bool is_variable_length = false;  // VARCHAR/TEXT/DECIMAL/BLOB etc.
        bool is_signed = true;            // Signed vs unsigned integer
        uint32_t decimal_precision = 0;   ///< For DECIMAL: total digits
        uint32_t decimal_scale = 0;       ///< For DECIMAL: digits after decimal point

        // Default value (as string)
        std::string default_value;
    };

    std::vector<Column> columns;
    std::vector<uint32_t> primary_key_columns;

    // Validation hints
    struct ValidationHints {
        uint32_t expected_min_records = 0;
        uint32_t expected_max_records = 0;
        uint64_t expected_data_size = 0;
        std::optional<uint64_t> truncate_lsn;
    } hints;

    // Recovery policy
    struct RecoveryPolicy {
        enum class MissingColumnPolicy {
            SKIP,           // Skip records with missing columns
            FILL_NULL,      // Fill missing columns with NULL
            FILL_DEFAULT,   // Fill missing columns with default
            ERROR           // Treat as error
        } missing_column_policy = MissingColumnPolicy::FILL_NULL;

        enum class ExtraColumnPolicy {
            IGNORE,         // Ignore extra columns
            STORE_GENERIC   // Store in generic field
        } extra_column_policy = ExtraColumnPolicy::IGNORE;
    } policy;

    // Get column by name
    const Column* get_column(const std::string& name) const {
        for (const auto& col : columns) {
            if (col.name == name) {
                return &col;
            }
        }
        return nullptr;
    }

    // Get index of primary key column
    std::optional<uint32_t> get_primary_key_index() const {
        if (primary_key_columns.empty()) {
            return std::nullopt;
        }
        return primary_key_columns[0];
    }
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_SCHEMA_TEMPLATE_HPP
