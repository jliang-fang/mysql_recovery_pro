#include "dbrecovery/innodb/binary_recovery_enhanced.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include <cmath>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <numeric>
#include <cstring>
#include <climits>

namespace dbrecovery {
namespace innodb {

// ==================== Constants for PhotoRec-style Recovery ====================
// Reference: TestDisk and ibd2sql implementations

/// @brief Signature values for PhotoRec-style scanning
/// When reading with big-endian (read_be), these are the correct page type values.
/// InnoDB stores all FIL header fields in big-endian format.
constexpr uint16_t SIG_INDEX         = 0x45BF;  // 17855 - FIL_PAGE_INDEX
constexpr uint16_t SIG_UNDO          = 0x45B2;  // 17842 - Undo log page
constexpr uint16_t SIG_ALLOCATED     = 0x0000;  // 0     - Freshly allocated
constexpr uint16_t SIG_SDI           = 0x45BE;  // 17854 - SDI page (MySQL 8.0+)
constexpr uint16_t SIG_BLOB          = 0x45BD;  // 17853 - BLOB page
constexpr uint16_t SIG_COMPRESSED    = 0x45BF;  // Same as INDEX; distinguished by context

/// @brief PhotoRec signature set for robust detection
static const std::unordered_set<uint16_t> VALID_PAGE_SIGNATURES = {
    SIG_INDEX, SIG_SDI, SIG_UNDO, SIG_ALLOCATED, SIG_BLOB, SIG_COMPRESSED
};

/// @brief Record types (from REC_INFO_STATUS_MASK)
constexpr uint8_t REC_TYPE_INFIMUM   = 0x02;
constexpr uint8_t REC_TYPE_SUPREMUM  = 0x03;

/// @brief PhotoRec-style scoring for ambiguous pages
constexpr int SCORE_VALID_INDEX     = 50;  // Valid index page signature
constexpr int SCORE_VALID_SDI       = 45;  // Valid SDI signature
constexpr int SCORE_VALID_UNDO      = 40;  // Valid undo log signature
constexpr int SCORE_ALLOCATED       = 25;  // Allocated page (candidate)
constexpr int SCORE_LSN_VALID       = 15;  // Valid LSN in range
constexpr int SCORE_PAGE_OFFSET_OK  = 10;  // Page offset looks reasonable
constexpr int SCORE_DIR_SLOTS_OK    = 10;  // Directory slots in valid range
constexpr int SCORE_HEAP_TOP_VALID  = 10;  // Heap top in valid range

/// @brief Heuristic thresholds for TRUNCATE detection
constexpr uint16_t TRUNCATE_HEAP_TOP_MAX      = 100;    // heap_top < 100 = likely truncated
constexpr uint16_t TRUNCATE_N_DIR_SLOTS_MIN   = 2;      // n_dir_slots == 2 = only system records
constexpr uint16_t TRUNCATE_N_HEAP_MAX        = 2;      // n_heap == 2 = only infimum/supremum
constexpr double TRUNCATE_FREE_SPACE_RATIO    = 0.95;   // >95% free space = truncated
constexpr uint16_t TRUNCATE_HEAP_TOP_STRICT   = 500;    // heap_top < 500 with few records = truncated

// ==================== BinaryScannerEnhanced Implementation ====================

BinaryScannerEnhanced::BinaryScannerEnhanced(uint32_t page_size)
    : page_size_(page_size), stats_{} {
    stats_.reset();
}

BinaryScannerEnhanced::~BinaryScannerEnhanced() = default;

uint64_t BinaryScannerEnhanced::scan_file(const std::string& file_path,
    std::function<bool(uint32_t page_no, const uint8_t* page_data, size_t size)> callback) {
    
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[BinaryScannerEnhanced] Failed to open file: " << file_path << std::endl;
        return 0;
    }
    
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    if (file_size < page_size_) {
        std::cerr << "[BinaryScannerEnhanced] File too small: " << file_size << " bytes" << std::endl;
        return 0;
    }
    
    int fd = open(file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "[BinaryScannerEnhanced] Failed to open fd: " << file_path << std::endl;
        return 0;
    }
    
    uint8_t* mapped = static_cast<uint8_t*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mapped == MAP_FAILED) {
        std::cerr << "[BinaryScannerEnhanced] mmap failed" << std::endl;
        close(fd);
        return 0;
    }
    
    madvise(mapped, file_size, MADV_SEQUENTIAL);
    
    uint64_t total_pages = file_size / page_size_;
    uint64_t pages_found = 0;
    
