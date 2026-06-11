#include "page/page_parser.h"
#include "page/page_parser_v55.h"
#include "page/page_parser_v84.h"
#include "page/page_parser_factory.h"
#include "dbrecovery/innodb/mach_read.hpp"
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace mysql_recovery {

// ==================== PageFactory Static Member Definitions ====================

// SDI Signature (8 bytes) - defined in header as public constexpr
constexpr uint8_t PageFactory::SDI_SIG[8];

// ==================== PageFactory Public Methods ====================

std::unique_ptr<PageHeaderBase> PageFactory::create_page_header(
    const uint8_t* page_data,
    size_t page_size
) {
    if (!page_data) {
        throw std::invalid_argument("Page data pointer is null");
    }

    // Detect version using enhanced factory
    MySQLVersion version = PageFactoryEnhanced::detect_version(page_data, page_size);

    // Create appropriate PageHeader based on version
    switch (version) {
        case MySQLVersion::MYSQL_56:
        case MySQLVersion::MYSQL_57:
            return std::make_unique<PageHeaderV56>(page_data, page_size);

        case MySQLVersion::MYSQL_80:
            return std::make_unique<PageHeaderV80>(page_data, page_size, version);

        case MySQLVersion::MYSQL_84:
            return std::make_unique<PageHeaderV84>(page_data, page_size, version);

        default:
            // Fallback to 5.6 compatible mode for unsupported versions
            return std::make_unique<PageHeaderV56>(page_data, page_size);
    }
}

MySQLVersion PageFactory::detect_version(const uint8_t* page_data, size_t page_size) {
    return PageFactoryEnhanced::detect_version(page_data, page_size);
}

RowFormat PageFactory::detect_row_format(
    const uint8_t* page_data,
    MySQLVersion version,
    size_t page_size
) {
    return PageFactoryEnhanced::detect_row_format(page_data, version, page_size);
}

bool PageFactory::is_sdi_page(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 72) {
        return false;
    }

    // Check page type
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data, 16);
    if (page_type == static_cast<uint16_t>(PageType::SDI)) {
        return true;
    }

    // Check SDI Signature
    if (std::memcmp(page_data + 64, SDI_SIG, 8) == 0) {
        return true;
    }

    return false;
}

RowFormat PageFactory::detect_row_format_from_record(
    const uint8_t* record_data,
    size_t record_len
) {
    if (!record_data || record_len < 5) {
        return RowFormat::COMPACT;
    }

    // Check extra_bytes (5th byte of record header)
    uint8_t extra_bytes = record_data[4];

    if (extra_bytes == 6) {
        return RowFormat::REDUNDANT;
    }

    // COMPACT/DYNAMIC: extra_bytes = 5
    // Through checking for external pointer flag to distinguish
    if (record_len > 25 && (record_data[0] & 0x20) != 0) {
        return RowFormat::DYNAMIC;
    }

    return RowFormat::COMPACT;
}

std::string PageFactory::page_type_to_string(uint16_t page_type) {
    return PageFactoryEnhanced::page_type_to_string(page_type);
}

// ==================== RowParser Implementations ====================

// CompactRowParser Implementation
CompactRowParser::RecordHeader CompactRowParser::parse_record_header(
    const uint8_t* data,
    RowFormat format
) {
    RecordHeader header{};

    if (format == RowFormat::REDUNDANT) {
        // Redundant format: 6-byte header
        // [0]: info_bits
        // [1-2]: next_record_offset (15 bits)
        // [3-4]: heap_no (15 bits)
        // [5]: n_fields

        header.heap_no = dbrecovery::innodb::read_le<uint16_t>(data, 3) & 0x7FFF;
        header.next_record_offset = dbrecovery::innodb::read_le<uint16_t>(data, 1) & 0x7FFF;
        header.rec_type = data[0] & 0x07;
        header.deleted_flag = (data[0] & 0x08) != 0;
        header.min_rec_flag = (data[0] & 0x10) != 0;
        header.record_length = data[5];
    } else {
        // Compact format: 5-byte header
        // [0]: info_bits
        // [1-2]: next_record_offset (15 bits)
        // [3-4]: heap_no (15 bits)

        header.heap_no = dbrecovery::innodb::read_le<uint16_t>(data, 3) & 0x7FFF;
        header.next_record_offset = dbrecovery::innodb::read_le<uint16_t>(data, 1) & 0x7FFF;
        header.rec_type = data[0] & 0x07;
        header.deleted_flag = (data[0] & 0x08) != 0;
        header.min_rec_flag = (data[0] & 0x10) != 0;
        header.record_length = (data[0] >> 3) & 0x1F;
    }

    return header;
}

