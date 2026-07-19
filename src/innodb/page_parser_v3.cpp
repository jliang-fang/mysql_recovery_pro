#include "dbrecovery/innodb/page_parser_v3.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// CRC32C lookup table (Castagnoli polynomial 0x82F63B78)
static uint32_t crc32c_table[256];
static bool crc32c_table_init = false;

static void init_crc32c() {
    if (crc32c_table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0x82F63B78) : (crc >> 1);
        }
        crc32c_table[i] = crc;
    }
    crc32c_table_init = true;
}

// ==================== PageParserV3 实现 ====================

PageParserV3::PageParserV3(const uint8_t* data, size_t size)
    : data_(data), size_(size) {
    // 初始化结果
    result_.success = false;
    result_.page_type = 0;
    result_.page_level = 0;
    result_.row_format = RowFormat::UNKNOWN;
    result_.version = MySQLVersion::UNKNOWN;
    result_.checksum_valid = false;
    result_.checksum_stored = 0;
    result_.checksum_computed = 0;
}

PageParseResult PageParserV3::parse() {
    // 1. 验证页大小
    if (size_ < 4096 || size_ > 65536) {
        result_.error_message = "Invalid page size: " + std::to_string(size_);
        return result_;
    }

    // 2. 创建解析器
    parser_ = PageParserFactoryV3::create_parser(data_, size_);
    if (!parser_) {
        result_.error_message = "Failed to create parser";
        return result_;
    }

    // 3. 解析页头
    if (!parser_->parse_header(data_, size_)) {
        result_.error_message = "Failed to parse page header";
        return result_;
    }

    // 4. 获取解析结果
    result_.success = true;
    result_.page_type = parser_->get_page_type();
    result_.page_level = parser_->get_page_level();
    result_.row_format = parser_->get_row_format();
    result_.version = parser_->get_mysql_version();

    // 5. 解析记录
    result_.records = parser_->parse_records(data_, size_);

    // 6. 解析 SDI 元数据
    result_.schema = parser_->parse_sdi_metadata(data_, size_);

    // 7. 获取页链信息
    auto chain = parser_->get_page_chain();
    result_.prev_page = chain.first;
    result_.next_page = chain.second;

    // 8. 验证页健康状态
    uint32_t stored_checksum = read_le<uint32_t>(data_, 0);
    result_.checksum_stored = stored_checksum;
    init_crc32c();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 4; i < size_; i++) {
        crc = (crc >> 8) ^ crc32c_table[(crc ^ data_[i]) & 0xFF];
    }
    result_.checksum_computed = crc ^ 0xFFFFFFFF;
    result_.checksum_valid = (stored_checksum == result_.checksum_computed);

    return result_;
}

PageParseResult PageParserV3::parse_as(MySQLVersion version) {
    // 使用指定版本创建解析器
    RowFormat format = parser_ ? parser_->get_row_format() : RowFormat::UNKNOWN;

    switch (version) {
        case MySQLVersion::MYSQL_56:
        case MySQLVersion::MYSQL_57:
            parser_ = std::make_unique<MySQL56PageParser>(format);
            break;

        case MySQLVersion::MYSQL_80:
            parser_ = std::make_unique<MySQL80PageParser>(format);
            break;

        case MySQLVersion::MYSQL_84:
            parser_ = std::make_unique<MySQL84PageParser>(format);
            break;

        default:
            parser_ = std::make_unique<GenericPageParser>(version, format);
            break;
    }

    return parse();
}

PageParseResult PageParserV3::parse_with_format(RowFormat format) {
    // 使用指定行格式创建解析器
    MySQLVersion version = parser_ ? parser_->get_mysql_version() : MySQLVersion::UNKNOWN;

    switch (version) {
        case MySQLVersion::MYSQL_56:
        case MySQLVersion::MYSQL_57:
            parser_ = std::make_unique<MySQL56PageParser>(format);
            break;

        case MySQLVersion::MYSQL_80:
            parser_ = std::make_unique<MySQL80PageParser>(format);
            break;

        case MySQLVersion::MYSQL_84:
            parser_ = std::make_unique<MySQL84PageParser>(format);
            break;

        default:
            parser_ = std::make_unique<GenericPageParser>(version, format);
            break;
    }

    return parse();
}

