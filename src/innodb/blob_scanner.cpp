/**
 * @file blob_scanner.cpp
 * @brief BLOB/TEXT page scanner for recovering large object data
 * 
 * MySQL BLOB/TEXT 数据存储在单独的 BLOB pages 中
 * 本扫描器从 BLOB pages 中提取完整的大字段数据
 */

#include "dbrecovery/innodb/blob_scanner.hpp"
#include "dbrecovery/innodb/mach_read.hpp"
#include "dbrecovery/innodb/page_defs.hpp"
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <set>
#include <limits>
#include <zlib.h>

namespace dbrecovery {
namespace innodb {

// ==================== BLOB Page Constants ====================
// Constants moved to page_defs.hpp (BTR_BLOB_HDR_SIZE, BTR_EXTERN_*, etc.)

// Local constants not in page_defs.hpp
constexpr uint16_t BTR_BLOB_NEXT = 0;       // Next page offset
constexpr uint16_t BTR_BLOB_PREV = 4;       // Previous page offset
constexpr uint16_t BTR_BLOB_LEN  = 8;       // Total length (4 bytes)
constexpr uint16_t BTR_BLOB_FLAG   = 12;    // Flag byte

// BLOB flag bits
constexpr uint8_t BTR_BLOB_PARTIAL = 0x80;  // Partially external storage
constexpr uint8_t BTR_BLOB_FREE    = 0x40;  // Free page

// First BLOB page flags
constexpr uint8_t BTR_BLOB_FIRST = 0x01;    // First page of BLOB

// ==================== BLOB Scanner Implementation ====================

std::vector<BLOBPage> BlobScanner::scan_ibd_file(const std::string& file_path) {
    std::vector<BLOBPage> blob_pages;
    
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        std::cerr << "[ERROR] Cannot open file: " << file_path << "\n";
        return blob_pages;
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    uint32_t total_pages = static_cast<uint32_t>((file_size + page_size_ - 1) / page_size_);
    
    std::cout << "[INFO] Scanning " << total_pages << " pages for BLOB data...\n";
    
    // Read and analyze each page
    std::vector<uint8_t> page_buffer(page_size_);
    
    for (uint32_t page_no = 0; page_no < total_pages; page_no++) {
        file.seekg(static_cast<std::streamoff>(page_no) * page_size_, std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);
        
        uint16_t page_type = get_page_type(page_buffer.data());
        
        // Check for BLOB page type
        if (page_type == FIL_PAGE_TYPE_BLOB || page_type == 0) {
            BLOBPage blob_page;
            blob_page.page_no = page_no;
            blob_page.page_type = page_type;
            
            // Parse BLOB page header
            if (page_size_ >= BTR_BLOB_HDR_SIZE) {
                blob_page.next_page = read_le<uint32_t>(page_buffer.data(), BTR_BLOB_NEXT);
                blob_page.prev_page = read_le<uint32_t>(page_buffer.data(), BTR_BLOB_PREV);
                blob_page.total_len = read_le<uint32_t>(page_buffer.data(), BTR_BLOB_LEN);
                blob_page.flag = page_buffer[BTR_BLOB_FLAG];
            }
            
            // Extract data payload (after BLOB header)
            if (page_size_ > BTR_BLOB_HDR_SIZE) {
                blob_page.data.assign(
                    page_buffer.data() + BTR_BLOB_HDR_SIZE,
                    page_buffer.data() + page_size_
                );
            }
            
            blob_pages.push_back(blob_page);
            stats_.blob_pages_found++;
        }
    }
    
    stats_.pages_scanned = total_pages;
    return blob_pages;
}

std::vector<BLOBField> BlobScanner::extract_blob_fields(
    const std::vector<uint8_t>& row_data,
    const std::vector<FieldType>& field_definitions) {
    
    std::vector<BLOBField> blob_fields;
    
    size_t offset = 0;
    
    for (size_t i = 0; i < field_definitions.size() && offset < row_data.size(); i++) {
        const auto& field_def = field_definitions[i];
        
        if (field_def.is_extern) {
            // This field is stored externally (BLOB)
            if (offset + BTR_EXTERN_SIZE > row_data.size()) {
                break;
            }
            
            BLOBField field;
            field.field_index = static_cast<uint16_t>(i);
            field.space_id = read_le<uint32_t>(row_data.data(), offset + BTR_EXTERN_SPACE_ID);
            field.page_no = read_le<uint32_t>(row_data.data(), offset + BTR_EXTERN_PAGE);
            field.offset = read_le<uint32_t>(row_data.data(), offset + BTR_EXTERN_OFFSET);
            field.total_len = read_le<uint32_t>(row_data.data(), offset + BTR_EXTERN_LEN);
            
            // Check if partial
            field.is_partial = (row_data[offset + BTR_EXTERN_VERSION] & BTR_BLOB_PARTIAL) != 0;
            
            blob_fields.push_back(field);
            offset += BTR_EXTERN_SIZE;
        } else {
            // Inline field - skip based on type
            size_t field_size = get_field_size(field_definitions[i], row_data, offset);
            offset += field_size;
        }
    }
    
    return blob_fields;
}

std::vector<uint8_t> BlobScanner::read_blob_from_pages(
    const std::string& file_path,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t offset,
    uint32_t length) {
    
    std::vector<uint8_t> result;
    
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        std::cerr << "[ERROR] Cannot open file: " << file_path << "\n";
        return result;
    }
    