    for (uint64_t i = 0; i < total_pages; ++i) {
        uint8_t* page_data = mapped + (i * page_size_);
        uint32_t page_no = static_cast<uint32_t>(i);
        
        stats_.pages_scanned++;
        
        bool valid = false;
        if (scan_mode_ == MODE_CARVE) {
            if (validate_multipattern(page_data, page_size_)) {
                valid = true;
                stats_.multipattern_found++;
                stats_.heuristic_scored++;
            }
        } else if (scan_mode_ == MODE_RECOVERY) {
            uint16_t page_type = get_page_type(page_data);
            if (is_valid_page_type(page_type)) {
                valid = true;
                stats_.heuristic_scored++;
            }
        } else {
            valid = validate_page_header(page_data, page_size_);
            if (valid) {
                stats_.valid_pages++;
            } else {
                stats_.checksum_failed++;
            }
        }
        
        if (valid) {
            uint16_t page_type = get_page_type(page_data);
            if (page_type == FIL_PAGE_INDEX || page_type == SIG_BLOB) {
                stats_.index_pages++;
            }
            
            if (callback(page_no, page_data, page_size_)) {
                stats_.pages_with_records++;
                pages_found++;
            }
        } else {
            stats_.type_failed++;
        }
    }
    
    munmap(mapped, file_size);
    close(fd);
    return pages_found;
}

uint64_t BinaryScannerEnhanced::scan_buffer(const uint8_t* data, size_t size, uint64_t start_offset,
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
        
        if (scan_mode_ == MODE_CARVE) {
            if (validate_multipattern(page_data, page_size_)) {
                stats_.valid_pages++;
                stats_.multipattern_found++;
                stats_.heuristic_scored++;
            }
        } else {
            if (validate_page_header(page_data, page_size_)) {
                stats_.valid_pages++;
            } else {
                stats_.checksum_failed++;
            }
        }
        
        if (callback(page_no, page_data, page_size_)) {
            stats_.pages_with_records++;
            pages_found++;
        }
    }
    
    return pages_found;
}

uint64_t BinaryScannerEnhanced::full_carve_scan(const std::string& file_path,
    std::function<bool(uint32_t page_no, const uint8_t* page_data, size_t size)> callback) {

    enable_multipattern_scan();
    set_scan_mode(MODE_CARVE);
    return scan_file(file_path, callback);
}

int BinaryScannerEnhanced::calculate_heuristic_score(const uint8_t* data, size_t page_size) {
    if (!data || page_size < 38 + 16) {
        return -100;
    }

    int score = SCORE_ALLOCATED;  // Base score for tentative pages

    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    
    // PhotoRec-style signature detection TestDisk/ibd2sql approach
    if (page_type == SIG_INDEX) {
        score += SCORE_VALID_INDEX + 5;  // INDEX page (highest confidence)
    } else if (page_type == SIG_SDI) {
        score += SCORE_VALID_SDI;  // SDI page
    } else if (page_type == SIG_UNDO) {
        score += SCORE_VALID_UNDO;  // Undo log page
    } else if (page_type == SIG_ALLOCATED) {
        score += SCORE_ALLOCATED;  // Freshly allocated (still a candidate)
    } else {
        // Unknown type - penalize but don't discard
        return score - 10;
    }

    // Page offset validation (big-endian read)
    uint32_t page_offset = read_be<uint32_t>(data, FIL_PAGE_OFFSET);
    if (page_offset < 100000000) {
        score += SCORE_PAGE_OFFSET_OK;
    }

    // LSN validation (big-endian read)
    uint64_t lsn = read_be<uint64_t>(data, FIL_PAGE_LSN_ABS);
    if (lsn > 0 && lsn < 1000000000000ULL) {
        score += SCORE_LSN_VALID;
    }

    // Page header validation (PhotoRec-style)
    uint16_t n_dir_slots = read_le<uint16_t>(data, 38);
    uint16_t heap_top = read_le<uint16_t>(data, 40);
    uint16_t n_heap = read_le<uint16_t>(data, 42) & 0x7FFF;

    // PhotoRec validation heuristics
    if (n_dir_slots >= 2 && n_dir_slots <= 1000) {
        score += SCORE_DIR_SLOTS_OK;
    }
    
    if (heap_top > 50 && heap_top < page_size - 50) {
        score += SCORE_HEAP_TOP_VALID;
    }

    return score;
}

InnoDBSignature BinaryScannerEnhanced::extract_signature(const uint8_t* data) {
    InnoDBSignature sig{};
    sig.page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    sig.page_offset = read_be<uint32_t>(data, FIL_PAGE_OFFSET);
    sig.lsn = read_be<uint64_t>(data, FIL_PAGE_LSN_ABS);
    return sig;
}

bool BinaryScannerEnhanced::is_valid_page_type(uint16_t type) {
    // PhotoRec-style signature validation (TestDisk/ibd2sql approach)
    return VALID_PAGE_SIGNATURES.count(type) > 0;
}

