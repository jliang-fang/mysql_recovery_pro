#include "dbrecovery/innodb/page_parser_v2.hpp"
#include "dbrecovery/innodb/sdi_manager.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <set>
#include <sstream>

namespace dbrecovery {
namespace innodb {

// ==================== PageParserFactory 实现 ====================

std::unique_ptr<IPageParserStrategy> PageParserFactory::create_parser(
    const uint8_t* data, size_t size) {

    // 1. 验证输入参数
    if (!data || size < 4096) {
        std::cerr << "[ERROR] Invalid input: data=" << (void*)data << ", size=" << size << std::endl;
        return nullptr;
    }

    // 2. 识别 MySQL 版本
    MySQLVersion version = detect_mysql_version(data, size);

    // 3. 识别行格式
    RowFormat format = detect_row_format(data, size);

    // 4. 根据版本创建对应的解析器
    switch (version) {
        case MySQLVersion::MYSQL_56:
        case MySQLVersion::MYSQL_57:
            return std::make_unique<MySQL56PageParser>(format);

        case MySQLVersion::MYSQL_80:
            return std::make_unique<MySQL80PageParser>(format);

        case MySQLVersion::MYSQL_84:
            return std::make_unique<MySQL84PageParser>(format);

        default:
            // 未知版本，使用通用解析器
            std::cerr << "[WARN] Unknown MySQL version (" << static_cast<int>(version)
                      << "), using generic parser" << std::endl;
            return std::make_unique<GenericPageParser>(version, format);
    }
}

MySQLVersion PageParserFactory::detect_mysql_version(const uint8_t* data, size_t size) {
    // 1. 验证输入参数
    if (!data || size < 38) {
        std::cerr << "[WARN] Data too small for version detection: " << size << " bytes" << std::endl;
        return MySQLVersion::UNKNOWN;
    }

    // 2. 首先尝试从 SDI 页识别版本
    uint16_t page_type = get_page_type(data);
    if (page_type == FIL_PAGE_SDI) {
        return detect_version_from_sdi(data, size);
    }

    // 3. 从页头识别版本
    return detect_version_from_header(data, size);
}

RowFormat PageParserFactory::detect_row_format(const uint8_t* data, size_t size) {
    // 验证输入参数
    if (!data || size < FIL_HEADER_SIZE + 44) {
        std::cerr << "[WARN] Data too small for row format detection: " << size << " bytes" << std::endl;
        return RowFormat::UNKNOWN;
    }

    // 从 BTR header 的 flags 中提取行格式
    uint16_t flags = read_le<uint16_t>(data, FIL_HEADER_SIZE + 42);
    uint8_t row_format_val = flags & 0x07;

    switch (row_format_val) {
        case 0: return RowFormat::REDUNDANT;
        case 1: return RowFormat::COMPACT;
        case 2: return RowFormat::DYNAMIC;
        case 3: return RowFormat::COMPRESSED;
        default: 
            std::cerr << "[WARN] Unknown row format value: " << static_cast<int>(row_format_val) << std::endl;
            return RowFormat::UNKNOWN;
    }
}

MySQLVersion PageParserFactory::detect_version_from_header(const uint8_t* data, size_t size) {
    uint16_t page_type = get_page_type(data);

    // 根据页类型和特征码判断版本
    if (page_type == FIL_PAGE_SDI) {
        // SDI 页只在 MySQL 8.0+ 存在
        return MySQLVersion::MYSQL_80;
    }

    // 检查页大小
    if (size == 16384) {
        // 16KB 页在所有版本中都存在
        // 需要检查其他特征
    }

    // 检查页头字段（MySQL 8.0+ 有更多字段）
    if (size >= FIL_HEADER_SIZE + 54) {
        // MySQL 8.0+ 有 PAGE_N_DIRECT_PAGES 字段
        uint16_t n_direct_pages = read_le<uint16_t>(data, FIL_HEADER_SIZE + 52);
        if (n_direct_pages > 0) {
            return MySQLVersion::MYSQL_80;
        }
    }

    // 默认返回 UNKNOWN（没有足够证据）
    return MySQLVersion::UNKNOWN;
}

MySQLVersion PageParserFactory::detect_version_from_sdi(const uint8_t* data, size_t size) {
    // SDI 页只在 MySQL 8.0+ 存在
    // 通过 SDI payload 中的版本信息进一步判断

    if (size < 58) { // 38 (FIL header) + 20 (SDI header minimum)
        return MySQLVersion::MYSQL_80; // 默认返回 8.0
    }

    // SDI payload 位于偏移 38
    const uint8_t* sdi_payload = data + 38;

    // SDI header: type(4) + id(8) + uncompressed_len(4) + compressed_len(4)
    uint32_t uncompressed_len = read_le<uint32_t>(sdi_payload, 12);

    if (uncompressed_len > 0 && size >= 58 + uncompressed_len) {
        // 尝试解压并解析 SDI JSON
        auto sdi_json_opt = SDIManager::extract_sdi_json(sdi_payload, size - 38);
        if (sdi_json_opt.has_value()) {
            try {
                auto json = nlohmann::json::parse(sdi_json_opt.value());

                // 检查 SDI 版本
                if (json.contains("version")) {
                    int sdi_version = json["version"];
                    if (sdi_version >= 200000) {
                        return MySQLVersion::MYSQL_84; // MySQL 8.4+
                    }
                    return MySQLVersion::MYSQL_80;
                }

                // 检查表结构中的新特性
                if (json.contains("schema") && json["schema"].contains("engine_attributes")) {
                    // engine_attributes 是 MySQL 8.0.20+ 的特性
                    return MySQLVersion::MYSQL_80;
                }

            } catch (...) {
                // JSON 解析失败，返回默认版本
            }
        }
    }

    return MySQLVersion::MYSQL_80;
}

// ==================== GenericPageParser 实现 ====================

GenericPageParser::GenericPageParser(MySQLVersion version, RowFormat format)
    : mysql_version_(version),
      row_format_(format),
      page_type_(0),
      page_level_(0),
      page_offset_(0),
      prev_page_(0xFFFFFFFF),
      next_page_(0xFFFFFFFF),
      n_dir_slots_(0),
      heap_top_(0),
      n_heap_(0),
      free_offset_(0),
      header_parsed_(false),
      records_parsed_(false),
      health_valid_(false) {
}

bool GenericPageParser::parse_header(const uint8_t* data, size_t size) {
    if (!data || size < FIL_HEADER_SIZE) {
        return false;
    }

    // 解析 FIL header
    if (!parse_fil_header(data, size)) {
        return false;
    }

    // 解析 page header
    if (!parse_page_header(data, size)) {
        return false;
    }

    // 解析 BTR header
    if (!parse_btr_header(data, size)) {
        return false;
    }

    header_parsed_ = true;
    return true;
}

bool GenericPageParser::parse_fil_header(const uint8_t* data, size_t size) {
    if (size < dbrecovery::innodb::FIL_HEADER_SIZE) {
        return false;
    }

    // FIL header fields are big-endian (canonical InnoDB format).
    page_type_ = read_be<uint16_t>(data, dbrecovery::innodb::FIL_PAGE_TYPE_ABS);   // offset 24
    page_offset_ = read_be<uint32_t>(data, dbrecovery::innodb::FIL_PAGE_OFFSET);   // offset 4
    prev_page_ = read_be<uint32_t>(data, dbrecovery::innodb::FIL_PAGE_PREV_ABS);   // offset 8
    next_page_ = read_be<uint32_t>(data, dbrecovery::innodb::FIL_PAGE_NEXT_ABS);   // offset 12

    return true;
}

bool GenericPageParser::parse_page_header(const uint8_t* data, size_t size) {
    if (size < FIL_HEADER_SIZE + 16) {
        return false;
    }

    const uint8_t* page_header = data + FIL_HEADER_SIZE;

    n_dir_slots_ = read_le<uint16_t>(page_header, 0);
    heap_top_ = read_le<uint16_t>(page_header, 2);
    n_heap_ = read_le<uint16_t>(page_header, 4) & 0x7FFF;
    free_offset_ = read_le<uint16_t>(page_header, 6);

    return true;
}

bool GenericPageParser::parse_btr_header(const uint8_t* data, size_t size) {
    if (size < FIL_HEADER_SIZE + 44) {
        return false;
    }

    const uint8_t* btr_header = data + FIL_HEADER_SIZE;

    page_level_ = read_le<uint16_t>(btr_header, 4);

    return true;
}

std::vector<Record> GenericPageParser::parse_records(const uint8_t* data, size_t size) {
    std::vector<Record> records;

    if (!header_parsed_) {
        return records;
    }

    // 解析记录偏移
    std::vector<uint16_t> record_offsets = parse_record_offsets(data, size);

    // 解析每条记录
    for (uint16_t offset : record_offsets) {
        if (offset + 5 > size) {
            continue;
        }

        Record record = parse_record(data, offset, size);
        records.push_back(record);
    }

    records_parsed_ = true;
    return records;
}

std::vector<uint16_t> GenericPageParser::parse_record_offsets(const uint8_t* data, size_t size) {
    std::vector<uint16_t> offsets;

    if (size < FIL_HEADER_SIZE + 36 + n_dir_slots_ * 2) {
        return offsets;
    }

    // 从页目录中提取记录偏移
    const uint8_t* dir = data + FIL_HEADER_SIZE + 36;

    // 从后往前遍历目录槽（InnoDB 目录槽按降序存储）
    for (int i = n_dir_slots_ - 1; i >= 0; --i) {
        uint16_t offset = read_le<uint16_t>(dir, i * 2) & 0x7FFF;
        if (offset < size) {
            offsets.push_back(offset);
        }
    }

    // 反转使其按升序排列
    std::reverse(offsets.begin(), offsets.end());

    return offsets;
}

Record GenericPageParser::parse_record(const uint8_t* data, size_t offset, size_t size) {
    Record record;
    record.offset = static_cast<uint16_t>(offset);

    if (offset + 5 > size) {
        return record;
    }

    uint8_t info_bits = data[offset];
    record.info_bits = info_bits;
    record.is_deleted = (info_bits & REC_INFO_DELETED_FLAG) != 0;
    record.n_fields = (info_bits & 0xF8) >> 3;

    // 读取 next_record_offset
    if (offset + 3 <= size) {
        record.next_offset = read_le<uint16_t>(data, offset + 1) & 0x3FFF;
    }

    // 读取 heap_no
    if (offset + 5 <= size) {
        record.heap_no = read_le<uint16_t>(data, offset + 3) & 0x7FFF;
    }

    return record;
}

bool GenericPageParser::validate_health() const {
    return true;
}

std::optional<catalog::TableSchema> GenericPageParser::parse_sdi_metadata(
    const uint8_t* data, size_t size) {

    if (page_type_ != FIL_PAGE_SDI) {
        return std::nullopt;
    }

    if (size < 58) {
        return std::nullopt;
    }

    const uint8_t* sdi_payload = data + 38;
    return SDIManager::parse_sdi_record(sdi_payload, size - 38);
}

std::pair<uint32_t, uint32_t> GenericPageParser::get_page_chain() const {
    return {prev_page_, next_page_};
}

uint16_t GenericPageParser::read_uint16_le(const uint8_t* data, size_t offset) const {
    return read_le<uint16_t>(data, offset);
}

uint32_t GenericPageParser::read_uint32_le(const uint8_t* data, size_t offset) const {
    return read_le<uint32_t>(data, offset);
}

uint64_t GenericPageParser::read_uint64_le(const uint8_t* data, size_t offset) const {
    return read_le<uint64_t>(data, offset);
}

// ==================== MySQL56PageParser 实现 ====================

MySQL56PageParser::MySQL56PageParser(RowFormat format)
    : GenericPageParser(MySQLVersion::MYSQL_57, format) {
}

bool MySQL56PageParser::parse_header(const uint8_t* data, size_t size) {
    // MySQL 5.6/5.7 特定的页头解析
    if (!GenericPageParser::parse_header(data, size)) {
        return false;
    }

    // MySQL 5.6/5.7 特有：检查系统字典页类型
    if (page_type_ == 0) {
        // 可能是系统字典页
    }

    return true;
}

std::vector<Record> MySQL56PageParser::parse_records(const uint8_t* data, size_t size) {
    // MySQL 5.6/5.7 特定的记录解析
    return GenericPageParser::parse_records(data, size);
}

std::optional<catalog::TableSchema> MySQL56PageParser::parse_sdi_metadata(
    const uint8_t* data, size_t size) {

    // MySQL 5.6/5.7 没有 SDI，元数据存储在系统表中
    return std::nullopt;
}

// ==================== MySQL80PageParser 实现 ====================

MySQL80PageParser::MySQL80PageParser(RowFormat format)
    : GenericPageParser(MySQLVersion::MYSQL_80, format),
      metadata_version_(0) {
}

bool MySQL80PageParser::parse_header(const uint8_t* data, size_t size) {
    // MySQL 8.0 特定的页头解析
    if (!GenericPageParser::parse_header(data, size)) {
        return false;
    }

    // MySQL 8.0 特有：Instant DDL 元数据
    parse_instant_ddl_metadata(data, size);

    return true;
}

std::vector<Record> MySQL80PageParser::parse_records(const uint8_t* data, size_t size) {
    // MySQL 8.0 特定的记录解析
    return GenericPageParser::parse_records(data, size);
}

std::optional<catalog::TableSchema> MySQL80PageParser::parse_sdi_metadata(
    const uint8_t* data, size_t size) {

    // MySQL 8.0 特有：SDI 元数据解析
    return GenericPageParser::parse_sdi_metadata(data, size);
}

bool MySQL80PageParser::parse_instant_ddl_metadata(const uint8_t* data, size_t size) {
    // MySQL 8.0 特有：Instant DDL 元数据
    // 位于 BTR header 之后
    if (size < FIL_HEADER_SIZE + 50) {
        return false;
    }

    const uint8_t* instant_ddl = data + FIL_HEADER_SIZE + 44;
    metadata_version_ = read_le<uint64_t>(instant_ddl, 0);

    return true;
}

PageParserV2::PageParserV2(const uint8_t* data, size_t size)
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

PageParseResult PageParserV2::parse() {
    // 1. 验证页大小
    if (size_ < 4096 || size_ > 65536) {
        result_.error_message = "Invalid page size: " + std::to_string(size_);
        return result_;
    }

    // 2. 创建解析器
    parser_ = PageParserFactory::create_parser(data_, size_);
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
    // Compute CRC32C (Castagnoli polynomial, bit-by-bit)
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 4; i < size_; i++) {
        crc ^= data_[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0x82F63B78) : (crc >> 1);
        }
    }
    result_.checksum_computed = crc ^ 0xFFFFFFFF;
    result_.checksum_valid = (stored_checksum == result_.checksum_computed);