std::vector<PageParseResult> PageParserV3::traverse_btree() {
    std::vector<PageParseResult> results;
    
    if (!parser_) {
        return results;
    }

    // 从根页开始遍历
    uint32_t root_page = 0; // 从 Page 0 开始（通常是 FSP_HDR）
    
    // 获取页链信息
    auto chain = parser_->get_page_chain();
    uint32_t current_page = chain.second; // 从 next_page 开始

    // 遍历 B+ 树
    BTreeTraverser traverser(data_, size_);
    results = traverser.traverse_from_page(current_page);
    
    return results;
}

std::vector<uint32_t> PageParserV3::get_page_chain() {
    std::vector<uint32_t> chain;
    
    if (!parser_) {
        return chain;
    }

    auto page_chain = parser_->get_page_chain();
    uint32_t current_page = page_chain.second;
    
    // 遍历页链
    while (current_page != 0xFFFFFFFF && current_page < size_ / 16384) {
        chain.push_back(current_page);
        
        // 读取下一页的 prev_page
        if (current_page * 16384 + 24 + 4 > size_) {
            break;
        }
        
        const uint8_t* next_page_data = data_ + current_page * 16384;
        current_page = read_be<uint32_t>(next_page_data, FIL_PAGE_NEXT_ABS); // FIL_PAGE_NEXT at offset 12
    }
    
    return chain;
}

// ==================== RedundantRowParser 实现 ====================

RedundantHeader RedundantRowParser::parse_redundant_header(const uint8_t* data) {
    RedundantHeader header{};

    if (!data) {
        return header;
    }

    // Redundant format: 6-byte header (big-endian)
    auto opt = RedundantHeader::parse(data, 6);
    return opt.has_value() ? opt.value() : header;
}

RecordHeader RedundantRowParser::parse_record_header(const uint8_t* data) {
    RecordHeader header{};

    if (!data) {
        return header;
    }

    // For backward compatibility, parse as RedundantHeader first
    RedundantHeader redundant = parse_redundant_header(data);

    // Map REDUNDANT fields to COMPACT-style RecordHeader (5 bytes)
    // Note: This mapping is lossy - heap_no encoding is different
    // COMPACT: bits 15-3 = heap_no (in byte1/byte2)
    // REDUNDANT: bits 23-11 = heap_no (in byte1/byte2)
    // For compatibility, encode heap_no for COMPACT format
    uint16_t heap_no = redundant.get_heap_no();
    // COMPACT format: heap_no is stored as (heap_no << 3) in bits 15-3
    uint16_t compact_heap = heap_no << 3;  // Shift to COMPACT position
    header.byte1 = (compact_heap >> 8) & 0xFF;
    header.byte2 = compact_heap & 0xFF;

    // Next offset - REDUNDANT is big-endian, COMPACT is big-endian too
    header.next_off_0 = redundant.next_off_0;
    header.next_off_1 = redundant.next_off_1;

    // Info bits (n_owned bits 19-16 vs 27-24)
    header.byte0 = redundant.get_n_owned();  // Just store n_owned

    return header;
}

