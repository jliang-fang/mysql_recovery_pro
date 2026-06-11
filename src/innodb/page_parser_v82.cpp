#include "dbrecovery/innodb/page_parser_v82.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>

namespace dbrecovery {
namespace innodb {

// ==================== MySQL82PageParser 实现 ====================

MySQL82PageParser::MySQL82PageParser(RowFormat format)
    : GenericPageParser(MySQLVersion::MYSQL_82, format),
      instant_ddl_version_(0),
      is_compressed_(false),
      zip_size_(0) {
}

bool MySQL82PageParser::parse_header(const uint8_t* data, size_t size) {
    if (!data || size < 4096) {
        return false;
    }

    // 解析 FIL header
    if (!parse_fil_header(data, size)) {
        return false;
    }

    // 解析 BTR header
    if (!parse_btr_header(data, size)) {
        return false;
    }

    // 解析 Instant DDL 元数据（MySQL 8.2+ 特有）
    parse_instant_ddl_metadata_v82(data, size);

    // 解析压缩信息
    is_compressed_ = (page_type_ == FIL_PAGE_COMPRESSED);
    if (is_compressed_) {
        if (size >= FIL_HEADER_SIZE + 26) {
            zip_size_ = read_le<uint16_t>(data, FIL_HEADER_SIZE + 24) & 0x0FFF;
        }
    }

    header_parsed_ = true;
    return true;
}

std::vector<Record> MySQL82PageParser::parse_records(const uint8_t* data, size_t size) {
    std::vector<Record> records;

    if (!data || size < 54) {
        return records;
    }

    // 解析页目录
    std::vector<uint16_t> dir_slots = parse_record_offsets(data, size);

    // 解析每个记录
    for (size_t i = 0; i < dir_slots.size(); i++) {
        uint16_t offset = dir_slots[i];
        if (offset < 54 || offset >= size) {
            continue;
        }

        Record record = parse_record(data + offset, offset, size - offset);
        if (record.offset > 0) {
            records.push_back(record);
        }
    }

    records_parsed_ = !records.empty();
    return records;
}

std::optional<catalog::TableSchema> MySQL82PageParser::parse_sdi_metadata(
    const uint8_t* data, size_t size) {
    
    // MySQL 8.2 使用与 8.0 相同的 SDI 格式
    return parse_sdi_metadata(data, size);
}

bool MySQL82PageParser::parse_instant_ddl_metadata_v82(const uint8_t* data, size_t size) {
    // Instant DDL 元数据位于 BTR header 之后
    // offset: FIL_HEADER_SIZE + 50 (BTR_INSTANT + n_fields)
    
    if (size < FIL_HEADER_SIZE + 54) {
        return false;
    }

    // 读取 Instant DDL 版本
    instant_ddl_version_ = read_le<uint64_t>(data, FIL_HEADER_SIZE + 50);
    
    return instant_ddl_version_ > 0;
}

// ==================== MySQL83PageParser 实现 ====================

MySQL83PageParser::MySQL83PageParser(RowFormat format)
    : MySQL82PageParser(format),
      parallel_degree_(0) {
}

bool MySQL83PageParser::parse_header(const uint8_t* data, size_t size) {
    if (!MySQL82PageParser::parse_header(data, size)) {
        return false;
    }

    // 解析并行查询元数据（MySQL 8.3+ 特有）
    parse_parallel_query_metadata(data, size);

    return true;
}

std::optional<catalog::TableSchema> MySQL83PageParser::parse_sdi_metadata(
    const uint8_t* data, size_t size) {
    
    // MySQL 8.3 使用增强的 SDI 格式
    return parse_sdi_metadata(data, size);
}

bool MySQL83PageParser::parse_parallel_query_metadata(const uint8_t* data, size_t size) {
    // 并行查询元数据位于页的特定位置
    // offset: FIL_HEADER_SIZE + 60
    
    if (size < FIL_HEADER_SIZE + 64) {
        return false;
    }

    // 读取并行度
    parallel_degree_ = read_le<uint32_t>(data, FIL_HEADER_SIZE + 60);
    
    return parallel_degree_ > 0;
}

// ==================== PageParserFactoryV82 实现 ====================

std::unique_ptr<IPageParserStrategy> PageParserFactoryV82::create_parser(
    const uint8_t* data, size_t size) {
    
    if (!data || size < 4096) {
        return nullptr;
    }

    // 检测版本
    MySQLVersion version = detect_mysql_version_v82(data, size);
    
    // 检测行格式
    RowFormat format = PageParserFactory::detect_row_format(data, size);
    
    // 创建解析器
    switch (version) {
        case MySQLVersion::MYSQL_81:
            return std::make_unique<MySQL82PageParser>(format);
            
        case MySQLVersion::MYSQL_82:
            return std::make_unique<MySQL82PageParser>(format);
            
        case MySQLVersion::MYSQL_83:
            return std::make_unique<MySQL83PageParser>(format);
            
        default:
            return std::make_unique<MySQL82PageParser>(format);
    }
}

MySQLVersion PageParserFactoryV82::detect_mysql_version_v82(
    const uint8_t* data, size_t size) {
    
    if (!data || size < 4096) {
        return MySQLVersion::UNKNOWN;
    }

    // 首先尝试从 SDI 识别
    MySQLVersion version = detect_version_from_sdi_v82(data, size);
    if (version != MySQLVersion::UNKNOWN) {
        return version;
    }

    // 从页头识别
    return detect_version_from_header_v82(data, size);
}

MySQLVersion PageParserFactoryV82::detect_version_from_sdi_v82(
    const uint8_t* data, size_t size) {
    
    // 检查 SDI 签名
    if (size < 72) {
        return MySQLVersion::UNKNOWN;
    }

    // MySQL 8.2/8.3 使用相同的 SDI 签名
    const uint8_t SDI_SIG[8] = {0x08, 'S', 'D', 'I', '0', '0', '0', '0'};
    
    if (std::memcmp(data + 64, SDI_SIG, 8) == 0) {
        // 检查是否有 MySQL 8.3 特有特征
        if (has_mysql83_features(data, size)) {
            return MySQLVersion::MYSQL_83;
        }
        return MySQLVersion::MYSQL_82;
    }

    return MySQLVersion::UNKNOWN;
}

MySQLVersion PageParserFactoryV82::detect_version_from_header_v82(
    const uint8_t* data, size_t size) {
    
    if (size < 42) {
        return MySQLVersion::UNKNOWN;
    }

    // 检查页头版本字段
    uint16_t header_version = read_le<uint16_t>(data, 40);
    if (header_version > 0) {
        // 检查是否有 MySQL 8.3 特有特征
        if (has_mysql83_features(data, size)) {
            return MySQLVersion::MYSQL_83;
        }
        return MySQLVersion::MYSQL_82;
    }

    return MySQLVersion::UNKNOWN;
}

bool PageParserFactoryV82::check_sdi_signature(
    const uint8_t* data, size_t size, const uint8_t* sig) {
    
    if (size < 72) {
        return false;
    }

    return std::memcmp(data + 64, sig, 8) == 0;
}

bool PageParserFactoryV82::has_mysql82_features(const uint8_t* data, size_t size) {
    // MySQL 8.2 特征：
    // 1. header_version > 0
    // 2. Instant DDL 元数据存在
    
    if (size < 42) {
        return false;
    }

    uint16_t header_version = read_le<uint16_t>(data, 40);
    if (header_version > 0) {
        return true;
    }

    // 检查 Instant DDL 元数据
    if (size >= FIL_HEADER_SIZE + 54) {
        uint64_t instant_version = read_le<uint64_t>(data, FIL_HEADER_SIZE + 50);
        if (instant_version > 0) {
            return true;
        }
    }

    return false;
}

bool PageParserFactoryV82::has_mysql83_features(const uint8_t* data, size_t size) {
    // MySQL 8.3 特征：
    // 1. header_version > 1
    // 2. 并行查询元数据存在
    
    if (size < 42) {
        return false;
    }

    uint16_t header_version = read_le<uint16_t>(data, 40);
    if (header_version > 1) {
        return true;
    }

    // 检查并行查询元数据
    if (size >= FIL_HEADER_SIZE + 64) {
        uint32_t parallel_degree = read_le<uint32_t>(data, FIL_HEADER_SIZE + 60);
        if (parallel_degree > 0) {
            return true;
        }
    }

    return false;
}

// ==================== BTreeChainReconstructor 实现 ====================

BTreeChainReconstructor::BTreeChainReconstructor()
    : stats_{} {
    stats_.total_pages = 0;
    stats_.valid_chain = 0;
    stats_.broken_chain = 0;
    stats_.circular_reference = 0;
    stats_.grafts_performed = 0;
}

void BTreeChainReconstructor::add_page(uint32_t page_no, const std::vector<uint8_t>& data) {
    pages_.push_back({page_no, data});
    page_map_[page_no] = data;
    stats_.total_pages++;
}

bool BTreeChainReconstructor::reconstruct() {
    if (page_map_.empty()) {
        return false;
    }

    // 首先尝试从页链重建
    if (build_chain_from_map()) {
        stats_.valid_chain++;
        return true;
    }

    // 尝试检测和修复断裂链
    if (detect_and_fix_broken_chain()) {
        stats_.broken_chain++;
        return true;
    }

    return false;
}

bool BTreeChainReconstructor::reconstruct_from_page(uint32_t start_page) {
    if (page_map_.find(start_page) == page_map_.end()) {
        return false;
    }

    // 从指定页开始重建
    pages_.clear();
    uint32_t current_page = start_page;

    while (current_page != 0xFFFFFFFF && page_map_.find(current_page) != page_map_.end()) {
        // 检查循环引用
        bool found = false;
        for (const auto& [pn, pd] : pages_) {
            if (pn == current_page) {
                found = true;
                break;
            }
        }
        if (found) {
            stats_.circular_reference++;
            break;
        }

        pages_.push_back({current_page, page_map_[current_page]});
        
        // 获取下一页
        const uint8_t* data = page_map_[current_page].data();
        if (page_map_[current_page].size() < 32) {
            break;
        }

        current_page = read_be<uint32_t>(data, FIL_PAGE_NEXT_ABS);  // FIL_PAGE_NEXT at offset 12
    }

    stats_.valid_chain++;
    return true;
}

bool BTreeChainReconstructor::build_chain_from_map() {
    if (page_map_.empty()) {
        return false;
    }

    // 尝试找到根页（通常是 page 0 或 page 3）
    uint32_t root_page = 0;
    if (page_map_.find(0) != page_map_.end()) {
        root_page = 0;
    } else if (page_map_.find(3) != page_map_.end()) {
        root_page = 3;
    } else {
        // 使用最小页号作为根页
        root_page = page_map_.begin()->first;
    }

    return reconstruct_from_page(root_page);
}

bool BTreeChainReconstructor::detect_and_fix_broken_chain() {
    if (page_map_.size() <= 1) {
        return false;
    }

    // 检测断裂点
    for (const auto& [page_no, data] : page_map_) {
        if (data.size() < 32) {
            continue;
        }

        uint32_t next_page = read_be<uint32_t>(data.data(), FIL_PAGE_NEXT_ABS);  // FIL_PAGE_NEXT at offset 12
        if (next_page != 0xFFFFFFFF && page_map_.find(next_page) == page_map_.end()) {
            broken_points_.push_back(page_no);
        }
    }

    if (broken_points_.empty()) {
        return false;
    }

    // 尝试修复断裂点
    for (uint32_t broken_page : broken_points_) {
        if (page_map_.find(broken_page) == page_map_.end()) {
            continue;
        }

        const uint8_t* data = page_map_[broken_page].data();
        if (page_map_[broken_page].size() < 32) {
            continue;
        }

        uint32_t expected_next = read_be<uint32_t>(data, FIL_PAGE_NEXT_ABS);
        
        // 尝试找到下一个页（通过页号顺序）
        for (const auto& [page_no, page_data] : page_map_) {
            if (page_no > broken_page && page_no < expected_next + 10) {
                // 可能是下一个页
                graft_pages(broken_page, page_no);
                stats_.grafts_performed++;
                break;
            }
        }
    }

    return !broken_points_.empty();
}

void BTreeChainReconstructor::graft_pages(uint32_t from_page, uint32_t to_page) {
    if (page_map_.find(from_page) == page_map_.end() ||
        page_map_.find(to_page) == page_map_.end()) {
        return;
    }

    // 修改 from_page 的 next_page 指针
    std::vector<uint8_t>& data = page_map_[from_page];
    if (data.size() < 32) {
        return;
    }

    write_be<uint32_t>(data.data(), FIL_PAGE_NEXT_ABS, to_page);  // FIL_PAGE_NEXT at offset 12
}

std::vector<uint32_t> BTreeChainReconstructor::get_reconstructed_chain() const {
    std::vector<uint32_t> chain;
    for (const auto& [page_no, data] : pages_) {
        chain.push_back(page_no);
    }
    return chain;
}

// ==================== UndoLogFlashback 实现 ====================

UndoLogFlashback::UndoLogFlashback(const uint8_t* data, size_t size)
    : data_(data), size_(size) {
}

std::vector<UndoLogFlashback::FlashbackEntry> UndoLogFlashback::parse_flashback(
    uint32_t first_page) {
    
    std::vector<FlashbackEntry> entries;
    
    uint32_t current_page = first_page;
    
    while (current_page != 0xFFFFFFFF && current_page < size_ / 16384) {
        const uint8_t* page_data = data_ + current_page * 16384;
        
        // 解析 Undo Log Header
        UndoLogHeader header = parse_undo_log_header(page_data);
        
        // 解析 Flashback 条目
        // 这是一个简化的实现
        // 在实际实现中，需要解析每个 undo log 记录
        
        // 移动到下一页
        current_page = header.next_page;
    }
    
    return entries;
}

std::vector<uint32_t> UndoLogFlashback::get_undo_log_pages() {
    std::vector<uint32_t> pages;
    
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

std::string UndoLogFlashback::generate_flashback_sql(
    const std::vector<FlashbackEntry>& entries) {
    
    std::string sql;
    
    for (const auto& entry : entries) {
        if (entry.type == 1) {  // TRX_UNDO_INSERT
            sql += "INSERT INTO table_name VALUES (...);\n";
        } else if (entry.type == 2) {  // TRX_UNDO_UPDATE
            sql += "UPDATE table_name SET ... WHERE ...;\n";
        } else if (entry.type == 3) {  // TRX_UNDO_DELETE
            sql += "DELETE FROM table_name WHERE ...;\n";
        }
    }
    
    return sql;
}

std::vector<std::vector<RowValue>> UndoLogFlashback::restore_data(
    const std::vector<FlashbackEntry>& entries) {
    
    std::vector<std::vector<RowValue>> rows;
    
    for (const auto& entry : entries) {
        // 解析记录数据
        std::vector<RowValue> row;
        
        // 这是一个简化的实现
        // 在实际实现中，需要解析每个字段
        
        rows.push_back(row);
    }
    
    return rows;
}

UndoLogFlashback::UndoLogHeader UndoLogFlashback::parse_undo_log_header(
    const uint8_t* page_data) {
    
    UndoLogHeader header{};
    
    header.state = read_le<uint16_t>(page_data, 0);
    header.direction = read_le<uint16_t>(page_data, 2);
    header.last_page = read_le<uint32_t>(page_data, 4);
    header.last_offset = read_le<uint16_t>(page_data, 8);
    header.prev_page = read_le<uint32_t>(page_data, 10);
    header.next_page = read_le<uint32_t>(page_data, 14);
    
    return header;
}

// ==================== InstantDDLParser 实现 ====================

InstantDDLParser::InstantDDLParser(const uint8_t* data, size_t size)
    : data_(data), size_(size) {
}

std::optional<InstantDDLParser::InstantMetadata> InstantDDLParser::parse_metadata(
    const uint8_t* data, size_t size) {
    
    if (size < FIL_HEADER_SIZE + 54) {
        return std::nullopt;
    }

    InstantMetadata metadata{};
    
    metadata.version = read_le<uint64_t>(data, FIL_HEADER_SIZE + 50);
    
    if (size >= FIL_HEADER_SIZE + 58) {
        metadata.added_columns = read_le<uint32_t>(data, FIL_HEADER_SIZE + 54);
    }
    
    if (size >= FIL_HEADER_SIZE + 62) {
        metadata.dropped_columns = read_le<uint32_t>(data, FIL_HEADER_SIZE + 58);
    }
    
    if (size >= FIL_HEADER_SIZE + 66) {
        metadata.modified_columns = read_le<uint32_t>(data, FIL_HEADER_SIZE + 62);
    }
    
    return metadata;
}

bool InstantDDLParser::is_instant_ddl_page(const uint8_t* data, size_t size) {
    if (size < FIL_HEADER_SIZE + 54) {
        return false;
    }

    uint64_t instant_version = read_le<uint64_t>(data, FIL_HEADER_SIZE + 50);
    return instant_version > 0;
}

uint64_t InstantDDLParser::get_instant_ddl_version(const uint8_t* data, size_t size) {
    if (size < FIL_HEADER_SIZE + 54) {
        return 0;
    }

    return read_le<uint64_t>(data, FIL_HEADER_SIZE + 50);
}

} // namespace innodb
} // namespace dbrecovery