    return result_;
}

PageParseResult PageParserV2::parse_as(MySQLVersion version) {
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

// ==================== PageScanner 实现 ====================

PageScanner::PageScanner(size_t page_size) : page_size_(page_size) {
}

std::vector<PageParseResult> PageScanner::scan_file(const std::string& file_path) {
    std::vector<PageParseResult> results;

    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Failed to open file: " << file_path << std::endl;
        return results;
    }

    // 获取文件大小
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 计算总页数
    uint32_t total_pages = static_cast<uint32_t>(file_size / page_size_);

    // 读取每一页
    std::vector<uint8_t> page_data(page_size_);
    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(page_no * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        // 检查是否是 InnoDB 页
        if (!is_innodb_page(page_data.data(), page_size_)) {
            continue;
        }

        // 解析页
        PageParserV2 parser(page_data.data(), page_size_);
        PageParseResult result = parser.parse();
        result.page_offset = page_no;

        results.push_back(result);
    }

    return results;
}

std::vector<PageParseResult> PageScanner::scan_buffer(const uint8_t* data, size_t size) {
    std::vector<PageParseResult> results;

    // 计算总页数
    uint32_t total_pages = static_cast<uint32_t>(size / page_size_);

    // 读取每一页
    for (uint32_t page_no = 0; page_no < total_pages; ++page_no) {
        const uint8_t* page_data = data + page_no * page_size_;

        // 检查是否是 InnoDB 页
        if (!is_innodb_page(page_data, page_size_)) {
            continue;
        }

        // 解析页
        PageParserV2 parser(page_data, page_size_);
        PageParseResult result = parser.parse();
        result.page_offset = page_no;

        results.push_back(result);
    }

    return results;
}

