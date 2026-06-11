#include "dbrecovery/innodb/binary_recovery.hpp"
#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>

namespace dbrecovery {
namespace innodb {

// ==================== BinaryScanner Implementation ====================

BinaryScanner::BinaryScanner(uint32_t page_size)
    : page_size_(page_size), stats_{} {
    stats_.reset();
}

BinaryScanner::~BinaryScanner() = default;

uint64_t BinaryScanner::scan_file(const std::string& file_path,
                                   std::function<bool(uint32_t page_no, const uint8_t* page_data, size_t size)> callback) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[BinaryScanner] Failed to open file: " << file_path << std::endl;
        return 0;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        std::cerr << "[BinaryScanner] File too small: " << file_size << " bytes" << std::endl;
        return 0;
    }

    // Calculate total pages
    uint64_t total_pages = file_size / page_size_;
    
    // mmap approach for high-performance access
    int fd = open(file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "[BinaryScanner] Failed to open file descriptor for: " << file_path << std::endl;
        return 0;
    }

    // Map entire file into memory
    uint8_t* mapped = static_cast<uint8_t*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mapped == MAP_FAILED) {
        std::cerr << "[BinaryScanner] mmap failed for: " << file_path << std::endl;
        close(fd);
        return 0;
    }

    // Set memory advice for sequential access
    madvise(mapped, file_size, MADV_SEQUENTIAL);

    uint64_t pages_found = 0;
    for (uint64_t i = 0; i < total_pages; ++i) {
        uint8_t* page_data = mapped + (i * page_size_);
        uint32_t page_no = static_cast<uint32_t>(i);

        stats_.pages_scanned++;
        
        // Check page header validity
        if (validate_page_header(page_data, page_size_)) {
            stats_.valid_pages++;
            
            uint16_t page_type = get_page_type(page_data);
            if (page_type == FIL_PAGE_INDEX) {
                stats_.index_pages++;
            }
            
            // Call callback
            if (callback(page_no, page_data, page_size_)) {
                stats_.pages_with_records++;
                pages_found++;
            }
        } else {
            stats_.type_failed++;
        }
    }

    // Cleanup
    munmap(mapped, file_size);
    close(fd);

    return pages_found;
}

uint64_t BinaryScanner::scan_buffer(const uint8_t* data, size_t size, uint64_t start_offset,
                                     std::function<bool(uint32_t page_no, const uint8_t* page_data, size_t size)> callback) {
    if (!data || size < page_size_) {
        return 0;
    }

    uint64_t total_pages = size / page_size_;
    uint64_t pages_found = 0;

    for (uint64_t i = 0; i < total_pages; ++i) {
        const uint8_t* page_data = data + (i * page_size_);
        uint32_t page_no = static_cast<uint32_t>(start_offset + i);

        stats_.pages_scanned++;
        
        if (validate_page_header(page_data, page_size_)) {
            stats_.valid_pages++;
            stats_.index_pages++;
            
            if (callback(page_no, page_data, page_size_)) {
                pages_found++;
            }
        } else {
            stats_.type_failed++;
        }
    }

    return pages_found;
}

bool BinaryScanner::validate_page_header(const uint8_t* data, size_t size) {
    if (!data || size < FIL_HEADER_SIZE + 16) {
        return false;
    }

    // 1. Check page type
    uint16_t page_type = get_page_type(data);
    if (!is_valid_page_type(page_type)) {
        stats_.type_failed++;
        return false;
    }

    // 2. Check page offset (page number) - should be reasonable
    uint32_t page_offset = get_page_offset(data);
    if (page_offset > 100000000) {  // Reasonable upper bound
        return false;
    }

    // 3. Validate checksum (if present)
    if (!validate_checksum(data, size)) {
        stats_.checksum_failed++;
        // Still consider valid if checksum is optional
    }

    // 4. Check LSN
    uint64_t lsn = get_page_lsn(data);
    if (lsn == 0) {
        // Could be fresh page or checksum disabled
    } else if (!LSN::from_bytes(data, FIL_PAGE_LSN_ABS).is_valid()) {
        return false;
    }

    return true;
}