bool BinaryScannerEnhanced::validate_multipattern(const uint8_t* data, size_t size) {
    if (!data || size < page_size_) {
        return false;
    }

    // PhotoRec-style multipattern scanning with heuristics
    uint16_t page_type = get_page_type(data);
    
    // Check if page type matches any known signature
    if (VALID_PAGE_SIGNATURES.count(page_type) > 0) {
        return true;
    }
    
    // Additional heuristic: check if it looks like data (not just zeros)
    // This helps detect pages with valid data but corrupted headers
    uint32_t page_offset = read_be<uint32_t>(data, FIL_PAGE_OFFSET);
    uint64_t lsn = read_be<uint64_t>(data, FIL_PAGE_LSN_ABS);
    
    // If page has reasonable LSN and offset, it might be valid despite unknown type
    if (lsn > 0 && lsn < 1000000000000ULL && page_offset < 100000000) {
        // Heuristic: check for non-zero data in header region
        uint32_t non_zero_count = 0;
        for (size_t i = 0; i < 38 && i < size; ++i) {
            if (data[i] != 0) {
                non_zero_count++;
            }
        }
        // At least 10 non-zero bytes in header = likely valid
        if (non_zero_count >= 10) {
            return true;
        }
    }
    
    return false;
}

bool BinaryScannerEnhanced::validate_page_header(const uint8_t* data, size_t size) {
    if (!data || size < 38 + 16) return false;
    
    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    if (!is_valid_page_type(page_type)) return false;
    
    uint32_t page_offset = read_be<uint32_t>(data, FIL_PAGE_OFFSET);
    if (page_offset > 100000000) return false;
    
    return true;
}

uint16_t BinaryScannerEnhanced::get_page_type(const uint8_t* data) {
    return read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
}

uint32_t BinaryScannerEnhanced::get_page_offset(const uint8_t* data) {
    return read_be<uint32_t>(data, FIL_PAGE_OFFSET);
}

uint64_t BinaryScannerEnhanced::get_page_lsn(const uint8_t* data) {
    return read_be<uint64_t>(data, FIL_PAGE_LSN_ABS);
}

uint32_t BinaryScannerEnhanced::compute_crc32(const uint8_t* data, size_t size) {
    if (size < page_size_) return 0;
    
    uint32_t crc = 0;
    for (size_t i = 4; i < page_size_; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    return crc;
}

bool BinaryScannerEnhanced::validate_checksum(const uint8_t* data, size_t size) {
    if (size < page_size_) return false;
    return innodb::is_page_checksum_valid(data, page_size_);
}

bool PageChainReconstructorEnhanced::validate_page_at(const uint8_t* data, size_t data_size, uint32_t page_num) {
    size_t pos = static_cast<size_t>(page_num) * 16384;
    if (pos + 38 > data_size) return false;

    // PhotoRec-style page validation with multiple signatures
    uint16_t page_type = read_be<uint16_t>(data, pos + FIL_PAGE_TYPE_ABS);
    return page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI ||
           page_type == FIL_PAGE_UNDO_LOG || page_type == FIL_PAGE_TYPE_BLOB;
}

// ==================== HeuristicSchemaInfererEnhanced Implementation ====================

HeuristicSchemaInfererEnhanced::HeuristicSchemaInfererEnhanced() = default;
HeuristicSchemaInfererEnhanced::~HeuristicSchemaInfererEnhanced() = default;

void HeuristicSchemaInfererEnhanced::analyze_record(const std::vector<std::vector<uint8_t>>& record) {
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
            case HeuristicFieldType::DATETIME:
                col_stats.datetime_patterns++;
                break;
            case HeuristicFieldType::TIMESTAMP:
                col_stats.timestamp_patterns++;
                break;
            case HeuristicFieldType::DATE:
                col_stats.date_patterns++;
                break;
            case HeuristicFieldType::TIME:
                col_stats.time_patterns++;
                break;
            default:
                if (field.size() == 1 && field[0] == 0x80) col_stats.null_count++;
                break;
        }
    }
    
    analyzed_records_.push_back(record);
}