bool PageScanner::is_innodb_page(const uint8_t* data, size_t size) {
    if (size < 38) {
        return false;
    }

    // 检查页类型 - 支持 MySQL 5.6 和 8.0 的不同页类型值
    uint16_t page_type = get_page_type(data);
    
    // 使用 page_defs.hpp 中的 is_valid_page_type 函数
    // 它同时支持 MySQL 5.6 (0x45BC = 17852) 和 8.0 (0x26F1 = 9969) 的页类型
    return is_valid_page_type(page_type);
}

// ==================== PageChainReconstructor 实现 ====================

PageChainReconstructor::PageChainReconstructor() {
    stats_.total_pages = 0;
    stats_.valid_chain = 0;
    stats_.broken_chain = 0;
    stats_.circular_reference = 0;
}

void PageChainReconstructor::add_page(uint32_t page_no, const std::vector<uint8_t>& data) {
    pages_.emplace_back(page_no, data);
    page_map_[page_no] = data;
    stats_.total_pages++;
}

bool PageChainReconstructor::reconstruct() {
    if (pages_.empty()) {
        return false;
    }

    // 按页号排序
    std::sort(pages_.begin(), pages_.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    // 检查页链连续性
    for (size_t i = 0; i < pages_.size() - 1; ++i) {
        uint32_t current_page = pages_[i].first;
        uint32_t next_page = pages_[i + 1].first;

        // 检查是否连续
        if (next_page != current_page + 1) {
            stats_.broken_chain++;
        }
    }

    // 检查循环引用
    std::set<uint32_t> seen;
    for (const auto& [page_no, data] : pages_) {
        if (seen.find(page_no) != seen.end()) {
            stats_.circular_reference++;
            break;
        }
        seen.insert(page_no);
    }

    if (stats_.broken_chain == 0 && stats_.circular_reference == 0) {
        stats_.valid_chain = static_cast<uint32_t>(pages_.size());
        return true;
    }

    return false;
}

// ==================== PageHealthChecker 实现 ====================

// CRC32 计算
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32_table() {
    if (crc32_table_initialized) return;
    
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78; // CRC-32C 多项式
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}

uint32_t PageHealthChecker::compute_crc32(const uint8_t* data, size_t size) {
    init_crc32_table();
    
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

uint32_t PageHealthChecker::compute_innodb_checksum(const uint8_t* data, size_t size) {
    uint32_t checksum = 0;
    const uint32_t* words = reinterpret_cast<const uint32_t*>(data);
    size_t num_words = size / 4;
    
    for (size_t i = 0; i < num_words; i++) {
        checksum += words[i];
    }
    
    return checksum;
}

PageState PageHealthChecker::get_page_state(uint16_t page_type) {
    switch (page_type) {
        case FIL_PAGE_TYPE_ALLOCATED: return PageState::ALLOCATED;
        case FIL_PAGE_INDEX: return PageState::INDEX;
        case FIL_PAGE_UNDO_LOG: return PageState::UNDO_LOG;
        case FIL_PAGE_SDI: return PageState::SYSTEM;
        case FIL_PAGE_TYPE_BLOB: return PageState::BLOB;
        default: return PageState::SYSTEM;
    }
}

PageHealthChecker::HealthInfo PageHealthChecker::check(const uint8_t* data, size_t size) {
    HealthInfo info;
    info.is_valid = true;
    info.page_size = static_cast<uint32_t>(size);
    info.checksum_type = ChecksumType::CRC32;

    // 1. 检查页大小
    if (size < 4096 || size > 65536) {
        info.is_valid = false;
        info.status_message = "Invalid page size: " + std::to_string(size);
        return info;
    }

    // 2. 检查页头
    if (size < 38) {
        info.is_valid = false;
        info.status_message = "Page header too small";
        return info;
    }

    // 3. 检查页类型
    uint16_t page_type = get_page_type(data);
    info.page_state = get_page_state(page_type);
    
    if (page_type != FIL_PAGE_INDEX &&
        page_type != FIL_PAGE_SDI &&
        page_type != FIL_PAGE_UNDO_LOG &&
        page_type != FIL_PAGE_TYPE_ALLOCATED) {
        info.is_valid = false;
        info.status_message = "Unknown page type: " + std::to_string(page_type);
    }

    // 4. 检查 INFIMUM/SUPREMUM
    if (size >= 100) {
        info.has_infimum = true;
        info.has_supremum = true;
    }

    // 5. 检查目录槽
    if (size >= 74) {
        uint16_t n_dir_slots = read_le<uint16_t>(data, 38);
        info.n_dir_slots = n_dir_slots;
    }

    // 6. 检查堆大小
    if (size >= 42) {
        uint16_t n_heap = read_le<uint16_t>(data, 42) & 0x7FFF;
        info.n_records = n_heap - 2; // 减去 INFIMUM/SUPREMUM
    }

    // 7. 检查校验和
    uint32_t stored_checksum = 0, computed_checksum = 0;
    if (validate_checksum(data, size, &stored_checksum, &computed_checksum)) {
        info.checksum_match = true;
        info.checksum_expected = stored_checksum;
        info.checksum_actual = computed_checksum;
    } else {
        info.checksum_match = false;
    }

    return info;
}

bool PageHealthChecker::validate_checksum(const uint8_t* data, size_t size,
                                          uint32_t* stored, uint32_t* computed,
                                          ChecksumType* type) {
    if (size < 52) {
        return false;
    }

    // 读取存储的校验和 (at offset 0 in FIL header)
    *stored = read_le<uint32_t>(data, 0);

    // Compute CRC32C over page content (skip checksum field)
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 4; i < size; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0x82F63B78) : (crc >> 1);
        }
    }
    *computed = crc ^ 0xFFFFFFFF;

    return true;
}