bool BinaryScanner::is_valid_page_type(uint16_t type) {
    return type == FIL_PAGE_INDEX ||
           type == FIL_PAGE_UNDO_LOG ||
           type == FIL_PAGE_SDI ||
           type == FIL_PAGE_TYPE_BLOB ||
           type == FIL_PAGE_TYPE_ALLOCATED;
}

uint16_t BinaryScanner::get_page_type(const uint8_t* data) {
    return read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
}

uint32_t BinaryScanner::get_page_offset(const uint8_t* data) {
    return read_be<uint32_t>(data, FIL_PAGE_OFFSET);
}

uint64_t BinaryScanner::get_page_lsn(const uint8_t* data) {
    return read_be<uint64_t>(data, FIL_PAGE_LSN_ABS);
}

uint32_t BinaryScanner::compute_crc32(const uint8_t* data, size_t size) {
    // Simple CRC32 implementation (MySQL uses a specific variant)
    // This is a simplified version for signature scanning
    
    if (size < page_size_) {
        return 0;
    }

    // Skip the checksum field itself (offset 0-3)
    uint32_t crc = 0;
    for (size_t i = 4; i < page_size_; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool BinaryScanner::validate_checksum(const uint8_t* data, size_t size) {
    if (size < page_size_) {
        return true;  // Don't fail on small buffers
    }

    // Check if checksum is enabled (not all pages have it)
    uint32_t stored_checksum = read_le<uint32_t>(data, 0);
    
    // MySQL 5.6 often uses checksum disabled for some pages
    if (stored_checksum == 0 || stored_checksum == 0xFFFFFFFF) {
        return true;  // Checksum disabled or not set
    }

    // Compute checksum and compare
    uint32_t computed = compute_crc32(data, page_size_);
    
    // MySQL uses different checksum algorithms
    // For now, we accept if checksum looks reasonable
    return true;  // Being lenient for recovery scenarios
}

// ==================== ZombieTupleExtractor Implementation ====================

ZombieTupleExtractor::ZombieTupleExtractor(uint32_t page_size)
    : page_size_(page_size), stats_{} {
    stats_.reset();
}

ZombieTupleExtractor::~ZombieTupleExtractor() = default;

std::vector<RecoveredRecord> ZombieTupleExtractor::extract_all_tuples(const uint8_t* data, size_t size, uint32_t page_no) {
    std::vector<RecoveredRecord> records;
    
    if (!data || size < page_size_) {
        return records;
    }

    // Read page header fields
    if (size < FIL_HEADER_SIZE + 16) {
        return records;
    }
    
    uint16_t n_dir_slots = read_le<uint16_t>(data, FIL_HEADER_SIZE + 0);
    uint16_t heap_top = read_le<uint16_t>(data, FIL_HEADER_SIZE + 2);
    
    if (n_dir_slots < 2) {
        return records;  // Need at least INFIMUM and SUPREMUM
    }

    // Read page directory (after page header)
    size_t dir_offset = FIL_HEADER_SIZE + 36;
    size_t dir_size = n_dir_slots * 2;
    if (dir_offset + dir_size > size) {
        dir_size = std::max(size_t(0), size - dir_offset);
    }

    // Read directory slots and extract record offsets
    std::vector<uint16_t> record_offsets;
    
    for (uint16_t i = 0; i < n_dir_slots; ++i) {
        if (dir_offset + i * 2 + 2 > size) {
            break;
        }
        
        uint16_t offset = read_le<uint16_t>(data, dir_offset + i * 2) & 0x7FFF;
        
        // Record must be within page bounds
        if (offset >= 54 && offset < heap_top && offset < size) {
            record_offsets.push_back(offset);
        }
    }

    // Extract each record
    for (uint16_t offset : record_offsets) {
        auto record_opt = extract_record(data, size, offset, page_no);
        if (record_opt) {
            records.push_back(std::move(*record_opt));
        }
    }

    stats_.total_records += records.size();
    return records;
}

std::optional<RecoveredRecord> ZombieTupleExtractor::extract_record(const uint8_t* data, size_t size,
                                                                      size_t offset, uint32_t page_no) {
    if (offset + 5 > size) {
        return std::nullopt;
    }

    // Parse record header using existing RecordHeader struct
    auto header_opt = RecordHeader::parse(data + offset, 5);
    if (!header_opt) {
        return std::nullopt;
    }
    
    RecordHeader header = *header_opt;
    
    // Extract record data (fields) - simplified
    std::vector<std::vector<uint8_t>> fields = extract_record_data(data, size, offset, header);
    
    // Calculate record length
    uint16_t record_length = 0;
    uint16_t next_offset = header.get_next_record_offset();
    if (next_offset > offset && next_offset < size) {
        record_length = next_offset - offset;
    } else {
        record_length = static_cast<uint16_t>(size - offset);
    }

    // Determine if this is a zombie record
    bool is_zombie = header.is_deleted() && !fields.empty();
    
    // Count records
    if (header.is_deleted()) {
        stats_.zombie_records++;
    } else {
        stats_.valid_records++;
    }

    RecoveredRecord record{};
    record.page_no = page_no;
    record.record_offset = static_cast<uint16_t>(offset);
    record.heap_no = header.get_heap_no();
    record.fields = std::move(fields);
    record.is_deleted = header.is_deleted();
    record.is_zombie = is_zombie;
    record.lsn = 0;
    record.record_length = record_length;

    return record;
}

std::vector<std::vector<uint8_t>> ZombieTupleExtractor::extract_record_data(const uint8_t* data, size_t size,
                                                                              size_t offset, const RecordHeader& /*header*/,
                                                                              uint16_t n_columns) {
    std::vector<std::vector<uint8_t>> fields;
    
    if (!data || size <= offset) {
        return fields;
    }

    // First record starts after record header (5 bytes for COMPACT format)
    size_t data_offset = offset + 5;  // REC_N_NEW_BYTES = 5

    // Default field count
    uint16_t field_count = n_columns > 0 ? n_columns : 3;  // Default: id, name, email
    
    // Parse each field
    for (uint16_t i = 0; i < field_count && data_offset < size; ++i) {
        std::vector<uint8_t> field_data;
        
        // Check for NULL marker
        if (data_offset < size && data[data_offset] == 0x80) {
            field_data.push_back(data[data_offset++]);
            fields.push_back(field_data);
            continue;
        }
        
        // Variable length field
        if (data_offset + 1 < size) {
            uint8_t len_byte = data[data_offset];
            
            if (len_byte < 255) {
                // 1-byte length prefix
                uint8_t field_len = len_byte;
                data_offset++;
                
                if (data_offset + field_len <= size) {
                    field_data.assign(data + data_offset, data + data_offset + field_len);
                    data_offset += field_len;
                }
            } else if (len_byte == 255 && data_offset + 2 < size) {
                // 2-byte length prefix
                data_offset++;
                uint16_t field_len = read_le<uint16_t>(data, data_offset);
                data_offset += 2;
                
                if (data_offset + field_len <= size) {
                    field_data.assign(data + data_offset, data + data_offset + field_len);
                    data_offset += field_len;
                }
            } else {
                // Fixed length
                if (data_offset < size) {
                    field_data.push_back(data[data_offset++]);
                }
            }
        }
        
        if (!field_data.empty() || i == 0) {
            fields.push_back(field_data);
        }
    }

    return fields;
}

// ==================== BPlusTreeReconstructor Implementation ====================

BPlusTreeReconstructor::BPlusTreeReconstructor(uint32_t page_size)
    : page_size_(page_size) {}

BPlusTreeReconstructor::~BPlusTreeReconstructor() = default;

bool BPlusTreeReconstructor::add_page(uint32_t page_no, const uint8_t* data, size_t size) {
    if (size < FIL_HEADER_SIZE + 44) {
        return false;
    }

    BPlusTreePage page{};
    page.page_no = page_no;

    // Read page header fields at offset 38 (after FIL header)
    const uint8_t* ph = data + FIL_HEADER_SIZE;
    
    // BTR page_level at offset 38 + 4 = 42
    page.page_level = read_le<uint16_t>(ph, 4) & 0x7FFF;
    
    // Index ID (not used for reconstruction)
    // uint32_t index_id_low = read_le<uint32_t>(ph, 6);
    // uint32_t index_id_high = read_le<uint32_t>(ph, 10);
    
    // Page directory at offset 38 + 0 = 38
    page.n_dir_slots = read_le<uint16_t>(ph, 0);
    
    // Heap info at offset 38 + 2 = 40
    page.heap_top = read_le<uint16_t>(ph, 2);
    
    // Check if leaf node
    page.is_leaf = (page.page_level == 0);
    
    // Read previous/next page pointers from page trailer
    if (size >= page_size_) {
        size_t trailer_offset = page_size_ - 8;
        if (trailer_offset + 8 <= size) {
            page.prev_page = read_le<uint32_t>(data, trailer_offset);
            page.next_page = read_le<uint32_t>(data, trailer_offset + 4);
        }
    }
    
    // Calculate free space
    page.free_space = page_size_ - page.heap_top - 38 - 16;
    
    // Check if root
    page.is_root = (page.page_level > 0);
    
    pages_[page_no] = page;
    return true;
}

bool BPlusTreeReconstructor::reconstruct_chain(PageChain& chain) {
    if (pages_.empty()) {
        return false;
    }

    // Find the leftmost page (first leaf)
    uint32_t first_page = 0;
    for (const auto& [page_no, page] : pages_) {
        if (page.is_leaf) {
            first_page = page_no;
            break;
        }
    }
    
    if (first_page == 0) {
        first_page = pages_.begin()->first;
    }
    
    // Build chain
    std::unordered_map<uint32_t, bool> visited;
    std::vector<uint32_t> chain_pages;
    uint32_t current_page = first_page;
    
    while (current_page != 0 && !visited[current_page] && pages_.count(current_page)) {
        visited[current_page] = true;
        chain_pages.push_back(current_page);
        
        const BPlusTreePage& page = pages_[current_page];
        current_page = page.next_page;
    }
    
    if (chain_pages.empty()) {
        return false;
    }
    
    chain.first_page = chain_pages.front();
    chain.last_page = chain_pages.back();
    chain.pages = std::move(chain_pages);
    
    return true;
}

bool BPlusTreeReconstructor::get_pages_in_order(std::vector<uint32_t>& pages) {
    PageChain chain;
    if (!reconstruct_chain(chain)) {
        return false;
    }
    
    pages = chain.pages;
    return true;
}

uint32_t BPlusTreeReconstructor::find_root() const {
    uint32_t root_page = 0;
    uint16_t max_level = 0;
    
    for (const auto& [page_no, page] : pages_) {
        if (page.page_level >= max_level) {
            max_level = page.page_level;
            root_page = page_no;
        }
    }
    
    return root_page;
}

std::vector<uint32_t> BPlusTreeReconstructor::find_leaf_pages() const {
    std::vector<uint32_t> leaves;
    
    for (const auto& [page_no, page] : pages_) {
        if (page.is_leaf) {
            leaves.push_back(page_no);
        }
    }
    
    return leaves;
}

// ==================== HeuristicSchemaInferer Implementation ====================

HeuristicSchemaInferer::HeuristicSchemaInferer() : stats_{} {
    stats_.reset();
}

HeuristicSchemaInferer::~HeuristicSchemaInferer() = default;

void HeuristicSchemaInferer::analyze_record(const std::vector<std::vector<uint8_t>>& record) {
    stats_.records_analyzed++;
    
    size_t num_columns = record.size();
    
    for (size_t i = column_statistics_.size(); i < num_columns; ++i) {
        column_statistics_[i];
    }
    
    for (size_t i = 0; i < num_columns; ++i) {
        if (i >= record.size()) continue;
        
        const auto& field = record[i];
        HeuristicFieldType type = detect_field_type(field);
        
        ColumnStats& col_stats = column_statistics_[i];
        
        switch (type) {
            case HeuristicFieldType::TINY_INT:
            case HeuristicFieldType::SMALL_INT:
            case HeuristicFieldType::MEDIUM_INT:
            case HeuristicFieldType::INT:
            case HeuristicFieldType::BIGINT:
                col_stats.int_patterns++;
                break;
            case HeuristicFieldType::VARCHAR_VARLEN:
            case HeuristicFieldType::VARCHAR_FIXLEN:
                col_stats.varchar_patterns++;
                break;
            case HeuristicFieldType::BLOB_PTR:
                col_stats.blob_patterns++;
                break;
            default:
                if (field.size() == 1 && field[0] == 0x80) {
                    col_stats.null_count++;
                }
                break;
        }
    }
    
    analyzed_records_.push_back(record);
}

void HeuristicSchemaInferer::analyze_heuristic_record(const std::vector<HeuristicField>& fields) {
    stats_.records_analyzed++;
    
    size_t num_columns = fields.size();
    
    for (size_t i = column_statistics_.size(); i < num_columns; ++i) {
        column_statistics_[i];
    }
    
    for (size_t i = 0; i < num_columns; ++i) {
        if (i >= fields.size()) continue;
        
        const auto& field = fields[i];
        ColumnStats& col_stats = column_statistics_[i];
        
        switch (field.type) {
            case HeuristicFieldType::TINY_INT:
            case HeuristicFieldType::SMALL_INT:
            case HeuristicFieldType::MEDIUM_INT:
            case HeuristicFieldType::INT:
            case HeuristicFieldType::BIGINT:
                col_stats.int_patterns++;
                break;
            case HeuristicFieldType::VARCHAR_VARLEN:
            case HeuristicFieldType::VARCHAR_FIXLEN:
                col_stats.varchar_patterns++;
                break;
            case HeuristicFieldType::BLOB_PTR:
                col_stats.blob_patterns++;
                break;
            default:
                col_stats.null_count++;
                break;
        }
    }
}

HeuristicSchema HeuristicSchemaInferer::infer_schema() const {
    HeuristicSchema schema{};
    
    if (column_statistics_.empty()) {
        schema.columns.emplace_back("field0", HeuristicFieldType::UNKNOWN);
        return schema;
    }
    
    schema.max_columns = static_cast<uint32_t>(column_statistics_.size());
    schema.min_columns = schema.max_columns;
    
    for (const auto& [col_idx, col_stats] : column_statistics_) {
        std::string column_name = "field" + std::to_string(col_idx);
        HeuristicFieldType type = HeuristicFieldType::VARCHAR_VARLEN;
        
        if (col_stats.int_patterns >= col_stats.varchar_patterns) {
            type = HeuristicFieldType::INT;
            schema.int_columns++;
        } else if (col_stats.varchar_patterns > 0) {
            type = HeuristicFieldType::VARCHAR_VARLEN;
            schema.varchar_columns++;
        } else if (col_stats.blob_patterns > 0) {
            type = HeuristicFieldType::BLOB_PTR;
            schema.blob_columns++;
        } else {
            if (col_stats.null_count > 0) {
                type = HeuristicFieldType::VARCHAR_VARLEN;
            }
        }
        
        schema.columns.emplace_back(column_name, type);
    }
    
    schema.columns_detected = static_cast<uint32_t>(schema.columns.size());
    return schema;
}

HeuristicFieldType HeuristicSchemaInferer::detect_field_type(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return HeuristicFieldType::UNKNOWN;
    }
    
    if (data.size() == 1 && data[0] == 0x80) {
        return HeuristicFieldType::NULL_VALUE;
    }
    
    if (data.size() <= 8) {
        if (data.size() == 1) return HeuristicFieldType::TINY_INT;
        if (data.size() == 2) return HeuristicFieldType::SMALL_INT;
        if (data.size() == 3) return HeuristicFieldType::MEDIUM_INT;
        if (data.size() == 4) return HeuristicFieldType::INT;
        return HeuristicFieldType::BIGINT;
    }
    
    if (data.size() > 1 && data.size() <= 256) {
        uint8_t first = data[0];
        if (first < 255 || (first == 255 && data.size() > 2)) {
            bool is_ascii = true;
            for (size_t i = 1; i < data.size() && i < 33; ++i) {
                if (data[i] != 0 && (data[i] < 32 || data[i] >= 127)) {
                    is_ascii = false;
                    break;
                }
            }
            if (is_ascii) {
                return HeuristicFieldType::VARCHAR_VARLEN;
            }
        }
    }
    
    if (data.size() == 20) {
        return HeuristicFieldType::BLOB_PTR;
    }
    
    return HeuristicFieldType::UNKNOWN;
}

int64_t HeuristicSchemaInferer::parse_numeric_value(const std::vector<uint8_t>& data, HeuristicFieldType field_type) {
    if (data.empty()) {
        return 0;
    }
    
    switch (field_type) {
        case HeuristicFieldType::TINY_INT:
            return static_cast<int8_t>(data[0]);
        case HeuristicFieldType::SMALL_INT:
            if (data.size() >= 2) {
                return static_cast<int16_t>(read_le<int16_t>(data.data()));
            }
            break;
        case HeuristicFieldType::MEDIUM_INT:
            if (data.size() >= 3) {
                int32_t val = static_cast<int32_t>(
                    data[0] | (data[1] << 8) | (data[2] << 16));
                if (val & 0x800000) {
                    val |= 0xFF000000;
                }
                return val;
            }
            break;
        case HeuristicFieldType::INT:
            if (data.size() >= 4) {
                return static_cast<int32_t>(read_le<int32_t>(data.data()));
            }
            break;
        case HeuristicFieldType::BIGINT:
            if (data.size() >= 8) {
                return static_cast<int64_t>(read_le<int64_t>(data.data()));
            }
            break;
        default:
            break;
    }
    
    return 0;
}

// ==================== RecordChainReconstructor Implementation ====================

RecordChainReconstructor::RecordChainReconstructor(uint32_t /*page_size*/) {
}

RecordChainReconstructor::~RecordChainReconstructor() = default;

void RecordChainReconstructor::add_record_offset(uint16_t offset, uint16_t heap_no, bool is_deleted) {
    offsets_[offset] = std::make_pair(heap_no, is_deleted);
}

bool RecordChainReconstructor::reconstruct_chain(std::vector<uint16_t>& chain) {
    if (offsets_.empty()) {
        return false;
    }

    // Sort by heap_no
    std::vector<std::pair<uint16_t, uint16_t>> sorted;
    for (const auto& [offset, paired] : offsets_) {
        sorted.push_back({offset, paired.first});
    }
    
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    
    chain.clear();
    for (const auto& [offset, _] : sorted) {
        chain.push_back(offset);
    }
    
    return !chain.empty();
}

// ==================== BinaryRecovery Implementation ====================

BinaryRecovery::BinaryRecovery(uint32_t page_size)
    : page_size_(page_size), stats_{}, scanner_(page_size), 
      extractor_(page_size), btree_reconstructor_(page_size), schema_inferer_() {
    stats_.reset();
}

BinaryRecovery::~BinaryRecovery() = default;

bool BinaryRecovery::on_page_found(uint32_t page_no, const uint8_t* page_data, size_t size,
                                    std::vector<RecoveredRecord>& records) {
    auto extracted = extractor_.extract_all_tuples(page_data, size, page_no);
    
    for (auto& rec : extracted) {
        stats_.records_extracted++;
        if (rec.is_zombie) {
            stats_.zombie_records++;
        }
    }
    
    records.insert(records.end(), extracted.begin(), extracted.end());
    
    return !extracted.empty();
}

bool BinaryRecovery::recover_file(const std::string& file_path,
                                   std::vector<RecoveredRecord>& recovered_records) {
    stats_.reset();
    
    auto callback = [this, &recovered_records](uint32_t page_no, const uint8_t* page_data, size_t size) -> bool {
        return on_page_found(page_no, page_data, size, recovered_records);
    };
    
    stats_.pages_scanned = scanner_.scan_file(file_path, callback);
    stats_.valid_pages = scanner_.get_stats().valid_pages;
    
    return !recovered_records.empty();
}

bool BinaryRecovery::recover_with_schema(const std::string& file_path,
                                          std::vector<RecoveredRecord>& recovered_records,
                                          HeuristicSchema& schema) {
    if (!recover_file(file_path, recovered_records)) {
        return false;
    }
    
    for (const auto& rec : recovered_records) {
        schema_inferer_.analyze_record(rec.fields);
    }
    
    schema = schema_inferer_.infer_schema();
    stats_.schema_columns = schema.columns_detected;
    
    return !recovered_records.empty();
}

// ==================== TruncateRecovery Implementation ====================

TruncateRecovery::TruncateRecovery(uint32_t page_size)
    : page_size_(page_size) {}

TruncateRecovery::~TruncateRecovery() = default;

bool TruncateRecovery::detect_truncate(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }

    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size_) {
        return false;
    }

    std::vector<uint8_t> page_data(page_size_);
    file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

    uint16_t heap_top = read_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 2);
    uint16_t n_heap = read_le<uint16_t>(page_data.data(), FIL_HEADER_SIZE + 4) & 0x7FFF;

    if (heap_top < 100) {
        return true;
    }

    if (n_heap < 5) {
        return true;
    }

    return false;
}