std::vector<RowValue> RedundantRowParser::parse_record_data(
    const uint8_t* data,
    size_t data_size,
    const RecordHeader& header,
    const std::vector<catalog::ColumnInfo>& columns
) {
    std::vector<RowValue> values;

    if (!data || data_size < 6) {
        return values;
    }

    // Skip header (6 bytes for Redundant)
    size_t offset = 6;
    
    // Read column lengths (2 bytes each)
    std::vector<uint16_t> col_lengths;
    for (size_t i = 0; i < columns.size(); i++) {
        if (offset + 2 > data_size) {
            break;
        }
        uint16_t len = read_le<uint16_t>(data, offset);
        col_lengths.push_back(len);
        offset += 2;
    }
    
    // Read column data
    for (size_t i = 0; i < columns.size(); i++) {
        if (i >= col_lengths.size()) {
            values.emplace_back(std::nullopt);
            continue;
        }
        
        uint16_t len = col_lengths[i];
        
        if (len == 0xFFFF || len == 0) {
            // NULL value
            values.emplace_back(std::nullopt);
        } else if (offset + len <= data_size) {
            // Valid data
            std::string value(reinterpret_cast<const char*>(data + offset), len);
            values.emplace_back(value);
            offset += len;
        } else {
            // Truncated data
            values.emplace_back(std::nullopt);
        }
    }
    
    return values;
}

size_t RedundantRowParser::get_record_size(const uint8_t* data, size_t max_size) {
    if (!data || max_size < 6) {
        return 0;
    }

    // Read next_record_offset to determine record size
    uint16_t next_offset = read_le<uint16_t>(data, 1) & 0x7FFF;
    
    if (next_offset == 0 || next_offset >= max_size) {
        // Last record or invalid
        return max_size;
    }
    
    return next_offset;
}

bool RedundantRowParser::is_record_deleted(const uint8_t* data) {
    if (!data) {
        return false;
    }
    return (data[0] & 0x08) != 0;
}

bool RedundantRowParser::is_min_record(const uint8_t* data) {
    if (!data) {
        return false;
    }
    return (data[0] & 0x07) == REC_STATUS_INFIMUM;
}

bool RedundantRowParser::is_max_record(const uint8_t* data) {
    if (!data) {
        return false;
    }
    return (data[0] & 0x07) == REC_STATUS_SUPREMUM;
}

// ==================== CompactRowParser 实现 ====================

RecordHeader CompactRowParser::parse_record_header(const uint8_t* data) {
    RecordHeader header{};

    if (!data) {
        return header;
    }

    // Compact format: 5-byte header (big-endian)
    // Byte 0-2: 24-bit info bits (bits 15-3 = heap_no)
    // Byte 3-4: 16-bit next_offset (signed)
    auto opt = RecordHeader::parse(data, 5);
    return opt.has_value() ? opt.value() : header;
}

std::vector<RowValue> CompactRowParser::parse_record_data(
    const uint8_t* data,
    size_t data_size,
    const RecordHeader& header,
    const std::vector<catalog::ColumnInfo>& columns
) {
    std::vector<RowValue> values;

    if (!data || data_size < 5) {
        return values;
    }

    // Skip header (5 bytes for Compact)
    size_t offset = 5;

    // Read variable length flags (1 byte per column for non-NULL columns)
    // Compact format uses 1-byte length for columns < 256 bytes
    std::vector<uint8_t> var_lengths;
    for (size_t i = 0; i < columns.size(); i++) {
        if (columns[i].is_variable_length && offset < data_size) {
            uint8_t len = data[offset];
            var_lengths.push_back(len);
            offset++;
        }
    }

    // Read column data
    for (size_t i = 0; i < columns.size(); i++) {
        if (columns[i].is_nullable && offset < data_size) {
            // Check null bitmap
            size_t null_byte = offset / 8;
            size_t null_bit = offset % 8;

            if (null_byte < data_size) {
                if (data[null_byte] & (1 << null_bit)) {
                    values.emplace_back(std::nullopt);
                    offset++;
                    continue;
                }
            }
        }

        if (columns[i].is_variable_length && i < var_lengths.size()) {
            uint8_t len = var_lengths[i];
            if (offset + len <= data_size) {
                std::string value(reinterpret_cast<const char*>(data + offset), len);
                values.emplace_back(value);
                offset += len;
            } else {
                values.emplace_back(std::nullopt);
            }
        } else {
            // Fixed length column
            size_t col_size = columns[i].column_length;
            if (offset + col_size <= data_size) {
                std::string value(reinterpret_cast<const char*>(data + offset), col_size);
                values.emplace_back(value);
                offset += col_size;
            } else {
                values.emplace_back(std::nullopt);
            }
        }
    }

    return values;
}