bool PageHealthChecker::validate_infimum_supremum(const uint8_t* data, size_t size) {
    if (size < 100) {
        return false;
    }

    // 检查 INFIMUM 记录
    uint8_t infimum_info = data[54];
    uint8_t infimum_type = infimum_info & 0x07;

    if (infimum_type != REC_STATUS_INFIMUM) {
        return false;
    }

    // 检查 SUPREMUM 记录
    uint16_t supremum_offset = read_le<uint16_t>(data, 55) & 0x3FFF;
    if (supremum_offset >= size) {
        return false;
    }

    uint8_t supremum_info = data[supremum_offset];
    uint8_t supremum_type = supremum_info & 0x07;

    return supremum_type == REC_STATUS_SUPREMUM;
}

bool PageHealthChecker::validate_directory(const uint8_t* data, size_t size) {
    if (size < 74) {
        return false;
    }

    uint16_t n_dir_slots = read_le<uint16_t>(data, 38);
    if (n_dir_slots < 2) {
        return false;
    }

    return true;
}

// ==================== PageTypeDetector 实现 ====================

uint16_t PageTypeDetector::detect_page_type(const uint8_t* data, size_t size) {
    if (size < 38) {
        return 0;
    }

    return get_page_type(data);
}

MySQLVersion PageTypeDetector::detect_version(uint16_t page_type,
                                              const uint8_t* data, size_t size) {
    if (page_type == FIL_PAGE_SDI) {
        return MySQLVersion::MYSQL_80;
    }

    return MySQLVersion::MYSQL_57;
}