    // For now, assume same file (space_id 0)
    // In production, would need to lookup space_id -> file path mapping
    
    size_t start_offset = static_cast<size_t>(page_no) * page_size_ + offset;
    
    file.seekg(static_cast<std::streamoff>(start_offset), std::ios::beg);
    
    // Read in chunks
    result.resize(length);
    file.read(reinterpret_cast<char*>(result.data()), length);
    
    return result;
}

std::vector<uint8_t> BlobScanner::read_blob_chain(
    const std::string& file_path,
    uint32_t first_page,
    uint32_t total_length) {

    std::vector<uint8_t> result;
    std::set<uint32_t> visited_pages;
    constexpr size_t MAX_BLOB_SIZE = 48 * 1024 * 1024;

    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file || page_size_ == 0) return result;
    const std::streamoff file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    uint32_t current_page = first_page;
    size_t remaining = total_length;
    size_t offset_in_page = BTR_BLOB_HDR_SIZE;
    while (current_page != 0 && remaining > 0 && visited_pages.size() < 10000) {
        if (visited_pages.count(current_page)) {
            std::cerr << "[ERROR] BLOB page loop detected at page " << current_page << "\n";
            return {};
        }
        visited_pages.insert(current_page);

        const uint64_t page_offset = static_cast<uint64_t>(current_page) * page_size_;
        if (page_offset > static_cast<uint64_t>(file_size) ||
            page_size_ > static_cast<size_t>(file_size - static_cast<std::streamoff>(page_offset))) {
            return {};
        }
        file.clear();
        file.seekg(static_cast<std::streamoff>(page_offset), std::ios::beg);
        std::vector<uint8_t> page_buffer(page_size_);
        file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);
        if (file.gcount() != static_cast<std::streamsize>(page_size_)) return {};

        const size_t available = page_size_ > offset_in_page ? page_size_ - offset_in_page : 0;
        const size_t to_read = std::min(available, remaining);
        if (result.size() > MAX_BLOB_SIZE - to_read) return {};
        result.insert(result.end(), page_buffer.begin() + offset_in_page,
                      page_buffer.begin() + offset_in_page + to_read);
        remaining -= to_read;

        if (page_buffer.size() < BTR_BLOB_NEXT + 4) return {};
        current_page = read_le<uint32_t>(page_buffer.data(), BTR_BLOB_NEXT);
        offset_in_page = BTR_BLOB_HDR_SIZE;
    }

    if (remaining != 0 || result.size() != total_length) return {};
    stats_.total_blob_bytes += static_cast<uint32_t>(result.size());
    return result;
}

// ==================== BLOB Field Size Calculation ====================