HeuristicSchemaInfererEnhanced::SchemaInfo HeuristicSchemaInfererEnhanced::infer_schema() const {
    SchemaInfo schema;
    
    if (column_statistics_.empty()) {
        schema.columns.emplace_back("field0", HeuristicFieldType::VARCHAR_VARLEN);
        return schema;
    }
    
    schema.max_columns = static_cast<uint32_t>(column_statistics_.size());
    schema.min_columns = schema.max_columns;
    
    for (const auto& [col_idx, col_stats] : column_statistics_) {
        std::string column_name = "field" + std::to_string(col_idx);
        HeuristicFieldType type = HeuristicFieldType::VARCHAR_VARLEN;
        
        uint64_t total = col_stats.int_patterns + col_stats.varchar_patterns + 
                        col_stats.blob_patterns + col_stats.datetime_patterns +
                        col_stats.timestamp_patterns + col_stats.date_patterns +
                        col_stats.time_patterns;
        
        if (total > 0) {
            double int_ratio = static_cast<double>(col_stats.int_patterns) / total;
            double varchar_ratio = static_cast<double>(col_stats.varchar_patterns) / total;
            double blob_ratio = static_cast<double>(col_stats.blob_patterns) / total;
            
            if (int_ratio > 0.5) type = HeuristicFieldType::INT;
            else if (varchar_ratio > 0.5) type = HeuristicFieldType::VARCHAR_VARLEN;
            else if (blob_ratio > 0.5) type = HeuristicFieldType::BLOB_PTR;
            else if (col_stats.datetime_patterns > col_stats.varchar_patterns) type = HeuristicFieldType::DATETIME;
        }
        
        schema.columns.emplace_back(column_name, type);
    }
    
    schema.columns_detected = static_cast<uint32_t>(schema.columns.size());
    return schema;
}

HeuristicSchemaInfererEnhanced::HeuristicFieldType HeuristicSchemaInfererEnhanced::detect_field_type(const std::vector<uint8_t>& data) {
    if (data.empty()) return HeuristicFieldType::UNKNOWN;
    if (data.size() == 1 && data[0] == 0x80) return HeuristicFieldType::NULL_VALUE;
    
    if (data.size() == 1) return HeuristicFieldType::TINY_INT;
    if (data.size() == 2) return HeuristicFieldType::SMALL_INT;
    if (data.size() == 3) return HeuristicFieldType::MEDIUM_INT;
    if (data.size() == 4) return HeuristicFieldType::INT;
    if (data.size() == 8) return HeuristicFieldType::BIGINT;
    if (data.size() == 20) return HeuristicFieldType::BLOB_PTR;
    if (data.size() == 4) return HeuristicFieldType::TIMESTAMP;
    if (data.size() == 3) return HeuristicFieldType::DATE;
    if (data.size() == 8) return HeuristicFieldType::DATETIME;
    
    // Variable-length: 1-255 bytes with length prefix
    if (data.size() > 1 && data.size() <= 256) {
        uint8_t first = data[0];
        if (first < 255) {
            bool is_ascii = true;
            for (size_t i = 1; i < data.size() && i < 33; ++i) {
                if (data[i] != 0 && (data[i] < 32 || data[i] >= 127)) {
                    is_ascii = false;
                    break;
                }
            }
            if (is_ascii) return HeuristicFieldType::VARCHAR_VARLEN;
        }
    }
    
    return HeuristicFieldType::VARCHAR_VARLEN;
}

int64_t HeuristicSchemaInfererEnhanced::parse_numeric_value(const std::vector<uint8_t>& data, HeuristicFieldType type) {
    if (data.empty()) return 0;
    
    switch (type) {
        case HeuristicFieldType::TINY_INT: return static_cast<int8_t>(data[0]);
        case HeuristicFieldType::SMALL_INT:
            if (data.size() >= 2) return static_cast<int16_t>(read_le<int16_t>(data.data()));
            break;
        case HeuristicFieldType::MEDIUM_INT:
            if (data.size() >= 3) {
                int32_t val = static_cast<int32_t>(data[0] | (data[1] << 8) | (data[2] << 16));
                if (val & 0x800000) val |= 0xFF000000;
                return val;
            }
            break;
        case HeuristicFieldType::INT:
            if (data.size() >= 4) return static_cast<int32_t>(read_le<int32_t>(data.data()));
            break;
        case HeuristicFieldType::BIGINT:
            if (data.size() >= 8) return static_cast<int64_t>(read_le<int64_t>(data.data()));
            break;
        default: break;
    }
    return 0;
}

// ==================== EnhancedZombieTupleExtractor Implementation ====================

EnhancedZombieTupleExtractor::EnhancedZombieTupleExtractor(uint32_t page_size)
    : page_size_(page_size), stats_{} {
    stats_.reset();
}

EnhancedZombieTupleExtractor::~EnhancedZombieTupleExtractor() = default;

bool EnhancedZombieTupleExtractor::validate_next_offset(uint16_t next_off, uint16_t current_off, 
                                                        uint16_t heap_top, size_t page_size) {
    if (next_off == 0 || next_off == 0xFFFF) return false;
    if (next_off <= current_off) return false;
    if (next_off > heap_top) return false;
    if (next_off > page_size) return false;
    uint16_t record_len = next_off - current_off;
    if (record_len < 5 || record_len > page_size - 100) return false;
    return true;
}

