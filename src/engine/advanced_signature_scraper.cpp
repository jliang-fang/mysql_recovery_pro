/**
 * @file advanced_signature_scraper.cpp
 * @brief Implementation of Advanced Signature-Based Page Scraper
 *
 * @version 2.2.0
 */

#include "dbrecovery/engine/advanced_signature_scraper.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <atomic>

// CRC32C implementation - use built-in if available, otherwise fallback
namespace {
// Simple CRC32C implementation for systems without crc32c library
uint32_t crc32c_extend(uint32_t crc, const uint8_t* data, size_t len) {
    // CRC32C polynomial (Castagnoli)
    static constexpr uint32_t POLY = 0x82F63B78;

    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ ((crc & 1) ? POLY : 0);
        }
    }
    return ~crc;
}
}

namespace dbrecovery {
namespace engine {

// ==================== PageSignatureDef Implementation ====================

bool PageSignatureDef::matches(const uint8_t* data, size_t size) const {
    if (signature_offset + signature.size() > size) {
        return false;
    }

    const uint8_t* ptr = data + signature_offset;

    if (mask.empty()) {
        return memcmp(ptr, signature.data(), signature.size()) == 0;
    } else {
        for (size_t i = 0; i < signature.size(); ++i) {
            if ((ptr[i] & mask[i]) != (signature[i] & mask[i])) {
                return false;
            }
        }
        return true;
    }
}

// ==================== InnoDBSignatureLibrary Implementation ====================

std::unordered_map<MySQLVersion, std::vector<PageSignatureDef>>
    InnoDBSignatureLibrary::signature_cache_;

std::vector<PageSignatureDef> InnoDBSignatureLibrary::get_mysql56_signatures() {
    std::vector<PageSignatureDef> sigs;

    // FIL_PAGE_INDEX signature (most common)
    {
        PageSignatureDef sig;
        sig.name = "FIL_PAGE_INDEX";
        sig.page_type = FIL_PAGE_INDEX;
        sig.signature_offset = FIL_PAGE_TYPE_OFFSET;
        sig.signature = {0x45, 0xBF};  // 17855 in big-endian
        sig.confidence_weight = 1.0;
        sig.min_version = MySQLVersion::MYSQL_56;
        sig.max_version = MySQLVersion::MYSQL_57;
        sigs.push_back(sig);
    }

    // FIL_PAGE_BLOB signature
    {
        PageSignatureDef sig;
        sig.name = "FIL_PAGE_BLOB";
        sig.page_type = FIL_PAGE_BLOB;
        sig.signature_offset = FIL_PAGE_TYPE_OFFSET;
        sig.signature = {0x45, 0xBD};  // 17853
        sig.confidence_weight = 0.9;
        sig.min_version = MySQLVersion::MYSQL_56;
        sig.max_version = MySQLVersion::MYSQL_90;
        sigs.push_back(sig);
    }

    // FIL_PAGE_UNDO_LOG signature
    {
        PageSignatureDef sig;
        sig.name = "FIL_PAGE_UNDO_LOG";
        sig.page_type = FIL_PAGE_UNDO_LOG;
        sig.signature_offset = FIL_PAGE_TYPE_OFFSET;
        sig.signature = {0x45, 0xB2};  // 17842
        sig.confidence_weight = 0.8;
        sig.min_version = MySQLVersion::MYSQL_56;
        sig.max_version = MySQLVersion::MYSQL_90;
        sigs.push_back(sig);
    }

    // FIL_PAGE_INODE signature
    {
        PageSignatureDef sig;
        sig.name = "FIL_PAGE_INODE";
        sig.page_type = FIL_PAGE_INODE;
        sig.signature_offset = FIL_PAGE_TYPE_OFFSET;
        sig.signature = {0x45, 0xB3};  // 17843
        sig.confidence_weight = 0.85;
        sig.min_version = MySQLVersion::MYSQL_56;
        sig.max_version = MySQLVersion::MYSQL_90;
        sigs.push_back(sig);
    }

    // FIL_PAGE_IBUF_BITMAP signature
    {
        PageSignatureDef sig;
        sig.name = "FIL_PAGE_IBUF_BITMAP";
        sig.page_type = FIL_PAGE_IBUF_BITMAP;
        sig.signature_offset = FIL_PAGE_TYPE_OFFSET;
        sig.signature = {0x45, 0xB5};  // 17845
        sig.confidence_weight = 0.7;
        sig.min_version = MySQLVersion::MYSQL_56;
        sig.max_version = MySQLVersion::MYSQL_90;
        sigs.push_back(sig);
    }

    return sigs;
}

std::vector<PageSignatureDef> InnoDBSignatureLibrary::get_mysql57_signatures() {
    auto sigs = get_mysql56_signatures();
    // MySQL 5.7 uses same signatures as 5.6
    return sigs;
}

std::vector<PageSignatureDef> InnoDBSignatureLibrary::get_mysql80_signatures() {
    auto sigs = get_mysql56_signatures();

    // Add FIL_PAGE_SDI (MySQL 8.0+)
    {
        PageSignatureDef sig;
        sig.name = "FIL_PAGE_SDI";
        sig.page_type = FIL_PAGE_SDI;
        sig.signature_offset = FIL_PAGE_TYPE_OFFSET;
        sig.signature = {0x45, 0xBE};  // 17854
        sig.confidence_weight = 0.95;
        sig.min_version = MySQLVersion::MYSQL_80;
        sig.max_version = MySQLVersion::MYSQL_90;
        sigs.push_back(sig);
    }

    return sigs;
}

std::vector<PageSignatureDef> InnoDBSignatureLibrary::get_signatures(MySQLVersion version) {
    switch (version) {
        case MySQLVersion::MYSQL_56:
        case MySQLVersion::MYSQL_57:
            return get_mysql56_signatures();
        case MySQLVersion::MYSQL_80:
        case MySQLVersion::MYSQL_84:
        case MySQLVersion::MYSQL_90:
            return get_mysql80_signatures();
        default:
            return get_mysql56_signatures();
    }
}

std::optional<PageSignatureDef> InnoDBSignatureLibrary::get_signature(
    uint16_t page_type, MySQLVersion version) {

    auto sigs = get_signatures(version);
    for (const auto& sig : sigs) {
        if (sig.page_type == page_type) {
            return sig;
        }
    }
    return std::nullopt;
}

double InnoDBSignatureLibrary::match_signature(
    const uint8_t* data, size_t size, const PageSignatureDef& sig) {

    if (!sig.matches(data, size)) {
        return 0.0;
    }

    // Base confidence from signature weight
    double confidence = sig.confidence_weight;

    // Additional checks for INDEX pages
    if (sig.page_type == FIL_PAGE_INDEX) {
        // Check for valid INFIMUM/SUPREMUM markers
        if (size >= 120) {
            // INFIMUM should be at offset 99
            if (data[INFIMUM_OFFSET] == 0x01 || data[INFIMUM_OFFSET] == 0x02) {
                confidence += 0.1;
            }
            // SUPREMUM marker check
            if (data[SUPREMUM_OFFSET] == 0x03 || data[SUPREMUM_OFFSET] == 0x04) {
                confidence += 0.1;
            }
        }

        // Check for reasonable page level (0-3)
        if (size >= PAGE_HEADER_OFFSET + PAGE_LEVEL_OFFSET + 2) {
            uint16_t level = (data[PAGE_HEADER_OFFSET + PAGE_LEVEL_OFFSET] << 8) |
                             data[PAGE_HEADER_OFFSET + PAGE_LEVEL_OFFSET + 1];
            if (level <= 3) {
                confidence += 0.05;
            }
        }
    }

    return std::min(confidence, 1.0);
}

uint16_t InnoDBSignatureLibrary::detect_page_type(const uint8_t* data, size_t size) {
    if (size < FIL_PAGE_TYPE_OFFSET + 2) {
        return 0xFFFF;
    }

    uint16_t page_type = (data[FIL_PAGE_TYPE_OFFSET] << 8) | data[FIL_PAGE_TYPE_OFFSET + 1];
    return page_type;
}

MySQLVersion InnoDBSignatureLibrary::detect_version(const uint8_t* page_data, size_t page_size) {
    if (page_size < 100) {
        return MySQLVersion::UNKNOWN;
    }

    uint16_t page_type = detect_page_type(page_data, page_size);

    // SDI pages only exist in MySQL 8.0+
    if (page_type == FIL_PAGE_SDI) {
        return MySQLVersion::MYSQL_80;
    }

    // Check for MySQL 5.6/5.7 specific patterns
    // In MySQL 5.6, the checksum algorithm can be detected from the stored checksum

    // Default to 5.6 for INDEX pages
    if (page_type == FIL_PAGE_INDEX) {
        // Try to detect from checksum format
        uint32_t checksum1 = (page_data[0] << 24) | (page_data[1] << 16) |
                              (page_data[2] << 8) | page_data[3];

        // CRC32C checksums typically have specific patterns
        // This is a heuristic
        if (checksum1 != 0 && checksum1 != 0xFFFFFFFF) {
            // Try CRC32C validation
            ChecksumAlgo algo;
            if (AdvancedPageCarver::validate_checksum(page_data, page_size, algo)) {
                if (algo == ChecksumAlgo::CRC32C) {
                    return MySQLVersion::MYSQL_57; // CRC32C default in 5.7+
                }
            }
        }

        return MySQLVersion::UNKNOWN;
    }

    return MySQLVersion::UNKNOWN;
}

bool InnoDBSignatureLibrary::is_valid_page_type(uint16_t type) {
    switch (type) {
        case FIL_PAGE_INDEX:
        case FIL_PAGE_UNDO_LOG:
        case FIL_PAGE_INODE:
        case FIL_PAGE_IBUF_FREE_LIST:
        case FIL_PAGE_IBUF_BITMAP:
        case FIL_PAGE_TYPE_ALLOCATED:
        case FIL_PAGE_SDI:
        case FIL_PAGE_BLOB:
        case FIL_PAGE_COMPRESSED:
        case FIL_PAGE_ENCRYPTED:
            return true;
        default:
            return false;
    }
}

bool InnoDBSignatureLibrary::is_leaf_page(const uint8_t* page_data, size_t page_size) {
    if (page_size < PAGE_HEADER_OFFSET + PAGE_LEVEL_OFFSET + 2) {
        return false;
    }

    uint16_t level = (page_data[PAGE_HEADER_OFFSET + PAGE_LEVEL_OFFSET] << 8) |
                     page_data[PAGE_HEADER_OFFSET + PAGE_LEVEL_OFFSET + 1];
    return level == 0;
}

bool InnoDBSignatureLibrary::is_root_page(const uint8_t* page_data, size_t page_size) {
    if (page_size < PAGE_HEADER_OFFSET + PAGE_LEVEL_OFFSET + 2) {
        return false;
    }

    uint16_t level = (page_data[PAGE_HEADER_OFFSET + PAGE_LEVEL_OFFSET] << 8) |
                     page_data[PAGE_HEADER_OFFSET + PAGE_LEVEL_OFFSET + 1];
    return level > 0;
}

std::optional<InnoDBSignatureLibrary::FILHeader> InnoDBSignatureLibrary::parse_fil_header(
    const uint8_t* data, size_t size) {

    if (size < 38) {
        return std::nullopt;
    }

    FILHeader header;
    header.checksum1 = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    header.page_number = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    header.prev_page = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];
    header.next_page = (data[12] << 24) | (data[13] << 16) | (data[14] << 8) | data[15];
    header.lsn = ((uint64_t)data[16] << 56) | ((uint64_t)data[17] << 48) |
                 ((uint64_t)data[18] << 40) | ((uint64_t)data[19] << 32) |
                 ((uint64_t)data[20] << 24) | ((uint64_t)data[21] << 16) |
                 ((uint64_t)data[22] << 8) | data[23];
    header.page_type = (data[24] << 8) | data[25];
    header.space_id = (data[34] << 24) | (data[35] << 16) | (data[36] << 8) | data[37];