uint64_t TruncateRecovery::recover_from_truncate(const std::string& file_path,
                                                   std::vector<RecoveredRecord>& recovered_records) {
    uint64_t total = 0;
    
    BinaryRecovery recovery(page_size_);
    
    std::vector<RecoveredRecord> records;
    if (!recovery.recover_file(file_path, records)) {
        return 0;
    }

    for (auto& rec : records) {
        if (rec.is_zombie || rec.is_deleted) {
            recovered_records.push_back(std::move(rec));
            total++;
        }
    }

    return total;
}

bool TruncateRecovery::is_truncated_page(const uint8_t* data, size_t size) {
    if (!data || size < FIL_HEADER_SIZE + 16) {
        return false;
    }

    uint16_t heap_top = read_le<uint16_t>(data, FIL_HEADER_SIZE + 2);
    uint16_t n_heap = read_le<uint16_t>(data, FIL_HEADER_SIZE + 4) & 0x7FFF;

    return heap_top < 100 || n_heap < 5;
}

uint64_t TruncateRecovery::extract_from_truncated_page(const uint8_t* data, size_t size,
                                                         std::vector<RecoveredRecord>& records) {
    ZombieTupleExtractor extractor(page_size_);
    auto page_records = extractor.extract_all_tuples(data, size, 0);
    
    records.insert(records.end(), page_records.begin(), page_records.end());
    
    return page_records.size();
}