std::vector<StructuredRecord> EnhancedZombieTupleExtractor::extract_all_tuples(const uint8_t* data, size_t size, uint32_t page_no) {
    std::vector<StructuredRecord> records;
    std::unordered_set<uint16_t> visited_offsets;
    
    if (!data || size < page_size_) return records;
    if (size < 38 + 16) return records;
    
    uint16_t n_dir_slots = read_le<uint16_t>(data, 38);
    uint16_t heap_top = read_le<uint16_t>(data, 40);
    if (n_dir_slots < 2) return records;
    
    uint16_t current_offset = 54;
    
    while (current_offset < heap_top - 5) {
        if (visited_offsets.count(current_offset) > 0) {
            stats_.circular_refs++;
            break;
        }
        visited_offsets.insert(current_offset);
        
        auto header_opt = EnhancedRecordHeader::parse(data, current_offset, size);
        if (!header_opt) break;
        
        const auto& header = *header_opt;
        
        if (header.is_min_rec) {
            if (!validate_next_offset(header.next_record_offset, current_offset, heap_top, page_size_)) break;
            current_offset = header.next_record_offset;
            continue;
        }
        
        if (!validate_next_offset(header.next_record_offset, current_offset, heap_top, page_size_)) break;
        
        auto field_data = extract_record_data(data, size, current_offset, header);
        uint16_t record_length = header.next_record_offset - current_offset;
        
        StructuredRecord record{};
        record.page_no = page_no;
        record.record_offset = current_offset;
        record.heap_no = header.heap_no;
        record.fields = std::move(field_data);
        record.is_deleted = header.is_deleted;
        record.lsn = 0;
        record.record_length = record_length;
        record.status = (header.info_bits & 0x07);
        
        if (header.is_deleted) stats_.zombie_records++;
        else stats_.valid_records++;
        
        stats_.total_records++;
        records.push_back(std::move(record));
        
        if (header.next_record_offset >= heap_top - 5) break;
        current_offset = header.next_record_offset;
    }
    
    return records;
}

std::optional<StructuredRecord> EnhancedZombieTupleExtractor::extract_record(const uint8_t* data, size_t size,
                                                                             size_t offset, uint32_t page_no) {
    if (offset + 6 > size) return std::nullopt;
    
    auto header_opt = EnhancedRecordHeader::parse(data, offset, size);
    if (!header_opt) return std::nullopt;
    
    auto fields = extract_record_data(data, size, offset, *header_opt);
    
    StructuredRecord record{};
    record.page_no = page_no;
    record.record_offset = static_cast<uint16_t>(offset);
    record.heap_no = header_opt->heap_no;
    record.fields = std::move(fields);
    record.is_deleted = header_opt->is_deleted;
    record.lsn = 0;
    record.status = (header_opt->info_bits & 0x07);
    
    stats_.total_records++;
    if (header_opt->is_deleted) stats_.zombie_records++;
    else stats_.valid_records++;
    
    return record;
}

std::vector<std::vector<uint8_t>> EnhancedZombieTupleExtractor::extract_record_data(const uint8_t* data, size_t size,
                                                                                   size_t offset, const EnhancedRecordHeader& header,
                                                                                   uint16_t n_columns) {
    std::vector<std::vector<uint8_t>> fields;
    
    if (!data || size <= offset) return fields;
    
    size_t data_offset = offset + 5;
    uint16_t field_count = n_columns > 0 ? n_columns : 3;
    
    for (uint16_t i = 0; i < field_count && data_offset < size; ++i) {
        std::vector<uint8_t> field_data;
        
        if (data_offset < size && data[data_offset] == 0x80) {
            field_data.push_back(data[data_offset++]);
            fields.push_back(field_data);
            continue;
        }
        
        if (data_offset < size) {
            uint8_t len_byte = data[data_offset];
            data_offset++;
            
            if (len_byte < 255) {
                uint8_t field_len = len_byte;
                if (data_offset + field_len <= size) {
                    field_data.assign(data + data_offset, data + data_offset + field_len);
                    data_offset += field_len;
                }
            } else if (len_byte == 255 && data_offset + 2 <= size) {
                uint16_t field_len = read_le<uint16_t>(data, data_offset);
                data_offset += 2;
                if (data_offset + field_len <= size) {
                    field_data.assign(data + data_offset, data + data_offset + field_len);
                    data_offset += field_len;
                }
            } else {
                if (data_offset < size) field_data.push_back(data[data_offset++]);
            }
        }
        
        if (!field_data.empty() || i == 0) fields.push_back(field_data);
    }
    
    return fields;
}

// ==================== TruncateRecoveryDetector Implementation ====================

TruncateRecoveryDetector::TruncateRecoveryDetector() : page_size_(16384) {}
TruncateRecoveryDetector::~TruncateRecoveryDetector() = default;