std::vector<CompactRowParser::ColumnInfo> CompactRowParser::parse_columns(
    const uint8_t* data,
    const std::vector<uint8_t>& col_meta,
    RowFormat format
) {
    std::vector<ColumnInfo> columns;

    // Parse column metadata
    size_t offset = 0;
    for (size_t i = 0; i < col_meta.size(); i++) {
        ColumnInfo col{};
        col.col_type = col_meta[i];

        if (format == RowFormat::DYNAMIC || format == RowFormat::COMPRESSED) {
            // Dynamic format: variable length columns have 2-byte length
            if (col.col_type >= 251) {
                col.is_variable = true;
                col.length = 2;  // Variable length indicator
            }
        } else {
            // Compact/Redundant: fixed length
            col.is_variable = false;
            col.length = 1;
        }

        col.offset = static_cast<uint16_t>(offset);
        offset += col.length;

        columns.push_back(col);
    }

    return columns;
}

std::string CompactRowParser::generate_create_statement(
    const std::vector<ColumnInfo>& cols,
    const std::unordered_map<uint32_t, std::string>& col_names
) {
    std::string sql = "CREATE TABLE recovered_table (\n";

    for (size_t i = 0; i < cols.size(); i++) {
        const auto& col = cols[i];
        std::string col_name = "col_" + std::to_string(i);

        auto it = col_names.find(i);
        if (it != col_names.end()) {
            col_name = it->second;
        }

        // Generate column type based on col_type
        std::string col_type;
        switch (col.col_type) {
            case 1:  // TINYINT
                col_type = "TINYINT";
                break;
            case 2:  // SMALLINT
                col_type = "SMALLINT";
                break;
            case 3:  // MEDIUMINT
                col_type = "MEDIUMINT";
                break;
            case 4:  // INT
                col_type = "INT";
                break;
            case 8:  // BIGINT
                col_type = "BIGINT";
                break;
            case 10:  // DATE
                col_type = "DATE";
                break;
            case 12:  // DATETIME
                col_type = "DATETIME";
                break;
            case 15:  // VARCHAR
                col_type = "VARCHAR(" + std::to_string(col.length) + ")";
                break;
            case 253:  // VAR_STRING
                col_type = "VARCHAR(" + std::to_string(col.length) + ")";
                break;
            case 252:  // BLOB
                col_type = "BLOB";
                break;
            default:
                col_type = "BINARY(" + std::to_string(col.length) + ")";
                break;
        }

        sql += "  `" + col_name + "` " + col_type;
        if (col.is_null) {
            sql += " NULL";
        } else {
            sql += " NOT NULL";
        }

        if (i < cols.size() - 1) {
            sql += ",";
        }
        sql += "\n";
    }

    sql += ")";

    return sql;
}

std::vector<std::optional<std::string>> CompactRowParser::extract_column_values(
    const uint8_t* record_data,
    size_t record_len,
    const std::vector<ColumnInfo>& cols
) {
    std::vector<std::optional<std::string>> values;

    size_t data_offset = 5;  // Skip record header

    for (const auto& col : cols) {
        if (data_offset >= record_len) {
            values.emplace_back(std::nullopt);
            continue;
        }

        if (col.is_null) {
            // Check null bitmap
            size_t null_byte = data_offset / 8;
            size_t null_bit = data_offset % 8;

            if (null_byte < record_len && (record_data[null_byte] & (1 << null_bit))) {
                values.emplace_back(std::nullopt);
                data_offset++;
                continue;
            }
        }

        // Extract column value
        std::string value;
        if (col.is_variable) {
            // Variable length column
            uint16_t len = dbrecovery::innodb::read_le<uint16_t>(record_data, data_offset);
            data_offset += 2;

            if (len > 0 && data_offset + len <= record_len) {
                value = std::string(reinterpret_cast<const char*>(record_data + data_offset), len);
                data_offset += len;
            }
        } else {
            // Fixed length column
            value = std::string(reinterpret_cast<const char*>(record_data + data_offset), col.length);
            data_offset += col.length;
        }

        values.emplace_back(value);
    }

    return values;
}

// RedundantRowParser Implementation
RedundantRowParser::RecordHeader RedundantRowParser::parse_record_header(
    const uint8_t* data,
    RowFormat format
) {
    RecordHeader header{};

    // Redundant format: 6-byte header
    header.heap_no = dbrecovery::innodb::read_le<uint16_t>(data, 3) & 0x7FFF;
    header.next_record_offset = dbrecovery::innodb::read_le<uint16_t>(data, 1) & 0x7FFF;
    header.rec_type = data[0] & 0x07;
    header.deleted_flag = (data[0] & 0x08) != 0;
    header.min_rec_flag = (data[0] & 0x10) != 0;
    header.record_length = data[5];

    return header;
}