    return header;
}

std::optional<InnoDBSignatureLibrary::PAGEHeader> InnoDBSignatureLibrary::parse_page_header(
    const uint8_t* data, size_t size) {

    if (size < PAGE_HEADER_OFFSET + 22) {
        return std::nullopt;
    }

    const uint8_t* ph = data + PAGE_HEADER_OFFSET;

    PAGEHeader header;
    header.n_dir_slots = (ph[0] << 8) | ph[1];
    header.heap_top = (ph[2] << 8) | ph[3];
    header.n_heap = (ph[4] << 8) | ph[5];
    header.free = (ph[6] << 8) | ph[7];
    header.garbage = (ph[8] << 8) | ph[9];
    header.level = (ph[12] << 8) | ph[13];
    header.index_id = ((uint64_t)ph[14] << 56) | ((uint64_t)ph[15] << 48) |
                      ((uint64_t)ph[16] << 40) | ((uint64_t)ph[17] << 32) |
                      ((uint64_t)ph[18] << 24) | ((uint64_t)ph[19] << 16) |
                      ((uint64_t)ph[20] << 8) | ph[21];

    return header;
}

// ==================== CarvedPage Implementation ====================

std::string CarvedPage::get_page_type_name() const {
    switch (page_type) {
        case InnoDBSignatureLibrary::FIL_PAGE_INDEX: return "INDEX";
        case InnoDBSignatureLibrary::FIL_PAGE_BLOB: return "BLOB";
        case InnoDBSignatureLibrary::FIL_PAGE_UNDO_LOG: return "UNDO_LOG";
        case InnoDBSignatureLibrary::FIL_PAGE_INODE: return "INODE";
        case InnoDBSignatureLibrary::FIL_PAGE_SDI: return "SDI";
        case InnoDBSignatureLibrary::FIL_PAGE_IBUF_BITMAP: return "IBUF_BITMAP";
        case InnoDBSignatureLibrary::FIL_PAGE_TYPE_ALLOCATED: return "ALLOCATED";
        case InnoDBSignatureLibrary::FIL_PAGE_COMPRESSED: return "COMPRESSED";
        case InnoDBSignatureLibrary::FIL_PAGE_ENCRYPTED: return "ENCRYPTED";
        default: return "UNKNOWN(" + std::to_string(page_type) + ")";
    }
}

// ==================== TwoLevelSignatureIndex Implementation ====================

TwoLevelSignatureIndex::TwoLevelSignatureIndex(const std::vector<PageSignatureDef>& signatures)
    : signatures_(signatures) {

    for (const auto& sig : signatures_) {
        uint32_t offset = sig.signature_offset;

        // Initialize offset entry if needed
        if (index_.find(offset) == index_.end()) {
            index_[offset] = {};
        }

        // Add signature to all matching first-byte buckets
        if (!sig.signature.empty()) {
            uint8_t first_byte = sig.signature[0];
            index_[offset][first_byte].push_back(&sig);

            // Also add to "any" bucket (255) for wildcard matching
            // index_[offset][255].push_back(&sig);
        }
    }
}

std::vector<const PageSignatureDef*> TwoLevelSignatureIndex::lookup(
    uint32_t offset, uint8_t first_byte) const {

    auto offset_it = index_.find(offset);
    if (offset_it == index_.end()) {
        return {};
    }

    // Access the array at first_byte index
    return offset_it->second[first_byte];
}

std::vector<const PageSignatureDef*> TwoLevelSignatureIndex::lookup_by_offset(
    uint32_t offset) const {

    auto offset_it = index_.find(offset);
    if (offset_it == index_.end()) {
        return {};
    }

    std::vector<const PageSignatureDef*> result;
    // Iterate through all 256 buckets
    for (const auto& bucket : offset_it->second) {
        for (const auto* sig : bucket) {
            result.push_back(sig);
        }
    }
    return result;
}

bool TwoLevelSignatureIndex::has_signatures_at(uint32_t offset) const {
    return index_.find(offset) != index_.end();
}

// ==================== AdvancedPageCarver Implementation ====================

AdvancedPageCarver::AdvancedPageCarver(EnhancedRawDiskEngine& engine)
    : engine_(engine) {

    // Initialize signature index for MySQL 5.6
    auto sigs = InnoDBSignatureLibrary::get_mysql56_signatures();
    signature_index_ = std::make_unique<TwoLevelSignatureIndex>(sigs);

    memset(&stats_, 0, sizeof(stats_));
}

AdvancedPageCarver::~AdvancedPageCarver() = default;

uint32_t AdvancedPageCarver::carve_pages(
    const CarvingConfig& config,
    std::function<void(const CarvedPage&)> callback) {

    if (!engine_.is_open() || !callback) {
        return 0;
    }

    auto start_time = std::chrono::steady_clock::now();
    uint64_t disk_size = engine_.get_size();
    uint32_t pages_found = 0;

    // Scan for each page size
    for (uint32_t page_size : config.page_sizes) {
        auto pages = sliding_window_scan(0, disk_size, page_size, config);

        for (const auto& page : pages) {
            callback(page);
            pages_found++;

            // Update stats
            stats_.pages_found++;
            if (page.checksum_valid) {
                stats_.pages_valid_checksum++;
            } else {
                stats_.pages_invalid_checksum++;
            }

            switch (page.page_type) {
                case InnoDBSignatureLibrary::FIL_PAGE_INDEX:
                    stats_.index_pages++;
                    break;
                case InnoDBSignatureLibrary::FIL_PAGE_BLOB:
                    stats_.blob_pages++;
                    break;
                case InnoDBSignatureLibrary::FIL_PAGE_SDI:
                    stats_.sdi_pages++;
                    break;
                case InnoDBSignatureLibrary::FIL_PAGE_UNDO_LOG:
                    stats_.undo_pages++;
                    break;
                case InnoDBSignatureLibrary::FIL_PAGE_TYPE_ALLOCATED:
                    stats_.allocated_pages++;
                    break;
            }

            if (config.max_pages > 0 && pages_found >= config.max_pages) {
                break;
            }
        }

        if (config.max_pages > 0 && pages_found >= config.max_pages) {
            break;
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    stats_.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
    stats_.bytes_scanned = disk_size;
    stats_.mb_per_second = (disk_size / (1024.0 * 1024.0)) / stats_.elapsed_seconds;

    return pages_found;
}

uint32_t AdvancedPageCarver::carve_range(
    uint64_t start_offset,
    uint64_t end_offset,
    const CarvingConfig& config,
    std::function<void(const CarvedPage&)> callback) {

    if (!engine_.is_open() || !callback) {
        return 0;
    }

    uint32_t pages_found = 0;

    for (uint32_t page_size : config.page_sizes) {
        auto pages = sliding_window_scan(start_offset, end_offset, page_size, config);
        for (const auto& page : pages) {
            callback(page);
            pages_found++;
        }
    }

    return pages_found;
}

uint32_t AdvancedPageCarver::carve_by_type(
    uint16_t page_type,
    const CarvingConfig& config,
    std::function<void(const CarvedPage&)> callback) {

    if (!engine_.is_open() || !callback) {
        return 0;
    }

    uint32_t pages_found = 0;

    auto original_callback = callback;
    callback = [page_type, original_callback](const CarvedPage& page) {
        if (page.page_type == page_type) {
            original_callback(page);
        }
    };

    return carve_pages(config, callback);
}

std::vector<CarvedFile> AdvancedPageCarver::aggregate_to_files(
    const std::vector<CarvedPage>& pages,
    const CarvingConfig& config) {

    // Group by (space_id, page_size)
    std::map<std::pair<uint32_t, uint32_t>, std::vector<const CarvedPage*>> groups;

    for (const auto& page : pages) {
        groups[{page.space_id, page.page_size}].push_back(&page);
    }

    std::vector<CarvedFile> files;

    for (const auto& [key, page_list] : groups) {
        if (page_list.size() < config.min_pages_per_file) {
            continue;
        }

        CarvedFile file;
        file.space_id = key.first;
        file.page_size = key.second;
        file.num_pages = page_list.size();
        file.confidence = 0.0;
        file.detected_version = MySQLVersion::MYSQL_56;

        // Sort by page number
        std::vector<const CarvedPage*> sorted_pages = page_list;
        std::sort(sorted_pages.begin(), sorted_pages.end(),
                  [](const CarvedPage* a, const CarvedPage* b) {
                      return a->page_number < b->page_number;
                  });

        // Find disk offset (minimum)
        file.disk_offset = sorted_pages.front()->disk_offset;

        // Collect page numbers
        for (const auto* page : sorted_pages) {
            file.page_numbers.push_back(page->page_number);
            file.confidence += page->confidence;

            // Track index info
            if (page->page_type == InnoDBSignatureLibrary::FIL_PAGE_INDEX) {
                if (file.index_id == 0) {
                    file.index_id = page->index_id;
                }
                if (page->is_leaf) {
                    file.leaf_pages++;
                } else {
                    file.non_leaf_pages++;
                }
            }

            if (page->page_type == InnoDBSignatureLibrary::FIL_PAGE_SDI) {
                file.has_sdi = true;
                file.detected_version = MySQLVersion::MYSQL_80;
            }
        }

        file.confidence /= page_list.size();

        // Check if complete (all pages in sequence)
        uint32_t expected_pages = sorted_pages.back()->page_number -
                                  sorted_pages.front()->page_number + 1;
        file.is_complete = (file.page_numbers.size() == expected_pages);

        // Generate suggested filename
        file.suggested_filename = "recovered_space_" + std::to_string(file.space_id) + ".ibd";

        files.push_back(file);
    }

    return files;
}

std::vector<CarvedFile> AdvancedPageCarver::aggregate_by_index(
    const std::vector<CarvedPage>& pages) {

    // Group INDEX pages by index_id
    std::map<uint64_t, std::vector<const CarvedPage*>> index_groups;

    for (const auto& page : pages) {
        if (page.page_type == InnoDBSignatureLibrary::FIL_PAGE_INDEX) {
            index_groups[page.index_id].push_back(&page);
        }
    }

    std::vector<CarvedFile> files;

    for (const auto& [index_id, page_list] : index_groups) {
        CarvedFile file;
        file.index_id = index_id;
        file.page_size = page_list.front()->page_size;
        file.space_id = page_list.front()->space_id;
        file.num_pages = page_list.size();
        file.confidence = 0.0;

        for (const auto* page : page_list) {
            file.page_numbers.push_back(page->page_number);
            file.confidence += page->confidence;

            if (page->is_leaf) {
                file.leaf_pages++;
            } else {
                file.non_leaf_pages++;
                // Root page has highest level
                if (page->page_level > 0) {
                    file.root_page = page->page_number;
                }
            }
        }

        file.confidence /= page_list.size();
        file.suggested_filename = "recovered_index_" + std::to_string(index_id) + ".ibd";

        files.push_back(file);
    }

    return files;
}

bool AdvancedPageCarver::validate_checksum(
    const uint8_t* page_data, uint32_t page_size, ChecksumAlgo& algo) {

    if (page_size < 8) {
        algo = ChecksumAlgo::NONE;
        return false;
    }

    // Get stored checksums
    uint32_t checksum1 = (page_data[0] << 24) | (page_data[1] << 16) |
                          (page_data[2] << 8) | page_data[3];
    uint32_t checksum2 = (page_data[page_size - 8] << 24) | (page_data[page_size - 7] << 16) |
                          (page_data[page_size - 6] << 8) | page_data[page_size - 5];

    // Both checksums should match
    if (checksum1 != checksum2) {
        algo = ChecksumAlgo::UNKNOWN;
        return false;
    }

    // Try CRC32C first (MySQL 5.7+ default)
    uint32_t crc32c_checksum = compute_crc32c_checksum(page_data, page_size);
    if (checksum1 == crc32c_checksum) {
        algo = ChecksumAlgo::CRC32C;
        return true;
    }

    // Try INNODB checksum (MySQL 5.6 default)
    uint32_t innodb_checksum = compute_innodb_checksum(page_data, page_size);
    if (checksum1 == innodb_checksum) {
        algo = ChecksumAlgo::INNODB;
        return true;
    }

    algo = ChecksumAlgo::UNKNOWN;
    return false;
}

uint32_t AdvancedPageCarver::compute_crc32c_checksum(
    const uint8_t* page_data, uint32_t page_size) {

    // CRC32C checksum: checksum of FIL header (4-26) + page data (38 to page_size-8)
    uint32_t c1 = crc32c_extend(0, page_data + 4, 22);  // FIL header
    uint32_t c2 = crc32c_extend(0, page_data + 38, page_size - 38 - 8);  // Page data

    return c1 ^ c2;
}

uint32_t AdvancedPageCarver::compute_innodb_checksum(
    const uint8_t* page_data, uint32_t page_size) {

    // InnoDB legacy checksum (ut_fold_binary)
    // This is a simplified implementation
    uint32_t sum = 0;

    // Checksum of first 4 bytes (checksum field itself)
    // and bytes 4 to page_size-8

    for (uint32_t i = 4; i < page_size - 8; ++i) {
        sum += page_data[i];
        sum = (sum << 1) | (sum >> 31);  // Rotate left
    }

    return sum;
}

std::vector<CarvedPage> AdvancedPageCarver::sliding_window_scan(
    uint64_t start, uint64_t end, uint32_t page_size, const CarvingConfig& config) {

    std::vector<CarvedPage> pages;

    // Align to page boundaries
    start = (start / page_size) * page_size;
    if (end % page_size != 0) {
        end = ((end / page_size) + 1) * page_size;
    }

    // Use streaming read with callback
    engine_.stream_read(start, end,
        [&](uint64_t offset, const uint8_t* data, size_t size) {
            // Check each page-aligned offset in this chunk
            for (size_t i = 0; i + page_size <= size; i += page_size) {
                auto page = check_page_at_offset(offset + i, page_size, config);
                if (page.has_value()) {
                    pages.push_back(*page);
                }
            }
        },
        page_size * 64);  // Read 64 pages at a time

    return pages;
}

std::optional<CarvedPage> AdvancedPageCarver::check_page_at_offset(
    uint64_t offset, uint32_t page_size, const CarvingConfig& config) {

    // Read page data
    std::vector<uint8_t> buffer(page_size);
    size_t bytes_read = engine_.read(offset, buffer.data(), page_size);

    if (bytes_read < page_size) {
        return std::nullopt;
    }

    // Detect page type
    uint16_t page_type = InnoDBSignatureLibrary::detect_page_type(buffer.data(), page_size);

    if (!InnoDBSignatureLibrary::is_valid_page_type(page_type)) {
        return std::nullopt;
    }

    // Check confidence
    auto sig = InnoDBSignatureLibrary::get_signature(page_type, config.target_version);
    double confidence = 0.0;

    if (sig.has_value()) {
        confidence = InnoDBSignatureLibrary::match_signature(
            buffer.data(), page_size, *sig);
    } else {
        confidence = 0.5;  // Unknown type, low confidence
    }

    if (confidence < config.signature_threshold) {
        return std::nullopt;
    }

    // Create carved page
    CarvedPage page;
    page.disk_offset = offset;
    page.page_size = page_size;
    page.page_type = page_type;
    page.confidence = confidence;
    page.data = std::move(buffer);

    // Parse headers
    auto fil_header = InnoDBSignatureLibrary::parse_fil_header(page.data.data(), page_size);
    if (fil_header.has_value()) {
        page.page_number = fil_header->page_number;
        page.space_id = fil_header->space_id;
        page.lsn = fil_header->lsn;
    }

    auto page_header = InnoDBSignatureLibrary::parse_page_header(page.data.data(), page_size);
    if (page_header.has_value()) {
        page.index_id = page_header->index_id;
        page.page_level = page_header->level;
        page.is_leaf = (page_header->level == 0);
    }

    // Validate checksum
    if (config.validate_checksums) {
        page.checksum_valid = validate_checksum(
            page.data.data(), page_size, page.checksum_algorithm);

        if (!page.checksum_valid && config.strict_checksum) {
            return std::nullopt;
        }
    }

    // Extract deleted records
    if (config.extract_deleted_records && page.page_type == InnoDBSignatureLibrary::FIL_PAGE_INDEX) {
        page.deleted_record_offsets = extract_deleted_offsets(page.data.data(), page_size);
        page.is_deleted = !page.deleted_record_offsets.empty();
    }

    return page;
}

std::vector<uint16_t> AdvancedPageCarver::extract_deleted_offsets(
    const uint8_t* page_data, uint32_t page_size) {

    std::vector<uint16_t> offsets;

    // Free list head is at PAGE_HEADER + 6
    if (page_size < InnoDBSignatureLibrary::PAGE_HEADER_OFFSET + 8) {
        return offsets;
    }

    const uint8_t* ph = page_data + InnoDBSignatureLibrary::PAGE_HEADER_OFFSET;
    uint16_t free_head = (ph[6] << 8) | ph[7];

    // Follow free list (max 100 entries to prevent infinite loop)
    uint16_t current = free_head;
    int count = 0;

    while (current != 0 && current < page_size && count < 100) {
        offsets.push_back(current);

        // Next pointer is at current + 2 (after record header)
        if (current + 4 > page_size) break;

        current = (page_data[current + 2] << 8) | page_data[current + 3];
        count++;
    }

    return offsets;
}

double AdvancedPageCarver::calculate_confidence(
    const uint8_t* page_data, uint32_t page_size, uint16_t page_type) {

    double confidence = 0.5;

    // Check for valid INFIMUM/SUPREMUM
    if (page_type == InnoDBSignatureLibrary::FIL_PAGE_INDEX) {
        if (page_size >= 120) {
            if (page_data[InnoDBSignatureLibrary::INFIMUM_OFFSET] != 0) {
                confidence += 0.2;
            }
            if (page_data[InnoDBSignatureLibrary::SUPREMUM_OFFSET] != 0) {
                confidence += 0.2;
            }
        }
    }

    // Check for reasonable LSN
    auto fil_header = InnoDBSignatureLibrary::parse_fil_header(page_data, page_size);
    if (fil_header.has_value()) {
        if (fil_header->lsn > 1000 && fil_header->lsn < 0x1000000000000ULL) {
            confidence += 0.1;
        }
    }

    return std::min(confidence, 1.0);
}

// ==================== ProgressPageScanner Implementation ====================

ProgressPageScanner::ProgressPageScanner(AdvancedPageCarver& carver)
    : carver_(carver) {}

void ProgressPageScanner::scan_with_progress(
    const CarvingConfig& config,
    std::function<void(const CarvedPage&)> page_callback,
    std::function<void(double, const AdvancedPageCarver::CarvingStats&)> progress_callback) {

    if (!progress_callback) {
        carver_.carve_pages(config, page_callback);
        return;
    }

    std::atomic<uint64_t> pages_found{0};

    auto wrapped_callback = [&](const CarvedPage& page) {
        page_callback(page);
        pages_found++;
    };

    // Run carving
    carver_.carve_pages(config, wrapped_callback);

    // Final progress
    progress_callback(1.0, carver_.get_stats());
}

} // namespace engine
} // namespace dbrecovery