TruncateRecoveryDetector::TruncateIndicators TruncateRecoveryDetector::analyze_page(const uint8_t* data, size_t size) {
    TruncateIndicators indicators{};
    
    if (!data || size < 38 + 16) {
        indicators.likely_truncated = true;
        indicators.reason = "Invalid page size";
        return indicators;
    }
    
    indicators.heap_top = read_le<uint16_t>(data, 40);
    indicators.n_dir_slots = read_le<uint16_t>(data, 38);
    indicators.n_heap = read_le<uint16_t>(data, 42) & 0x7FFF;
    indicators.free_space_ratio = static_cast<double>(page_size_ - indicators.heap_top) / page_size_;
    
    if (indicators.heap_top < 100) {
        indicators.likely_truncated = true;
        indicators.reason = "heap_top too small (< 100 bytes)";
    } else if (indicators.n_dir_slots == 2 && indicators.n_heap == 2) {
        indicators.likely_truncated = true;
        indicators.reason = "Only system records present";
    } else if (indicators.free_space_ratio > 0.95 && indicators.n_heap <= 2) {
        indicators.likely_truncated = true;
        indicators.reason = "Very high free space ratio";
    } else if (indicators.heap_top < 500 && indicators.n_heap <= 5) {
        indicators.likely_truncated = true;
        indicators.reason = "Small heap_top with few records";
    }
    
    return indicators;
}

bool TruncateRecoveryDetector::is_likely_truncated(const uint8_t* data, size_t size) {
    auto indicators = analyze_page(data, size);
    return indicators.likely_truncated;
}

TruncateRecoveryDetector::FileAnalysis TruncateRecoveryDetector::analyze_file(const uint8_t* data, size_t size, uint32_t page_size) {
    FileAnalysis analysis{};
    uint32_t num_pages = static_cast<uint32_t>(size / page_size);
    page_size_ = page_size;
    
    for (uint32_t page_no = 0; page_no < num_pages; ++page_no) {
        size_t page_offset = static_cast<size_t>(page_no) * page_size;
        const uint8_t* page_data = data + page_offset;
        auto indicators = analyze_page(page_data, page_size);
        
        if (indicators.likely_truncated) analysis.empty_pages.push_back(page_no);
        else if (indicators.free_space_ratio > 0.8) analysis.partial_pages.push_back(page_no);
        else analysis.full_pages.push_back(page_no);
    }
    
    if (!analysis.empty_pages.empty()) {
        double empty_ratio = static_cast<double>(analysis.empty_pages.size()) / num_pages;
        if (empty_ratio > 0.7) analysis.likely_truncated = true;
    }
    
    return analysis;
}

// ==================== PageChainReconstructorEnhanced Implementation ====================

PageChainReconstructorEnhanced::PageChainReconstructorEnhanced(uint32_t page_size)
    : page_size_(page_size), stats_{} {
    stats_.reset();
}

PageChainReconstructorEnhanced::~PageChainReconstructorEnhanced() = default;

bool PageChainReconstructorEnhanced::add_page(uint32_t page_no, const uint8_t* data, size_t size) {
    if (size < 38 + 44) return false;
    page_cache_[page_no] = 1;
    // Store page data for later reconstruction without raw pointer access
    page_data_cache_[page_no] = std::vector<uint8_t>(data, data + std::min(size, static_cast<size_t>(page_size_)));
    return true;
}

std::vector<uint32_t> PageChainReconstructorEnhanced::reconstruct_chain(uint32_t seed_page) {
    std::vector<uint32_t> chain;

    // If we have pages in the cache, return them as the chain
    if (!page_cache_.empty()) {
        for (const auto& [page_no, flag] : page_cache_) {
            chain.push_back(page_no);
        }
        return chain;
    }

    // No data available — return empty chain
    return chain;
}

PageChain PageChainReconstructorEnhanced::reconstruct_chain_with_validation(uint32_t seed_page, const uint8_t* data, size_t data_size) {
    PageChain chain;
    chain.first_page = find_leftmost(seed_page, data, data_size);
    
    // Walk forward from leftmost
    std::unordered_set<uint32_t> visited;
    uint32_t current = chain.first_page;
    uint32_t traversed = 0;
    
    while (current != 0xFFFFFFFF && current != 0 && traversed < 1000000) {
        if (visited.count(current) > 0) {
            stats_.cycles_detected++;
            break;
        }
        
        visited.insert(current);
        stats_.pages_found++;
        
        if (!validate_page_at(data, data_size, current)) {
            stats_.pages_skipped++;
            break;
        }
        
        stats_.pages_validated++;
        chain.pages.push_back(current);
        
        size_t pos = static_cast<size_t>(current) * page_size_;
        if (pos + 44 > data_size) break;
        
        uint32_t next = read_be<uint32_t>(data, pos + FIL_PAGE_NEXT_ABS);
        if (next == 0xFFFFFFFF || next == current || next >= data_size / page_size_) break;
        
        current = next;
        traversed++;
    }
    
    chain.last_page = current;
    chain.is_valid = !chain.pages.empty();
    return chain;
}