std::vector<RedundantRowParser::ColumnInfo> RedundantRowParser::parse_columns(
    const uint8_t* data,
    const std::vector<uint8_t>& col_meta,
    RowFormat format
) {
    std::vector<ColumnInfo> columns;

    // Redundant format: fixed 2-byte length per column
    for (size_t i = 0; i < col_meta.size(); i++) {
        ColumnInfo col{};
        col.col_type = col_meta[i];
        col.is_variable = false;
        col.length = 2;  // Redundant uses 2-byte length

        columns.push_back(col);
    }

    return columns;
}

std::string RedundantRowParser::generate_create_statement(
    const std::vector<ColumnInfo>& cols,
    const std::unordered_map<uint32_t, std::string>& col_names
) {
    CompactRowParser parser;
    return parser.generate_create_statement(cols, col_names);
}

std::vector<std::optional<std::string>> RedundantRowParser::extract_column_values(
    const uint8_t* record_data,
    size_t record_len,
    const std::vector<ColumnInfo>& cols
) {
    CompactRowParser parser;
    return parser.extract_column_values(record_data, record_len, cols);
}

// ==================== SDIParser Implementation ====================

std::vector<SDIParser::SDIEntry> SDIParser::parse_sdi_page(const uint8_t* page_data, size_t page_size) {
    return SDIParserImpl::parse_sdi_page_impl(page_data, page_size);
}

std::optional<SDIParser::SDIEntry> SDIParser::find_table_by_name(
    const std::vector<SDIParser::SDIEntry>& sdi_entries,
    const std::string& table_name
) {
    for (const auto& entry : sdi_entries) {
        if (entry.table_name == table_name) {
            return entry;
        }

        // Recursively search child entries
        auto found = find_table_by_name(entry.child_entries, table_name);
        if (found.has_value()) {
            return found;
        }
    }

    return std::nullopt;
}

bool SDIParser::is_sdi_page(const uint8_t* page_data, size_t page_size) {
    if (!page_data || page_size < 72) {
        return false;
    }

    // Check page type
    uint16_t page_type = dbrecovery::innodb::read_le<uint16_t>(page_data, 16);
    if (page_type == static_cast<uint16_t>(PageType::SDI)) {
        return true;
    }

    // Check SDI Signature
    if (std::memcmp(page_data + 64, PageFactory::SDI_SIG, 8) == 0) {
        return true;
    }

    return false;
}

std::vector<SDIParser::SDIEntry> SDIParserImpl::parse_sdi_page_impl(
    const uint8_t* page_data,
    size_t page_size
) {
    std::vector<SDIParser::SDIEntry> entries;

    if (!page_data || page_size < 128) {
        return entries;
    }

    // SDI page structure (MySQL 8.0+)
    // Offset 0-3: FIL_PAGE_PREV
    // Offset 4-7: FIL_PAGE_NEXT
    // Offset 8-15: FIL_PAGE_LSN
    // Offset 16-17: FIL_PAGE_TYPE (should be 0x26F3 = 9971)
    // Offset 18-21: FIL_PAGE_SPACE_ID
    // Offset 22-23: FIL_PAGE_SPACE_OFFSET
    // Offset 24-31: FIL_PAGE_FILE_FLUSH_LSN (8 bytes in 8.0+)
    // Offset 32-33: PAGE_N_DIR_SLOTS
    // Offset 34-35: PAGE_HEAP_TOP
    // Offset 36-37: PAGE_N_HEAP
    // Offset 38-39: PAGE_FREE
    // Offset 40-41: PAGE_GARBAGE_END
    // Offset 42-43: PAGE_LAST_INSERT
    // Offset 44-45: PAGE_DIRECTION
    // Offset 46-47: PAGE_N_DIRECT_PAGES

    // SDI data starts at offset 64
    // SDI Signature: "08SDI0000" (8 bytes at offset 64)

    // Check SDI Signature
    if (page_size < 72 || std::memcmp(page_data + 64, PageFactory::SDI_SIG, 8) != 0) {
        return entries;
    }

    // Parse SDI data
    // SDI data is JSON format, needs parsing
    // Simplified here, returns empty entry list
    // Full implementation needs complete JSON parser

    return entries;
}

} // namespace mysql_recovery
