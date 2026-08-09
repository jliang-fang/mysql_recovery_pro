/**
 * @file compressed_table_recovery.cpp
 * @brief Compressed table recovery implementations
 *
 * Extracted from compressed_table_recovery.hpp for better compilation.
 */

#include "dbrecovery/innodb/compressed_table_recovery.hpp"

#include <cstring>
#include <chrono>
#include <fstream>
#include <zlib.h>

namespace dbrecovery {
namespace innodb {

// ==================== CompressedTableRecovery ====================

CompressedTableRecovery::CompressedTableRecovery(size_t page_size)
    : page_size_(page_size), compressed_page_size_(16384) {
    std::memset(&stats_, 0, sizeof(stats_));
}

void CompressedTableRecovery::set_file_path(const std::filesystem::path& file_path) {
    file_path_ = file_path;
}

void CompressedTableRecovery::set_schema(const catalog::TableSchema& schema) {
    schema_ = schema;
}

void CompressedTableRecovery::set_compressed_page_size(size_t compressed_size) {
    compressed_page_size_ = compressed_size;
}

void CompressedTableRecovery::clear() {
    compressed_pages_.clear();
    records_.clear();
    std::memset(&stats_, 0, sizeof(stats_));
}

std::vector<ScannedPage> CompressedTableRecovery::scan_pages() {
    auto start = std::chrono::high_resolution_clock::now();

    compressed_pages_.clear();
    std::vector<ScannedPage> pages;

    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return pages;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    size_t total_pages = file_size / page_size_;
    stats_.total_pages = total_pages;

    if (total_pages == 0) {
        return pages;
    }

    // Deterministic fixed-size reads; page numbers derive from full physical
    // file offsets so they do not reset per buffer. FIL fields parsed with the
    // canonical big-endian offsets from page_defs.hpp.
    std::vector<uint8_t> page_buf(page_size_);
    for (size_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(static_cast<std::streamoff>(page_no * page_size_), std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buf.data()),
                  static_cast<std::streamsize>(page_size_));
        if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
            break;
        }

        const uint8_t* p = page_buf.data();
        ScannedPage page{};
        page.offset = static_cast<uint32_t>(page_no * page_size_);
        page.page_no = static_cast<uint32_t>(page_no);
        page.space_id = read_be<uint32_t>(p, FIL_PAGE_SPACE_ID);
        page.page_no = read_be<uint32_t>(p, FIL_PAGE_OFFSET);
        page.prev_page = read_be<uint32_t>(p, FIL_PAGE_PREV_ABS);
        page.next_page = read_be<uint32_t>(p, FIL_PAGE_NEXT_ABS);
        page.lsn = read_be<uint64_t>(p, FIL_PAGE_LSN_ABS);
        page.type = read_be<uint16_t>(p, FIL_PAGE_TYPE_ABS);
        page.page_type = page.type;
        page.is_valid = PageScanner::is_innodb_page(p, page_size_);

        if (is_page_compressed(p, page_size_)) {
            page.is_compressed = true;
            compressed_pages_.push_back(page.page_no);
        }

        pages.push_back(page);
    }

    stats_.compressed_pages = compressed_pages_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.decompress_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return pages;
}