std::string PageTypeDetector::page_type_to_string(uint16_t type) {
    switch (type) {
        case FIL_PAGE_INDEX: return "FIL_PAGE_INDEX";
        case FIL_PAGE_SDI: return "FIL_PAGE_SDI";
        case FIL_PAGE_UNDO_LOG: return "FIL_PAGE_UNDO_LOG";
        case FIL_PAGE_TYPE_BLOB: return "FIL_PAGE_TYPE_BLOB";
        case FIL_PAGE_TYPE_ALLOCATED: return "FIL_PAGE_TYPE_ALLOCATED";
        case FIL_PAGE_COMPRESSED: return "FIL_PAGE_COMPRESSED";
        default: return "UNKNOWN";
    }
}

RowFormat PageTypeDetector::detect_row_format(const uint8_t* data, size_t size) {
    if (size < FIL_HEADER_SIZE + 44) {
        return RowFormat::UNKNOWN;
    }
    uint16_t flags = read_le<uint16_t>(data, FIL_HEADER_SIZE + 42);
    uint8_t row_format_val = flags & 0x07;
    switch (row_format_val) {
        case 0: return RowFormat::REDUNDANT;
        case 1: return RowFormat::COMPACT;
        case 2: return RowFormat::DYNAMIC;
        case 3: return RowFormat::COMPRESSED;
        default: return RowFormat::UNKNOWN;
    }
}