size_t CompactRowParser::get_record_size(const uint8_t* data, size_t max_size) {
    if (!data || max_size < 5) {
        return 0;
    }

    // Read next_record_offset to determine record size
    uint16_t next_offset = read_le<uint16_t>(data, 1) & 0x7FFF;
    
    if (next_offset == 0 || next_offset >= max_size) {
        return max_size;
    }
    
    return next_offset;
}

bool CompactRowParser::is_record_deleted(const uint8_t* data) {
    if (!data) {
        return false;
    }
    return (data[0] & 0x08) != 0;
}

bool CompactRowParser::is_min_record(const uint8_t* data) {
    if (!data) {
        return false;
    }
    return (data[0] & 0x07) == REC_STATUS_INFIMUM;
}

bool CompactRowParser::is_max_record(const uint8_t* data) {
    if (!data) {
        return false;
    }
    return (data[0] & 0x07) == REC_STATUS_SUPREMUM;
}

// ==================== DynamicRowParser 实现 ====================

RecordHeader DynamicRowParser::parse_record_header(const uint8_t* data) {
    // Dynamic uses same header format as Compact
    CompactRowParser parser;
    return parser.parse_record_header(data);
}

std::vector<RowValue> DynamicRowParser::parse_record_data(
    const uint8_t* data,
    size_t data_size,
    const RecordHeader& header,
    const std::vector<catalog::ColumnInfo>& columns
) {
    // Dynamic uses same format as Compact for inline data
    // External BLOB data is stored separately
    CompactRowParser parser;
    return parser.parse_record_data(data, data_size, header, columns);
}

size_t DynamicRowParser::get_record_size(const uint8_t* data, size_t max_size) {
    CompactRowParser parser;
    return parser.get_record_size(data, max_size);
}

bool DynamicRowParser::is_record_deleted(const uint8_t* data) {
    CompactRowParser parser;
    return parser.is_record_deleted(data);
}

bool DynamicRowParser::is_min_record(const uint8_t* data) {
    CompactRowParser parser;
    return parser.is_min_record(data);
}

bool DynamicRowParser::is_max_record(const uint8_t* data) {
    CompactRowParser parser;
    return parser.is_max_record(data);
}

std::optional<DynamicRowParser::ExternalPointer> DynamicRowParser::parse_external_pointer(const uint8_t* data, size_t data_size) {
    if (!data || data_size < 20) {
        return std::nullopt;
    }

    // External pointer format (20 bytes):
    // [0-3]: space_id
    // [4-7]: page_no
    // [8-11]: offset
    // [12-15]: len
    // [16-19]: compressed_len (optional)

    ExternalPointer ptr{};
    ptr.space_id = read_le<uint32_t>(data, 0);
    ptr.page_no = read_le<uint32_t>(data, 4);
    ptr.offset = read_le<uint32_t>(data, 8);
    ptr.len = read_le<uint32_t>(data, 12);

    // Check if compressed
    if (data_size >= 20) {
        ptr.is_compressed = (data[16] & 0x80) != 0;
    }

    return ptr;
}

// ==================== BTreeTraverser 实现 ====================

BTreeTraverser::BTreeTraverser(const uint8_t* data, size_t size)
    : data_(data), size_(size) {
    stats_.total_pages = 0;
    stats_.leaf_pages = 0;
    stats_.internal_pages = 0;
    stats_.root_page = 0;
    stats_.max_level = 0;
}

std::vector<PageParseResult> BTreeTraverser::traverse_from_page(uint32_t page_no) {
    std::vector<PageParseResult> results;
    
    if (!data_ || page_no >= size_ / 16384) {
        return results;
    }

    traverse_recursive(page_no, 0, results);
    
    return results;
}