bool CompressedTableRecovery::is_page_compressed(const uint8_t* page_data, size_t page_size) {
    // For compressed pages, the on-disk format is:
    //   [ZSD header: 32 bytes (little-endian)]
    //   [Compressed payload: zlen bytes]
    // The FIL header is inside the compressed data, so we cannot check
    // page type without decompressing first. Instead, validate the ZSD
    // header structure directly.
    //
    // ZSD header layout (32 bytes, all fields little-endian):
    //   offset 0-1: zip_size (in 512-byte units, 8/16/32 = 4K/8K/16K)
    //   offset 2-3: version (1)
    //   offset 4-7: checksum
    //   offset 8-9: zlen (compressed payload length)
    //   offset 10-13: original_size (upper)
    //   offset 14-17: original_size (lower)
    //   offset 18-19: compression_type (0=zlib, 1=LZ4)
    //   offset 20-31: padding
    if (page_size < 38 + 32) {
        return false;
    }

    uint16_t zip_size = read_le<uint16_t>(page_data, 0);
    if (zip_size != 8 && zip_size != 16 && zip_size != 32) return false;

    if (static_cast<uint32_t>(zip_size) * 512 > page_size) return false;

    uint16_t version = read_le<uint16_t>(page_data, 2);
    if (version != 1) return false;

    uint16_t zlen = read_le<uint16_t>(page_data, 8);
    if (zlen == 0 || zlen > static_cast<uint16_t>(zip_size * 512)) return false;

    uint32_t orig_size = read_le<uint32_t>(page_data, 14);
    if (orig_size != 4096 && orig_size != 8192 && orig_size != 16384 &&
        orig_size != 32768 && orig_size != 65536) return false;

    return true;
}

std::vector<uint8_t> CompressedTableRecovery::decompress_page_data(
    const uint8_t* compressed_data, size_t compressed_size, size_t expected_size) {

    auto start = std::chrono::high_resolution_clock::now();

    constexpr size_t ZSD_HEADER_SIZE = 32;
    constexpr size_t MAX_COMPRESSION_RATIO = 256;  // max 256:1 compression ratio

    if (compressed_size < ZSD_HEADER_SIZE) {
        return {};
    }

    uint16_t zlen = read_le<uint16_t>(compressed_data, 8);
    uint16_t compression_type = read_le<uint16_t>(compressed_data, 18);

    const uint8_t* payload = compressed_data + ZSD_HEADER_SIZE;
    size_t payload_size = std::min(static_cast<size_t>(zlen), compressed_size - ZSD_HEADER_SIZE);

    // Compression ratio safety check: reject if zlen > 512 * expected_size
    if (zlen > static_cast<uint16_t>(expected_size * 512)) {
        return {};
    }

    // Safety: reject extreme compression ratios (e.g., 16KB → 64 bytes)
    if (payload_size > 0 && expected_size / payload_size > MAX_COMPRESSION_RATIO) {
        return {};
    }

    std::vector<uint8_t> decompressed(expected_size, 0);

    auto end_decompress = [&](bool success, size_t actual_size) {
        auto end = std::chrono::high_resolution_clock::now();
        stats_.decompress_time_ms += std::chrono::duration<double, std::milli>(end - start).count();
        if (success && actual_size == expected_size) {
            stats_.decompressed_pages++;
            return decompressed;
        }
        return std::vector<uint8_t>{};
    };

    if (compression_type == 0) {
        uLong dest_len = static_cast<uLong>(expected_size);
        int result = uncompress(decompressed.data(), &dest_len,
                                payload, static_cast<uLong>(payload_size));
        if (result == Z_OK) {
            return end_decompress(true, dest_len);
        }
    }

    {
        auto lz4_result = dbrecovery::utils::LZ4Helper::decompress(
            payload, payload_size, expected_size);
        if (!lz4_result.empty()) {
            decompressed = std::move(lz4_result);
            return end_decompress(true, decompressed.size());
        }
    }

    if (compression_type != 0) {
        uLong dest_len = static_cast<uLong>(expected_size);
        int result = uncompress(decompressed.data(), &dest_len,
                                payload, static_cast<uLong>(payload_size));
        if (result == Z_OK) {
            return end_decompress(true, dest_len);
        }
    }

    return end_decompress(false, 0);
}