size_t BlobScanner::get_field_size(FieldType type, 
                                   const std::vector<uint8_t>& data, 
                                   size_t offset) {
    if (offset >= data.size()) {
        return 0;
    }
    
    switch (type.type_code) {
        case mysql_types::TINY_BLOB:
        case mysql_types::BLOB:
        case mysql_types::MEDIUM_BLOB:
        case mysql_types::LONG_BLOB:
            // BLOB fields store 20-byte pointer in row
            return BTR_EXTERN_SIZE;
            
        case mysql_types::VARCHAR:
        case mysql_types::VAR_STRING: {
            uint8_t first_byte = data[offset];
            if (first_byte < 0x80) {
                return 1 + first_byte;
            } else {
                if (offset + 2 > data.size()) return 1;
                uint16_t len = read_le<uint16_t>(data.data(), offset) & 0x3FFF;
                return 2 + len;
            }
        }
            
        case mysql_types::TINY: return 1;
        case mysql_types::SHORT: return 2;
        case mysql_types::LONG: return 4;
        case mysql_types::LONGLONG: return 8;
        case mysql_types::FLOAT: return 4;
        case mysql_types::DOUBLE: return 8;
            
        default:
            // Variable length, try to parse
            if (offset < data.size()) {
                return std::min(data.size() - offset, static_cast<size_t>(255));
            }
            return 0;
    }
}

// ==================== Full BLOB Recovery ====================

std::vector<RecoveredBLOB> BlobScanner::recover_all_blobs(
    const std::string& file_path) {
    
    std::vector<RecoveredBLOB> recovered;
    
    // Step 1: Find all BLOB pages
    auto blob_pages = scan_ibd_file(file_path);
    std::cout << "[INFO] Found " << blob_pages.size() << " BLOB pages\n";
    
    // Step 2: Link BLOB pages using next/prev pointers
    std::map<uint32_t, BLOBPage*> page_map;
    for (auto& bp : blob_pages) {
        page_map[bp.page_no] = &bp;
    }
    
    // Step 3: Process each BLOB chain
    for (auto& bp : blob_pages) {
        // Only process first pages (prev_page == 0)
        if (bp.prev_page != 0) {
            continue;
        }
        
        if (bp.total_len == 0 || bp.total_len > 48 * 1024 * 1024) {
            continue;  // Skip invalid lengths
        }
        
        // Read complete BLOB
        auto blob_data = read_blob_chain(file_path, bp.page_no, bp.total_len);
        
        if (!blob_data.empty()) {
            RecoveredBLOB rb;
            rb.page_no = bp.page_no;
            rb.total_length = bp.total_len;
            rb.data = blob_data;
            rb.is_text = is_ascii_or_utf8(blob_data);
            recovered.push_back(rb);
        }
    }
    
    stats_.blobs_recovered = recovered.size();
    return recovered;
}

bool BlobScanner::is_ascii_or_utf8(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return false;
    }
    
    // Simple heuristic: check for high bytes
    size_t high_bytes = 0;
    for (uint8_t b : data) {
        if (b >= 0x80) {
            high_bytes++;
        }
    }
    
    // If less than 10% high bytes, likely ASCII
    return (high_bytes * 100 / data.size()) < 10;
}

// ==================== Secondary Index BLOB Recovery ====================

std::vector<HeuristicRecord> BlobScanner::recover_from_secondary_index(
    const std::string& file_path,
    uint32_t index_page_no) {
    
    std::vector<HeuristicRecord> records;
    
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return records;
    }
    
    file.seekg(static_cast<std::streamoff>(index_page_no) * page_size_, std::ios::beg);
    std::vector<uint8_t> page_buffer(page_size_);
    file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);
    
    // Parse index page for records with external pointers
    auto parser = HeuristicParser();
    auto index_records = parser.parse_all_records(
        page_buffer.data(), page_size_, 
        read_le<uint16_t>(page_buffer.data(), 40));
    
    for (const auto& rec : index_records) {
        if (rec.is_infimum || rec.is_supremum || rec.is_deleted) {
            continue;
        }
        
        // Check each field for BLOB pointers
        for (size_t i = 0; i < rec.fields.size(); i++) {
            const auto& field = rec.fields[i];
            
            if (field.data.size() == BTR_EXTERN_SIZE) {
                // This might be a BLOB pointer - try to extract
                uint32_t page_no = read_le<uint32_t>(field.data.data(), BTR_EXTERN_PAGE);
                uint32_t len = read_le<uint32_t>(field.data.data(), BTR_EXTERN_LEN);
                
                if (page_no != 0 && len > 0 && len < 1000000) {
                    auto blob_data = read_blob_chain(file_path, page_no, len);
                    
                    if (!blob_data.empty()) {
                        HeuristicRecord full_rec = rec;
                        HeuristicField full_field = field;
                        full_field.data = blob_data;
                        full_field.is_valid = true;
                        full_rec.fields[i] = full_field;
                        records.push_back(full_rec);
                    }
                }
            }
        }
    }
    
    return records;
}