std::vector<PageParseResult> BTreeTraverser::traverse_from_root() {
    // Root page is typically at offset 3 (after FSP_HDR, IBUF_BITMAP, INODE)
    return traverse_from_page(3);
}

void BTreeTraverser::traverse_recursive(
    uint32_t page_no,
    uint16_t level,
    std::vector<PageParseResult>& results
) {
    if (!data_ || page_no >= size_ / 16384) {
        return;
    }

    // Read page
    const uint8_t* page_data = data_ + page_no * 16384;
    
    // Parse page header
    uint16_t page_type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);
    uint16_t page_level = read_le<uint16_t>(page_data, 42);
    uint16_t n_heap = read_le<uint16_t>(page_data, 44) & 0x7FFF;
    
    // Parse records
    PageParseResult result{};
    result.page_type = page_type;
    result.page_level = page_level;
    result.version = MySQLVersion::MYSQL_80; // Default
    
    // Parse record offsets from page directory
    uint16_t n_dir_slots = read_le<uint16_t>(page_data, 38);
    const uint8_t* dir = page_data + 38;

    for (int i = n_dir_slots - 1; i >= 0; --i) {
        uint16_t rec_offset = read_le<uint16_t>(dir, i * 2) & 0x7FFF;

        if (rec_offset < 100 || rec_offset >= 16384) {
            continue;
        }

        // Parse record
        RecordHeader header = CompactRowParser().parse_record_header(page_data + rec_offset);

        Record record{};
        record.offset = rec_offset;
        record.next_offset = header.get_next_record_offset();
        record.is_deleted = header.is_deleted();
        record.heap_no = header.get_heap_no();
        record.info_bits = header.get_info_bits();

        result.records.push_back(record);
    }

    results.push_back(result);

    // Update stats
    stats_.total_pages++;
    if (page_level == 0) {
        stats_.leaf_pages++;
    } else {
        stats_.internal_pages++;
    }
    if (page_level > stats_.max_level) {
        stats_.max_level = page_level;
    }

    // For leaf pages, we're done
    if (page_level == 0) {
        return;
    }

    // For internal pages, traverse child pages (node pointers)
    // Node pointers are stored after the last user record
    // This is a simplified implementation

    // In a real implementation, we would:
    // 1. Find the last record in the page
    // 2. Read the node pointer (next 6 bytes after last record)
    // 3. Recursively traverse child pages
}

// ==================== UndoLogChainParser 实现 ====================

UndoLogChainParser::UndoLogChainParser(const uint8_t* data, size_t size)
    : data_(data), size_(size) {
}

std::vector<UndoLogChainParser::UndoLogEntry> UndoLogChainParser::parse_chain(uint32_t first_page) {
    std::vector<UndoLogEntry> entries;

    uint32_t current_page = first_page;

    while (current_page != 0xFFFFFFFF && current_page < size_ / 16384) {
        const uint8_t* page_data = data_ + current_page * 16384;

        // Parse Undo Log Header
        UndoLogHeader header = parse_undo_log_header(page_data);

        // Parse entries in this page
        // This is a simplified implementation
        // In a real implementation, we would parse each undo log record

        // Move to next page
        current_page = header.next_page;
    }

    return entries;
}

std::vector<uint32_t> UndoLogChainParser::get_undo_log_pages() {
    std::vector<uint32_t> pages;
    
    // Scan for UNDO_LOG pages
    size_t num_pages = size_ / 16384;
    for (size_t i = 0; i < num_pages; i++) {
        const uint8_t* page_data = data_ + i * 16384;
        uint16_t page_type = read_be<uint16_t>(page_data, FIL_PAGE_TYPE_ABS);
        
        if (page_type == FIL_PAGE_UNDO_LOG) {
            pages.push_back(static_cast<uint32_t>(i));
        }
    }
    
    return pages;
}