std::vector<std::map<std::string, std::string>> CompressedTableRecovery::parse_page_records(
    const std::vector<ScannedPage>& pages) {

    auto start = std::chrono::high_resolution_clock::now();

    records_.clear();

    for (const auto& page : pages) {
        if (!page.is_valid) {
            continue;
        }

        std::ifstream file(file_path_, std::ios::binary);
        if (!file) continue;

        size_t offset = static_cast<size_t>(page.page_no) * page_size_;
        file.seekg(offset, std::ios::beg);

        std::vector<uint8_t> page_data(page_size_);
        file.read(reinterpret_cast<char*>(page_data.data()), page_size_);

        if (file.gcount() < static_cast<std::streamsize>(page_size_)) {
            continue;
        }

        std::vector<uint8_t> working_data = page_data;
        if (page.is_compressed) {
            working_data = decompress_page_data(page_data.data(), page_data.size(), page_size_);
            if (working_data.empty()) {
                continue;
            }
        }

        BPlusTreeParser parser(working_data);
        if (!parser.parse()) {
            continue;
        }

        auto page_records = parser.parse_records(schema_);
        records_.insert(records_.end(), page_records.begin(), page_records.end());
    }

    stats_.recovered_records = records_.size();

    auto end = std::chrono::high_resolution_clock::now();
    stats_.recovery_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.total_time_ms = stats_.decompress_time_ms + stats_.recovery_time_ms;

    return records_;
}

std::vector<std::map<std::string, std::string>> CompressedTableRecovery::recover() {
    auto total_start = std::chrono::high_resolution_clock::now();

    clear();

    auto all_pages = scan_pages();

    records_ = parse_page_records(all_pages);

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

// ==================== CompressedTableRecoveryManager ====================

CompressedTableRecoveryManager::CompressedTableRecoveryManager() {}

bool CompressedTableRecovery::auto_detect_compression(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return false;

    std::vector<uint8_t> page0(16384, 0);
    file.read(reinterpret_cast<char*>(page0.data()), page0.size());
    if (file.gcount() < 46) return false;

    // FSP_SPACE_FLAGS at absolute offset 42 (FIL_HEADER_SIZE + 4), big-endian
    uint32_t fsp_flags = read_be<uint32_t>(page0.data(), 42);
    // ZIP_SSIZE at bits 1-4 (4 bits): compressed_page_size = 16384 / (1 << zip_ssize)
    uint32_t zip_ssize = (fsp_flags >> 1) & 0x0F;

    // Validate zip_ssize: values >= 5 produce invalid page sizes (< 1024 bytes)
    if (zip_ssize >= 5) {
        return false;
    }

    if (zip_ssize > 0) {
        compressed_page_size_ = 16384 / (1 << zip_ssize);
        return true;
    }
    return false;
}

void CompressedTableRecoveryManager::add_compressed_table(const std::string& table_name,
                                                          uint32_t compressed_page_size) {
    tables_[table_name] = compressed_page_size;
}

void CompressedTableRecoveryManager::set_compressed_page_size(const std::string& table_name,
                                                              size_t size) {
    tables_[table_name] = size;
}

std::optional<size_t> CompressedTableRecoveryManager::get_compressed_page_size(
    const std::string& table_name) const {
    auto it = tables_.find(table_name);
    if (it != tables_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<size_t> CompressedTableRecoveryManager::parse_compression_info(
    const std::string& ibd_path) {
    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 16384) return std::nullopt;

    std::vector<uint8_t> page_data(16384);
    file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

    // FSP_SPACE_FLAGS at absolute offset 42 (FIL_HEADER_SIZE + 4), big-endian
    // Bit layout: POST_ANTELOPE=bit0, ZIP_SSIZE=bits1-4, ATOMIC_BLOBS=bit5
    // Compressed page size = 16384 / (1 << zip_ssize) for zip_ssize > 0
    if (page_data.size() >= 46) {
        uint32_t fsp_flags = read_be<uint32_t>(page_data.data(), 42);
        uint32_t zip_ssize = (fsp_flags >> 1) & 0x0F;
        if (zip_ssize > 0) {
            return static_cast<size_t>(16384 / (1 << zip_ssize));
        }
    }

    return std::nullopt;
}

} // namespace innodb
} // namespace dbrecovery