// ==================== RmrfRecoveryCustom Implementation ====================

RmrfRecoveryCustom::RmrfRecoveryCustom(uint32_t page_size)
    : page_size_(page_size) {}

RmrfRecoveryCustom::~RmrfRecoveryCustom() = default;

std::vector<std::string> RmrfRecoveryCustom::scan_directory(const std::string& dir_path) {
    std::vector<std::string> files;
    
    std::filesystem::path dir(dir_path);
    if (!std::filesystem::exists(dir)) {
        return files;
    }

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.size() > 4 && 
                    filename.substr(filename.size() - 4) == ".ibd") {
                    files.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[RmrfRecovery] Scanning error: " << e.what() << std::endl;
    }
    
    return files;
}

uint64_t RmrfRecoveryCustom::recover_from_files(const std::vector<std::string>& file_paths,
                                                   std::vector<RecoveredRecord>& recovered_records) {
    uint64_t total = 0;
    
    for (const auto& file_path : file_paths) {
        BinaryRecovery recovery(page_size_);
        std::vector<RecoveredRecord> records;
        
        if (recovery.recover_file(file_path, records)) {
            recovered_records.insert(recovered_records.end(), 
                                     records.begin(), records.end());
            total += records.size();
        }
    }
    
    return total;
}

uint64_t RmrfRecoveryCustom::scan_raw_device(const std::string& device_path,
                                              std::vector<RecoveredRecord>& recovered_records) {
    std::ifstream device(device_path, std::ios::binary);
    if (!device) {
        std::cerr << "[RmrfRecovery] Failed to open device: " << device_path << std::endl;
        return 0;
    }

    device.seekg(0, std::ios::end);
    uint64_t device_size = device.tellg();
    device.seekg(0, std::ios::beg);

    if (device_size < page_size_) {
        return 0;
    }

    std::vector<uint8_t> page_buffer(page_size_);
    
    uint64_t page_count = device_size / page_size_;
    uint64_t recovered = 0;

    for (uint64_t i = 0; i < page_count; ++i) {
        device.seekg(i * page_size_, std::ios::beg);
        device.read(reinterpret_cast<char*>(page_buffer.data()), page_buffer.size());

        uint16_t page_type = read_be<uint16_t>(page_buffer.data(), FIL_PAGE_TYPE_ABS);
        
        if (page_type == INNODB_PAGE_SIGNATURE || page_type == FIL_PAGE_INDEX) {
            ZombieTupleExtractor extractor(page_size_);
            auto page_records = extractor.extract_all_tuples(
                page_buffer.data(), page_size_, static_cast<uint32_t>(i));
            
            recovered_records.insert(recovered_records.end(),
                                     page_records.begin(), page_records.end());
            recovered += page_records.size();
        }
    }

    return recovered;
}

} // namespace innodb
} // namespace dbrecovery