UndoLogChainParser::UndoLogHeader UndoLogChainParser::parse_undo_log_header(const uint8_t* page_data) {
    UndoLogHeader header{};
    
    // Undo Log Header format:
    // [0-1]: state (TRX_UNDO_ACTIVE = 1, TRX_UNDO_REMOVED = 2)
    // [2-3]: direction (TRX_UNDO_INSERT = 1, TRX_UNDO_UPDATE = 2)
    // [4-7]: last_page
    // [8-9]: last_offset
    // [10-13]: prev_page
    // [14-17]: next_page
    
    header.state = read_le<uint16_t>(page_data, 0);
    header.direction = read_le<uint16_t>(page_data, 2);
    header.last_page = read_le<uint32_t>(page_data, 4);
    header.last_offset = read_le<uint16_t>(page_data, 8);
    header.prev_page = read_le<uint32_t>(page_data, 10);
    header.next_page = read_le<uint32_t>(page_data, 14);
    
    return header;
}

// ==================== PageParserFactoryV3 实现 ====================

std::unique_ptr<IPageParserStrategy> PageParserFactoryV3::create_parser(
    const uint8_t* data, size_t size) {
    
    if (!data || size < 4096) {
        return nullptr;
    }

    // Detect version
    MySQLVersion version = PageParserFactory::detect_mysql_version(data, size);
    
    // Detect row format
    RowFormat format = detect_row_format_v3(data, size);
    
    // Create parser based on version
    switch (version) {
        case MySQLVersion::MYSQL_56:
        case MySQLVersion::MYSQL_57:
            return std::make_unique<MySQL56PageParser>(format);
            
        case MySQLVersion::MYSQL_80:
            return std::make_unique<MySQL80PageParser>(format);

        case MySQLVersion::MYSQL_84:
            return std::make_unique<MySQL84PageParser>(format);

        default:
            return std::make_unique<GenericPageParser>(version, format);
    }
}

std::unique_ptr<IRowParser> PageParserFactoryV3::create_row_parser(RowFormat format) {
    switch (format) {
        case RowFormat::REDUNDANT:
            return std::make_unique<RedundantRowParser>();
            
        case RowFormat::COMPACT:
            return std::make_unique<CompactRowParser>();
            
        case RowFormat::DYNAMIC:
            return std::make_unique<DynamicRowParser>();
            
        default:
            return std::make_unique<CompactRowParser>();
    }
}

RowFormat PageParserFactoryV3::detect_row_format_v3(const uint8_t* data, size_t size) {
    if (!data || size < FIL_HEADER_SIZE + 44) {
        return RowFormat::UNKNOWN;
    }

    // First, try to detect from BTR header flags
    RowFormat format = detect_from_btr_flags(data, size);
    if (format != RowFormat::UNKNOWN) {
        return format;
    }

    // Fall back to record header detection
    return detect_from_record_header(data, size);
}

RowFormat PageParserFactoryV3::detect_from_btr_flags(const uint8_t* data, size_t size) {
    if (size < FIL_HEADER_SIZE + 44) {
        return RowFormat::UNKNOWN;
    }

    // Read BTR header flags (offset 42-43)
    uint16_t flags = read_le<uint16_t>(data, FIL_HEADER_SIZE + 42);
    uint8_t row_format_val = flags & 0x07;

    // InnoDB page format stores row format differently from the enum:
    // Page format: 0=REDUNDANT, 1=COMPACT, 2=DYNAMIC, 3=COMPRESSED
    // Enum: UNKNOWN=0, REDUNDANT=1, COMPACT=2, DYNAMIC=3, COMPRESSED=4
    // So we need to add 1 to the page format value to get the enum value
    switch (row_format_val) {
        case 0: return RowFormat::REDUNDANT;  // 0 + 1 = 1
        case 1: return RowFormat::COMPACT;    // 1 + 1 = 2
        case 2: return RowFormat::DYNAMIC;    // 2 + 1 = 3
        case 3: return RowFormat::COMPRESSED; // 3 + 1 = 4
        default: return RowFormat::UNKNOWN;
    }
}

