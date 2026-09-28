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
#include <limits>
#include <zlib.h>

namespace dbrecovery {
namespace innodb {

// ==================== CompressedTableRecovery ====================

CompressedTableRecovery::CompressedTableRecovery(size_t page_size)
    : page_size_(page_size), compressed_page_size_(page_size) {
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

    // Use compressed_page_size_ as stride when the on-disk pages are smaller
    // than the logical page_size_ (key fix for compressed tables — without this
    // the scan reads inter-page boundaries and breaks on the first page).
    size_t stride = compressed_page_size_ > 0 && compressed_page_size_ < page_size_
                        ? compressed_page_size_ : page_size_;
    size_t total_pages = file_size / stride;
    stats_.total_pages = total_pages;

    if (total_pages == 0) {
        return pages;
    }

    // Deterministic fixed-size reads; page numbers derive from full physical
    // file offsets so they do not reset per buffer. FIL fields parsed with the
    // canonical big-endian offsets from page_defs.hpp.
    std::vector<uint8_t> page_buf(stride);
    for (size_t page_no = 0; page_no < total_pages; ++page_no) {
        file.seekg(static_cast<std::streamoff>(page_no * stride), std::ios::beg);
        file.read(reinterpret_cast<char*>(page_buf.data()),
                  static_cast<std::streamsize>(stride));
        if (file.gcount() < static_cast<std::streamsize>(stride)) {
            break;
        }

        const uint8_t* p = page_buf.data();
        ScannedPage page{};
        page.offset = static_cast<uint32_t>(page_no * stride);
        page.physical_page_index = static_cast<uint32_t>(page_no);
        page.space_id = read_be<uint32_t>(p, FIL_PAGE_SPACE_ID);
        page.page_no = read_be<uint32_t>(p, FIL_PAGE_OFFSET);
        page.prev_page = read_be<uint32_t>(p, FIL_PAGE_PREV_ABS);
        page.next_page = read_be<uint32_t>(p, FIL_PAGE_NEXT_ABS);
        page.lsn = read_be<uint64_t>(p, FIL_PAGE_LSN_ABS);
        page.type = read_be<uint16_t>(p, FIL_PAGE_TYPE_ABS);
        page.page_type = page.type;
        page.is_valid = PageScanner::is_innodb_page(p, stride);

        // Identity validation: the header page_no must match the physical
        // position in the file.  A mismatch indicates page-level corruption,
        // misaligned scan, or cross-file reassembly.  Reject such pages.
        if (page.is_valid && page.page_no != page.physical_page_index) {
            page.is_valid = false;
        }

        if (is_page_compressed(p, stride)) {
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
    // The custom little-endian ZSD byte-header heuristic has never been
    // validated against a real compressed .ibd and is NOT reliable enough to
    // classify ordinary tablespaces, so this path is disabled. Compressed
    // layout detection must come from canonical FSP flags (page 0 only).
    (void)page_data;
    (void)page_size;
    return false;
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

    // Compression ratio safety check: reject if zlen > 512 * expected_size.
    const size_t max_compressed_size =
        expected_size > std::numeric_limits<size_t>::max() / 512
            ? std::numeric_limits<size_t>::max()
            : expected_size * 512;
    if (static_cast<size_t>(zlen) > max_compressed_size) {
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

        size_t stride = compressed_page_size_ > 0 && compressed_page_size_ < page_size_
                            ? compressed_page_size_ : page_size_;
        size_t offset = static_cast<size_t>(page.physical_page_index) * stride;
        file.seekg(offset, std::ios::beg);

        std::vector<uint8_t> page_data(stride);
        file.read(reinterpret_cast<char*>(page_data.data()), stride);

        if (file.gcount() < static_cast<std::streamsize>(stride)) {
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

    // Without a fixture-validated compressed-page decoder, report
    // RECOGNIZED_UNSUPPORTED and emit zero records.  This prevents
    // claiming success on an empty result set.
    if (!decoder_is_validated()) {
        stats_.status = CompressionStatus::RECOGNIZED_UNSUPPORTED;
        auto total_end = std::chrono::high_resolution_clock::now();
        stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
        return records_;
    }

    auto all_pages = scan_pages();

    records_ = parse_page_records(all_pages);

    stats_.status = records_.empty()
        ? CompressionStatus::RECOGNIZED_UNSUPPORTED
        : CompressionStatus::RECOVERED;

    auto total_end = std::chrono::high_resolution_clock::now();
    stats_.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();

    return records_;
}

// ==================== CompressedTableRecoveryManager ====================

CompressedTableRecoveryManager::CompressedTableRecoveryManager() {}

CompressionStatus CompressedTableRecovery::classify(const std::string& file_path, size_t page_size) const {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return CompressionStatus::MALFORMED;

    std::vector<uint8_t> page0(page_size, 0);
    file.read(reinterpret_cast<char*>(page0.data()), page0.size());
    if (file.gcount() < 46) return CompressionStatus::MALFORMED;

    const auto metadata_result = parse_fsp_compression_metadata_detailed(
        page0.data(), static_cast<size_t>(file.gcount()), page_size);
    if (!metadata_result) {
        if (metadata_result.status == FSPCompressionMetadataStatus::INVALID_ZIP_SSIZE ||
            metadata_result.status == FSPCompressionMetadataStatus::INVALID_PHYSICAL_PAGE_SIZE) {
            return CompressionStatus::CORRUPTED_METADATA;
        }
        return CompressionStatus::MALFORMED;
    }
    const auto& metadata = *metadata_result.metadata;
    if (!metadata.is_compressed()) return CompressionStatus::NOT_COMPRESSED;

    // Verify that the on-disk page stride matches the declared physical page size.
    // Read the second physical page (stride = physical_page_size) and check for
    // a valid InnoDB page type.  If the stride doesn't match, the file layout is
    // inconsistent with the FSP flags declaration.
    if (metadata.physical_page_size < page_size) {
        file.seekg(0, std::ios::end);
        const size_t file_size = static_cast<size_t>(file.tellg());
        if (file_size >= metadata.physical_page_size * 2) {
            file.seekg(static_cast<std::streamoff>(metadata.physical_page_size), std::ios::beg);
            std::vector<uint8_t> page1(metadata.physical_page_size, 0);
            file.read(reinterpret_cast<char*>(page1.data()), page1.size());
            if (file.gcount() < static_cast<std::streamsize>(metadata.physical_page_size)) {
                return CompressionStatus::PHYSICAL_LENGTH_MISMATCH;
            }
            const uint16_t page1_type = read_be<uint16_t>(page1.data(), FIL_PAGE_TYPE_ABS);
            if (page1_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
                return CompressionStatus::COMPRESSED_AND_ENCRYPTED;
            }
            // Accept any known InnoDB page type; anything else means the stride is wrong.
            if (page1_type != FIL_PAGE_INDEX &&
                page1_type != FIL_PAGE_TYPE_FSP_HDR &&
                page1_type != FIL_PAGE_TYPE_XDES &&
                page1_type != FIL_PAGE_SDI &&
                page1_type != FIL_PAGE_SDI_ZIP &&
                page1_type != FIL_PAGE_TYPE_BLOB &&
                page1_type != FIL_PAGE_UNDO_LOG &&
                page1_type != FIL_PAGE_COMPRESSED) {
                return CompressionStatus::PHYSICAL_LENGTH_MISMATCH;
            }
        }
    }

    // Canonical FSP flags indicate compression.  Without a fixture-validated
    // decoder, report RECOGNIZED_UNSUPPORTED — never claim recovery.
    return CompressionStatus::RECOGNIZED_UNSUPPORTED;
}

bool CompressedTableRecovery::decoder_is_validated() {
    // No fixture-validated compressed-page decoder exists yet.  Return false
    // so that recover() for compressed layout reports RECOGNIZED_UNSUPPORTED
    // instead of claiming successful recovery.
    return false;
}

bool CompressedTableRecovery::auto_detect_compression(const std::string& file_path) {
    const CompressionStatus status = classify(file_path, page_size_);
    if (status == CompressionStatus::RECOGNIZED_UNSUPPORTED) {
        // Store the physical page size from FSP flags for metadata reporting
        std::ifstream file(file_path, std::ios::binary);
        std::vector<uint8_t> page0(page_size_, 0);
        file.read(reinterpret_cast<char*>(page0.data()), page0.size());
        auto metadata = parse_fsp_compression_metadata(page0.data(),
                                                       static_cast<size_t>(file.gcount()),
                                                       page_size_);
        if (metadata) compressed_page_size_ = metadata->physical_page_size;
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
    const std::string& ibd_path, size_t page_size) {
    std::ifstream file(ibd_path, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < page_size) return std::nullopt;

    std::vector<uint8_t> page_data(page_size);
    file.read(reinterpret_cast<char*>(page_data.data()), page_data.size());

    auto metadata = parse_fsp_compression_metadata(page_data.data(),
                                                   static_cast<size_t>(file.gcount()),
                                                   page_size);
    if (metadata && metadata->is_compressed()) {
        return metadata->physical_page_size;
    }

    return std::nullopt;
}

} // namespace innodb
} // namespace dbrecovery