uint32_t PageChainReconstructorEnhanced::find_leftmost(uint32_t page_num, const uint8_t* data, size_t data_size) {
    std::unordered_set<uint32_t> visited;
    uint32_t current = page_num;
    uint32_t leftmost = page_num;
    uint32_t traversed = 0;
    
    while (traversed < 1000000) {
        if (visited.count(current) > 0) {
            stats_.cycles_detected++;
            break;
        }
        
        size_t pos = static_cast<size_t>(current) * page_size_;
        if (pos + 8 > data_size) break;
        
        uint32_t prev = read_be<uint32_t>(data, pos + FIL_PAGE_PREV_ABS);  // FIL_PAGE_PREV at offset 8
        
        if (prev == 0xFFFFFFFF || prev == 0 || prev == current) break;
        if (!validate_page_at(data, data_size, prev)) break;
        
        visited.insert(current);
        if (prev < leftmost) leftmost = prev;
        
        current = prev;
        traversed++;
    }
    
    stats_.first_page = leftmost;
    return leftmost;
}

uint32_t PageChainReconstructorEnhanced::find_rightmost(uint32_t page_num, const uint8_t* data, size_t data_size) {
    std::unordered_set<uint32_t> visited;
    uint32_t current = page_num;
    uint32_t rightmost = page_num;
    uint32_t traversed = 0;
    
    while (traversed < 1000000) {
        if (visited.count(current) > 0) {
            stats_.cycles_detected++;
            break;
        }
        
        size_t pos = static_cast<size_t>(current) * page_size_;
        if (pos + 44 > data_size) break;
        
        uint32_t next = read_be<uint32_t>(data, pos + FIL_PAGE_NEXT_ABS);  // FIL_PAGE_NEXT at offset 12
        
        if (next == 0xFFFFFFFF || next == 0 || next == current) break;
        if (!validate_page_at(data, data_size, next)) break;
        
        visited.insert(current);
        rightmost = next;
        current = next;
        traversed++;
    }
    
    return rightmost;
}

uint16_t PageChainReconstructorEnhanced::get_page_type(const uint8_t* data) {
    return read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
}

// ==================== EnhancedBinaryRecovery Implementation ====================

EnhancedBinaryRecovery::EnhancedBinaryRecovery(uint32_t page_size)
    : page_size_(page_size), stats_{}, scanner_(page_size), extractor_(page_size),
      chain_reconstructor_(page_size) {
    stats_.reset();
}

EnhancedBinaryRecovery::~EnhancedBinaryRecovery() = default;

std::vector<StructuredRecord> EnhancedBinaryRecovery::recover_file(
    const std::string& file_path,
    std::function<HeuristicSchemaInfererEnhanced::SchemaInfo(const std::string&)> schema_callback) {
    
    std::vector<StructuredRecord> all_records;
    seen_records_.clear();
    stats_.reset();
    
    scanner_.enable_multipattern_scan();
    scanner_.set_scan_mode(BinaryScannerEnhanced::MODE_CARVE);
    
    auto callback = [this, &all_records](uint32_t page_no, const uint8_t* page_data, size_t size) -> bool {
        auto records = recover_page(page_data, size, page_no);
        all_records.insert(all_records.end(), records.begin(), records.end());
        return !records.empty();
    };
    
    stats_.pages_scanned = scanner_.scan_file(file_path, callback);
    stats_.pages_with_records = scanner_.get_stats().pages_with_records;
    stats_.total_records = extractor_.get_stats().total_records;
    stats_.zombie_records = extractor_.get_stats().zombie_records;
    
    return all_records;
}

std::vector<StructuredRecord> EnhancedBinaryRecovery::recover_with_heuristic_schema(const std::string& file_path) {
    return recover_file(file_path, nullptr);
}

uint64_t EnhancedBinaryRecovery::scan_and_extract(const std::string& file_path,
                                                   std::function<bool(const StructuredRecord&)> record_callback) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) return 0;
    
    file.seekg(0, std::ios::end);
    uint64_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    if (file_size < page_size_) return 0;
    
    uint64_t total_pages = file_size / page_size_;
    uint64_t pages_processed = 0;
    
    std::vector<uint8_t> page_data(page_size_);
    
    for (uint64_t i = 0; i < total_pages; ++i) {
        file.seekg(i * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);
        
        auto records = recover_page(page_data.data(), page_size_, static_cast<uint32_t>(i));
        
        for (const auto& record : records) {
            if (record_callback(record)) {
                stats_.total_records++;
            } else {
                stats_.duplicate_records++;
            }
        }
        
        pages_processed++;
        if (i % 10000 == 0) {
            std::cout << "[EnhancedBinaryRecovery] Scanned " << i << "/" << total_pages << " pages" << std::endl;
        }
    }
    
    stats_.pages_scanned = pages_processed;
    return pages_processed;
}