// ==================== BLOB Pattern Scanner (for extreme corruption) ====================

std::vector<RecoveredBLOB> BlobScanner::scan_for_blobs_by_pattern(
    const std::string& file_path) {
    
    std::vector<RecoveredBLOB> recovered;
    
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return recovered;
    }
    
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Read file in chunks
    const size_t chunk_size = 1024 * 1024;  // 1MB chunks
    std::vector<uint8_t> buffer(chunk_size);
    
    size_t position = 0;
    while (position < file_size) {
        size_t to_read = std::min(chunk_size, file_size - position);
        file.read(reinterpret_cast<char*>(buffer.data()), to_read);
        
        // Search for BLOB patterns
        // 1. Long ASCII strings (> 255 chars)
        // 2. JSON/XML data patterns
        // 3. Base64 patterns
        
        for (size_t i = 0; i < to_read - 1000; i++) {
            // Check for long ASCII sequence
            size_t ascii_len = 0;
            while (i + ascii_len < to_read && 
                   (buffer[i + ascii_len] >= 32 && buffer[i + ascii_len] < 127 ||
                    buffer[i + ascii_len] == '\n' || buffer[i + ascii_len] == '\r')) {
                ascii_len++;
            }
            
            if (ascii_len > 255) {
                RecoveredBLOB rb;
                rb.data.assign(buffer.begin() + i, buffer.begin() + i + ascii_len);
                rb.total_length = ascii_len;
                rb.is_text = true;
                rb.source_offset = position + i;
                recovered.push_back(rb);
                
                i += ascii_len;
            }
        }
        
        position += chunk_size;
    }
    
    return recovered;
}

// ==================== ZBLOB Streaming Decompression ====================

/**
 * ZBLOB Page Header Layout:
 * Offset 0-3: FIL header space_id (should be 0 for BLOB)
 * Offset 4-7: FIL header page_no
 * Offset 8-11: Next page number (4294967295 = 0xFFFFFFFF if last)
 * Offset 12-15: Previous page number
 * Offset 16-37: Additional BLOB metadata
 * Offset 38+: Compressed data (zlib stream)
 */

constexpr uint16_t ZBLOB_NEXT_PAGE = 8;     // Next page offset in ZBLOB
constexpr uint16_t ZBLOB_DATA_START = 38;   // Compressed data starts here
constexpr uint32_t ZBLOB_END_MARKER = 0xFFFFFFFF;  // End of chain marker

std::vector<uint8_t> BlobScanner::read_zblob_chain(
    const std::string& file_path,
    uint32_t first_page) {

    constexpr size_t MAX_BLOB_SIZE = 48 * 1024 * 1024;
    std::vector<uint8_t> result;
    std::set<uint32_t> visited_pages;
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file || page_size_ == 0 || page_size_ <= ZBLOB_DATA_START) return {};
    const std::streamoff file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    z_stream strm{};
    if (inflateInit(&strm) != Z_OK) return {};
    bool stream_finished = false;
    uint32_t current_page = first_page;
    uint8_t out_buffer[64 * 1024];

    while (current_page != ZBLOB_END_MARKER && visited_pages.size() < 10000) {
        if (!visited_pages.insert(current_page).second) {
            inflateEnd(&strm);
            return {};
        }
        const uint64_t page_offset = static_cast<uint64_t>(current_page) * page_size_;
        if (page_offset > static_cast<uint64_t>(file_size) ||
            page_size_ > static_cast<size_t>(file_size - static_cast<std::streamoff>(page_offset))) {
            inflateEnd(&strm);
            return {};
        }
        file.clear();
        file.seekg(static_cast<std::streamoff>(page_offset), std::ios::beg);
        std::vector<uint8_t> page_buffer(page_size_);
        file.read(reinterpret_cast<char*>(page_buffer.data()), page_size_);
        if (file.gcount() != static_cast<std::streamsize>(page_size_)) {
            inflateEnd(&strm);
            return {};
        }

        const uint32_t next_page = read_be<uint32_t>(page_buffer.data(), ZBLOB_NEXT_PAGE);
        const size_t compressed_size = page_size_ - ZBLOB_DATA_START;
        if (compressed_size > static_cast<size_t>(std::numeric_limits<uInt>::max())) {
            inflateEnd(&strm);
            return {};
        }
        strm.avail_in = static_cast<uInt>(compressed_size);
        strm.next_in = page_buffer.data() + ZBLOB_DATA_START;

        do {
            strm.avail_out = sizeof(out_buffer);
            strm.next_out = out_buffer;
            const int ret = inflate(&strm, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                inflateEnd(&strm);
                return {};
            }
            const size_t have = sizeof(out_buffer) - strm.avail_out;
            if (have > MAX_BLOB_SIZE - result.size()) {
                inflateEnd(&strm);
                return {};
            }
            result.insert(result.end(), out_buffer, out_buffer + have);
            if (ret == Z_STREAM_END) {
                stream_finished = true;
                if (strm.avail_in != 0) {
                    inflateEnd(&strm);
                    return {};
                }
                break;
            }
        } while (strm.avail_out == 0 || strm.avail_in != 0);

        if (stream_finished) break;
        current_page = next_page;
    }

    inflateEnd(&strm);
    if (!stream_finished) return {};
    stats_.total_blob_bytes += static_cast<uint32_t>(result.size());
    return result;
}