RowFormat PageParserFactoryV3::detect_from_record_header(const uint8_t* data, size_t size) {
    if (size < 60) { // Need at least INFIMUM + SUPREMUM + first user record
        return RowFormat::UNKNOWN;
    }

    // Check first user record (after INFIMUM at 54, SUPREMUM at 67)
    const uint8_t* first_rec = data + 54 + 13; // After INFIMUM (13 bytes)
    
    if (size < 65) {
        return RowFormat::UNKNOWN;
    }

    // Check extra_bytes (5th byte of record header)
    uint8_t extra_bytes = first_rec[4];
    
    if (extra_bytes == 6) {
        return RowFormat::REDUNDANT;
    }
    
    if (extra_bytes != 5) {
        return RowFormat::UNKNOWN;
    }
    
    // Check for external pointer flag (bit 5)
    uint8_t info_bits = first_rec[0];
    if ((info_bits & 0x20) != 0) {
        return RowFormat::DYNAMIC;
    }
    
    return RowFormat::COMPACT;
}

// ==================== 实用工具函数 ====================

size_t calculate_record_size(
    const uint8_t* data,
    size_t data_size,
    RowFormat format,
    uint16_t n_fields
) {
    if (!data || data_size < 5) {
        return 0;
    }

    switch (format) {
        case RowFormat::REDUNDANT:
            // 6-byte header + column lengths (2 bytes each) + data
            return 6 + n_fields * 2 + data_size - 6;
            
        case RowFormat::COMPACT:
        case RowFormat::DYNAMIC:
            // 5-byte header + variable lengths + data
            return 5 + data_size - 5;
            
        default:
            return data_size;
    }
}

std::vector<std::optional<std::string>> extract_field_values(
    const uint8_t* record_data,
    size_t record_len,
    RowFormat format,
    const std::vector<catalog::ColumnInfo>& columns
) {
    if (!record_data || record_len < 5) {
        return {};
    }

    switch (format) {
        case RowFormat::REDUNDANT: {
            RedundantRowParser parser;
            RecordHeader header = parser.parse_record_header(record_data);
            return parser.parse_record_data(record_data, record_len, header, columns);
        }

        case RowFormat::COMPACT: {
            CompactRowParser parser;
            RecordHeader header = parser.parse_record_header(record_data);
            return parser.parse_record_data(record_data, record_len, header, columns);
        }

        case RowFormat::DYNAMIC: {
            DynamicRowParser parser;
            RecordHeader header = parser.parse_record_header(record_data);
            return parser.parse_record_data(record_data, record_len, header, columns);
        }

        default:
            return {};
    }
}

std::string generate_create_statement_heuristic(
    const std::vector<RowValue>& sample_rows,
    const std::vector<catalog::ColumnInfo>& inferred_columns
) {
    std::string sql = "CREATE TABLE recovered_table (\n";

    for (size_t i = 0; i < inferred_columns.size(); i++) {
        const auto& col = inferred_columns[i];
        std::string col_name = "col_" + std::to_string(i);

        // Generate column type based on inferred type
        std::string col_type;
        switch (col.mysql_type_id) {
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
                col_type = "VARCHAR(255)";
                break;
            case 253:  // VAR_STRING
                col_type = "VARCHAR(65535)";
                break;
            case 252:  // BLOB
                col_type = "TEXT";
                break;
            default:
                col_type = "BINARY(16)";
                break;
        }

        sql += "  `" + col_name + "` " + col_type;

        // Check if all values are NULL (nullable)
        bool all_null = true;
        for (const auto& row : sample_rows) {
            if (row.has_value()) {
                all_null = false;
                break;
            }
        }

        if (!all_null) {
            sql += " NOT NULL";
        }

        if (i < inferred_columns.size() - 1) {
            sql += ",";
        }
        sql += "\n";
    }

    sql += ")";

    return sql;
}

} // namespace innodb
} // namespace dbrecovery