std::vector<StructuredRecord> EnhancedBinaryRecovery::recover_page(const uint8_t* data, size_t size, uint32_t page_no) {
    std::vector<StructuredRecord> records;

    uint16_t page_type = read_be<uint16_t>(data, FIL_PAGE_TYPE_ABS);
    if (page_type != FIL_PAGE_INDEX && page_type != FIL_PAGE_TYPE_BLOB) {
        return records;
    }

    auto structured = extractor_.extract_all_tuples(data, size, page_no);

    if (structured.empty()) {
        auto heuristic = extract_records_heuristic(data, size, page_no);
        records = std::move(heuristic);
    } else {
        records = std::move(structured);
    }

    // PhotoRec-style deduplication using offset + field data hash
    // This prevents false duplicates from same record appearing in multiple pages
    std::unordered_set<std::string> page_seen_records;
    std::vector<StructuredRecord> unique_records;
    for (const auto& record : records) {
        // Create unique key: page_no + record_offset + first few bytes of first field
        // This is more robust than just to_hex() which might change format
        std::string key = std::to_string(record.page_no) + "_" + 
                         std::to_string(record.record_offset) + "_";
        
        // Add hash of first 8 bytes of first field (or all if smaller)
        uint64_t field_hash = 0;
        if (!record.fields.empty() && !record.fields[0].empty()) {
            size_t hash_len = std::min(size_t(8), record.fields[0].size());
            for (size_t i = 0; i < hash_len; ++i) {
                field_hash = (field_hash << 8) ^ record.fields[0][i];
            }
        }
        key += std::to_string(field_hash);
        
        if (seen_records_.find(key) == seen_records_.end() && 
            page_seen_records.find(key) == page_seen_records.end()) {
            seen_records_.insert(key);
            page_seen_records.insert(key);
            unique_records.push_back(record);
        } else {
            stats_.duplicate_records++;
        }
    }

    return unique_records;
}

std::vector<StructuredRecord> EnhancedBinaryRecovery::extract_records_heuristic(const uint8_t* data, size_t size, uint32_t page_no) {
    std::vector<StructuredRecord> records;
    std::unordered_set<uint16_t> visited;
    uint16_t heap_top = read_le<uint16_t>(data, 40);
    uint16_t current_offset = 54;
    
    while (current_offset < heap_top - 5) {
        if (visited.count(current_offset) > 0) break;
        visited.insert(current_offset);
        
        if (current_offset < size && (data[current_offset] & 0x07) == 0x02) {
            // INFIMUM - skip
            if (current_offset + 2 > size) break;
            uint16_t next = read_le<uint16_t>(data, current_offset + 1) & 0x7FFF;
            if (next <= current_offset || next >= heap_top) break;
            current_offset = next;
            continue;
        }
        
        if (current_offset + 5 > size) break;
        
        uint8_t info_bits = data[current_offset];
        uint16_t next_off = read_le<uint16_t>(data, current_offset + 1) & 0x7FFF;
        uint16_t heap_no = ((data[current_offset + 4] & 0x7F) << 8) | data[current_offset + 3];
        
        if ((info_bits & 0x07) == 0x03) break;  // SUPREMUM
        
        // Extract fields
        std::vector<std::vector<uint8_t>> fields;
        size_t field_offset = current_offset + 5;
        for (int f = 0; f < 3 && field_offset < size; ++f) {
            if (field_offset >= heap_top - 10) break;
            
            if (data[field_offset] == 0x80) {
                fields.push_back({0x80});
                field_offset++;
            } else if (field_offset + 1 < size) {
                uint8_t len = data[field_offset];
                field_offset++;
                
                if (len < 255 && field_offset + len <= size) {
                    fields.push_back(std::vector<uint8_t>(data + field_offset, data + field_offset + len));
                    field_offset += len;
                } else if (len == 255 && field_offset + 2 <= size) {
                    uint16_t len2 = read_le<uint16_t>(data, field_offset);
                    field_offset += 2;
                    if (field_offset + len2 <= size) {
                        fields.push_back(std::vector<uint8_t>(data + field_offset, data + field_offset + len2));
                        field_offset += len2;
                    }
                }
            }
        }
        
        if (!fields.empty()) {
            StructuredRecord record{};
            record.page_no = page_no;
            record.record_offset = current_offset;
            record.heap_no = heap_no;
            record.fields = std::move(fields);
            record.is_deleted = (info_bits & 0x08) != 0;
            record.status = (info_bits & 0x07);
            record.record_length = next_off - current_offset;
            records.push_back(record);
        }
        
        if (next_off >= heap_top - 5) break;
        current_offset = next_off;
    }
    
    return records;
}

} // namespace innodb
} // namespace dbrecovery