std::vector<uint8_t> BlobScanner::decompress_zblob_page(
    const std::vector<uint8_t>& compressed_data,
    void* decompressor_state) {

    std::vector<uint8_t> result;
    constexpr size_t MAX_BLOB_SIZE = 48 * 1024 * 1024;
    if (compressed_data.empty() ||
        compressed_data.size() > static_cast<size_t>(std::numeric_limits<uInt>::max())) {
        return result;
    }
    bool stream_finished = false;
    z_stream* strm = nullptr;
    bool own_strm = false;

    if (decompressor_state) {
        strm = static_cast<z_stream*>(decompressor_state);
    } else {
        strm = new z_stream;
        memset(strm, 0, sizeof(z_stream));
        if (inflateInit(strm) != Z_OK) {
            delete strm;
            return result;
        }
        own_strm = true;
    }

    strm->avail_in = static_cast<uInt>(compressed_data.size());
    strm->next_in = const_cast<uint8_t*>(compressed_data.data());

    uint8_t out_buffer[16 * 1024];

    do {
        strm->avail_out = sizeof(out_buffer);
        strm->next_out = out_buffer;

        const int ret = inflate(strm, Z_NO_FLUSH);

        if (ret != Z_OK && ret != Z_STREAM_END) {
            if (own_strm) inflateEnd(strm);
            if (own_strm) delete strm;
            return {};
        }

        const size_t have = sizeof(out_buffer) - strm->avail_out;
        if (have > MAX_BLOB_SIZE - result.size()) {
            if (own_strm) inflateEnd(strm);
            if (own_strm) delete strm;
            return {};
        }
        result.insert(result.end(), out_buffer, out_buffer + have);

        if (ret == Z_STREAM_END) {
            stream_finished = true;
            if (strm->avail_in != 0) {
                if (own_strm) inflateEnd(strm);
                if (own_strm) delete strm;
                return {};
            }
            break;
        }
    } while (strm->avail_out == 0);

    if (own_strm) {
        inflateEnd(strm);
        delete strm;
    }

    return stream_finished ? result : std::vector<uint8_t>{};
}

std::vector<uint8_t> BlobScanner::read_blob_auto(
    const std::string& file_path,
    uint32_t first_page,
    uint32_t total_length,
    uint16_t page_type) {

    // Auto-detect page type if not specified
    if (page_type == 0) {
        std::ifstream file(file_path, std::ios::binary);
        if (file) {
            file.seekg(static_cast<std::streamoff>(first_page) * page_size_ + 24, std::ios::beg);
            uint16_t detected_type = 0;
            file.read(reinterpret_cast<char*>(&detected_type), 2);
            // Convert from big-endian
            page_type = (detected_type >> 8) | (detected_type << 8);
        }
    }

    // Check page type
    if (page_type == static_cast<uint16_t>(BlobPageType::ZBLOB) ||
        page_type == static_cast<uint16_t>(BlobPageType::ZBLOB2)) {
        return read_zblob_chain(file_path, first_page);
    } else {
        // Regular BLOB
        return read_blob_chain(file_path, first_page, total_length);
    }
}

} // namespace innodb
} // namespace dbrecovery