std::map<std::string, std::string> PageTypeDetector::detect_all_features(const uint8_t* data, size_t size) {
    std::map<std::string, std::string> features;
    
    if (size < 38) {
        features["error"] = "Data too small";
        return features;
    }

    uint16_t page_type = get_page_type(data);
    features["page_type"] = std::to_string(page_type);
    features["page_type_name"] = page_type_to_string(page_type);

    uint16_t page_level = read_le<uint16_t>(data, FIL_HEADER_SIZE + 4);
    features["page_level"] = std::to_string(page_level);

    uint32_t page_offset = read_le<uint32_t>(data, 20);
    features["page_offset"] = std::to_string(page_offset);

    uint32_t prev_page = read_le<uint32_t>(data, 24);
    uint32_t next_page = read_le<uint32_t>(data, 28);
    features["prev_page"] = std::to_string(prev_page);
    features["next_page"] = std::to_string(next_page);

    RowFormat format = detect_row_format(data, size);
    features["row_format"] = std::to_string(static_cast<int>(format));

    return features;
}

// ==================== MySQL84PageParser 实现 ====================

MySQL84PageParser::MySQL84PageParser(RowFormat format)
    : MySQL80PageParser(format) {
}

bool MySQL84PageParser::parse_header(const uint8_t* data, size_t size) {
    // MySQL 8.4 特定的页头解析
    if (!MySQL80PageParser::parse_header(data, size)) {
        return false;
    }

    // MySQL 8.4 特有：增强的加密和临时页支持
    // Note: parse_instant_ddl_metadata is inherited from MySQL80PageParser

    return true;
}

std::optional<catalog::TableSchema> MySQL84PageParser::parse_sdi_metadata(
    const uint8_t* data, size_t size) {

    // MySQL 8.4 特有：增强的 SDI 元数据解析
    return MySQL80PageParser::parse_sdi_metadata(data, size);
}

} // namespace innodb
} // namespace dbrecovery
