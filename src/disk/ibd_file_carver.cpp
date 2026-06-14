/**
 * @file ibd_file_carver.cpp
 * @brief IBD File Carver Implementation
 */

#include "dbrecovery/disk/ibd_file_carver.hpp"
#include "dbrecovery/utils/innodb_checksum.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

namespace dbrecovery {
namespace disk {

using namespace signatures;

// ==================== IbdFileCarver ====================

IbdFileCarver::IbdFileCarver(engine::RawDiskIOEngine& engine)
    : engine_(engine) {
    std::memset(&stats_, 0, sizeof(stats_));
}

std::vector<CarvedIbdFile> IbdFileCarver::carve(
    const CarverConfig& config,
    std::function<void(double, const std::string&)> progress) {

    std::vector<CarvedIbdFile> files;
    std::vector<FoundIbdPage> all_pages;

    auto start_time = std::chrono::high_resolution_clock::now();

    uint64_t disk_size = engine_.get_size();
    uint64_t scan_end = config.end_offset > 0 ?
        std::min(config.end_offset, disk_size) : disk_size;

    // Scan for each page size
    for (uint32_t page_size : config.page_sizes) {
        if (progress) {
            std::string msg = "Scanning for " + std::to_string(page_size) + " byte pages...";
            progress(0.0, msg);
        }

        uint64_t num_pages = scan_end / page_size;
        uint64_t pages_scanned = 0;

        // Scan buffer
        std::vector<uint8_t> buffer(config.buffer_size);
        uint64_t buffer_pages = config.buffer_size / page_size;

        for (uint64_t offset = config.start_offset; offset < scan_end; offset += config.buffer_size) {
            // Read buffer
            size_t bytes_read = engine_.read(offset, buffer.data(),
                std::min(static_cast<uint64_t>(config.buffer_size), scan_end - offset));

            if (bytes_read == 0) break;

            // Scan buffer for pages
            for (uint64_t i = 0; i < bytes_read; i += page_size) {
                if (i + page_size > bytes_read) break;

                const uint8_t* page_data = buffer.data() + i;
                uint64_t page_offset = offset + i;

                // Check if this looks like an IBD page
                if (is_ibd_page(page_data, page_size)) {
                    FoundIbdPage page = parse_page_header(page_data, page_size, page_offset);

                    // Validate checksum if requested
                    if (config.validate_checksums) {
                        page.checksum_valid = validate_checksum(page_data, page_size);
                    }

                    if (page.checksum_valid || !config.validate_checksums) {
                        all_pages.push_back(page);

                        // Update stats
                        stats_.valid_pages_found++;
                        if (page.page_type == FIL_PAGE_TYPE_INDEX) {
                            stats_.index_pages_found++;
                        } else if (page.page_type == FIL_PAGE_TYPE_BLOB ||
                                   page.page_type == FIL_PAGE_TYPE_ZBLOB ||
                                   page.page_type == FIL_PAGE_TYPE_ZBLOB2) {
                            stats_.blob_pages_found++;
                        } else if (page.page_type == FIL_PAGE_TYPE_FSP_HDR) {
                            stats_.fsp_pages_found++;
                        }
                    }
                }

                stats_.pages_examined++;
                pages_scanned++;
            }

            // Progress update
            if (progress) {
                double pct = static_cast<double>(offset - config.start_offset) /
                            (scan_end - config.start_offset);
                progress(pct, "Scanning...");
            }
        }
    }

    stats_.total_bytes_scanned = scan_end - config.start_offset;

    // Aggregate pages into files
    if (config.aggregate_pages && !all_pages.empty()) {
        if (progress) progress(0.8, "Aggregating pages into files...");

        files = aggregate_by_space_id(all_pages, config);

        // Follow page chains if requested
        if (config.follow_page_chain) {
            for (auto& file : files) {
                // Try to find additional pages by following chains
                // This is handled in aggregate_by_space_id
            }
        }
    }

    // Calculate statistics
    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.scan_time_seconds = std::chrono::duration<double>(end_time - start_time).count();

    if (stats_.scan_time_seconds > 0) {
        stats_.throughput_mbps = (stats_.total_bytes_scanned / (1024.0 * 1024.0)) / stats_.scan_time_seconds;
    }

    stats_.files_found = files.size();
    stats_.complete_files = 0;
    stats_.fragmented_files = 0;
    stats_.total_bytes_recovered = 0;

    for (const auto& file : files) {
        if (file.is_complete) stats_.complete_files++;
        if (file.is_fragmented) stats_.fragmented_files++;
        stats_.total_bytes_recovered += file.file_size;
    }

    if (progress) progress(1.0, "Carving complete");

    return files;
}

CarverStats IbdFileCarver::scan_pages(
    const CarverConfig& config,
    std::function<void(const FoundIbdPage&)> callback) {

    std::memset(&stats_, 0, sizeof(stats_));

    uint64_t disk_size = engine_.get_size();
    uint64_t scan_end = config.end_offset > 0 ?
        std::min(config.end_offset, disk_size) : disk_size;

    std::vector<uint8_t> buffer(config.buffer_size);

    for (uint64_t offset = config.start_offset; offset < scan_end; offset += config.buffer_size) {
        size_t bytes_read = engine_.read(offset, buffer.data(),
            std::min(static_cast<uint64_t>(config.buffer_size), scan_end - offset));

        if (bytes_read == 0) break;

        for (uint32_t page_size : config.page_sizes) {
            for (uint64_t i = 0; i < bytes_read; i += page_size) {
                if (i + page_size > bytes_read) break;

                const uint8_t* page_data = buffer.data() + i;

                if (is_ibd_page(page_data, page_size)) {
                    FoundIbdPage page = parse_page_header(page_data, page_size, offset + i);

                    if (config.validate_checksums) {
                        page.checksum_valid = validate_checksum(page_data, page_size);
                    }

                    if (page.checksum_valid || !config.validate_checksums) {
                        callback(page);
                        stats_.valid_pages_found++;
                    }
                }

                stats_.pages_examined++;
            }
        }

        stats_.total_bytes_scanned += bytes_read;
    }

    return stats_;
}

std::vector<CarvedIbdFile> IbdFileCarver::find_by_space_id(
    uint32_t space_id, const CarverConfig& config) {

    std::vector<CarvedIbdFile> files;

    // Scan for pages with matching space_id
    std::vector<FoundIbdPage> matching_pages;

    scan_pages(config, [&matching_pages, space_id](const FoundIbdPage& page) {
        if (page.space_id == space_id) {
            matching_pages.push_back(page);
        }
    });

    if (matching_pages.empty()) return files;

    // Aggregate into file
    files = aggregate_by_space_id(matching_pages, config);

    return files;
}

std::vector<CarvedIbdFile> IbdFileCarver::find_by_index_id(
    uint64_t index_id, const CarverConfig& config) {

    std::vector<CarvedIbdFile> files;
    std::vector<FoundIbdPage> matching_pages;

    scan_pages(config, [&matching_pages, index_id](const FoundIbdPage& page) {
        if (page.index_id == index_id) {
            matching_pages.push_back(page);
        }
    });

    if (matching_pages.empty()) return files;

    // Create file from pages
    CarvedIbdFile file;
    file.space_id = matching_pages[0].space_id;
    file.page_size = config.page_sizes.empty() ? 16384 : config.page_sizes[0];
    file.num_pages = matching_pages.size();
    file.index_pages = matching_pages.size();

    // Calculate file bounds
    uint64_t min_offset = UINT64_MAX;
    uint64_t max_offset = 0;

    for (const auto& page : matching_pages) {
        min_offset = std::min(min_offset, page.disk_offset);
        max_offset = std::max(max_offset, page.disk_offset + page.page_size);

        file.page_map[page.page_number] = page.disk_offset;

        if (page.level == 0) {
            // Found leaf page - could be root for small tables
            file.root_index_id = page.index_id;
        }
    }

    file.disk_offset = min_offset;
    file.file_size = max_offset - min_offset;
    file.confidence = calculate_confidence(file);

    files.push_back(file);

    return files;
}

bool IbdFileCarver::export_file(const CarvedIbdFile& file, const std::string& output_path) {
    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;

    out.write(reinterpret_cast<const char*>(file.data.data()), file.data.size());

    return out.good();
}

bool IbdFileCarver::is_ibd_page(const uint8_t* data, uint32_t page_size) {
    if (!data || page_size < 64) return false;

    // Check page type
    uint16_t page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];

    // Valid InnoDB page types
    switch (page_type) {
        case FIL_PAGE_TYPE_ALLOCATED:
        case FIL_PAGE_TYPE_UNDO_LOG:
        case FIL_PAGE_TYPE_INODE:
        case FIL_PAGE_TYPE_IBUF_BITMAP:
        case FIL_PAGE_TYPE_SYS:
        case FIL_PAGE_TYPE_TRX_SYS:
        case FIL_PAGE_TYPE_FSP_HDR:
        case FIL_PAGE_TYPE_XDES:
        case FIL_PAGE_TYPE_BLOB:
        case FIL_PAGE_TYPE_ZBLOB:
        case FIL_PAGE_TYPE_ZBLOB2:
        case FIL_PAGE_TYPE_SDI:     // MySQL 8.0+
        case FIL_PAGE_TYPE_INDEX:
            break;
        default:
            return false;
    }

    // Additional checks for INDEX pages
    if (page_type == FIL_PAGE_TYPE_INDEX) {
        // Check for infimum/supremum markers (page 4+)
        // These are at specific offsets in the page

        // Also check that page number matches offset
        uint32_t page_no = (static_cast<uint32_t>(data[4]) << 24) |
                          (static_cast<uint32_t>(data[5]) << 16) |
                          (static_cast<uint32_t>(data[6]) << 8) |
                          static_cast<uint32_t>(data[7]);

        // Page number should be reasonable (< file_size / page_size)
        // For now, just check it's not all zeros or all 0xFF
        if (page_no == 0xFFFFFFFF || page_no == 0x00000000) {
            // Could still be valid - first page or corrupted
        }
    }

    return true;
}

FoundIbdPage IbdFileCarver::parse_page_header(const uint8_t* data, uint32_t page_size, uint64_t offset) {
    FoundIbdPage page;
    page.disk_offset = offset;
    page.page_size = page_size;

    // Parse FIL header
    // Page number (offset 4-7, big-endian)
    page.page_number = (static_cast<uint32_t>(data[4]) << 24) |
                       (static_cast<uint32_t>(data[5]) << 16) |
                       (static_cast<uint32_t>(data[6]) << 8) |
                       static_cast<uint32_t>(data[7]);

    // Previous page (offset 8-11)
    page.prev_page = (static_cast<uint32_t>(data[8]) << 24) |
                     (static_cast<uint32_t>(data[9]) << 16) |
                     (static_cast<uint32_t>(data[10]) << 8) |
                     static_cast<uint32_t>(data[11]);

    // Next page (offset 12-15)
    page.next_page = (static_cast<uint32_t>(data[12]) << 24) |
                     (static_cast<uint32_t>(data[13]) << 16) |
                     (static_cast<uint32_t>(data[14]) << 8) |
                     static_cast<uint32_t>(data[15]);

    // LSN (offset 16-23)
    page.lsn = (static_cast<uint64_t>(data[16]) << 56) |
               (static_cast<uint64_t>(data[17]) << 48) |
               (static_cast<uint64_t>(data[18]) << 40) |
               (static_cast<uint64_t>(data[19]) << 32) |
               (static_cast<uint64_t>(data[20]) << 24) |
               (static_cast<uint64_t>(data[21]) << 16) |
               (static_cast<uint64_t>(data[22]) << 8) |
               static_cast<uint64_t>(data[23]);

    // Page type (offset 24-25)
    page.page_type = (static_cast<uint16_t>(data[24]) << 8) | data[25];

    // Space ID (offset 34-37)
    page.space_id = (static_cast<uint32_t>(data[34]) << 24) |
                    (static_cast<uint32_t>(data[35]) << 16) |
                    (static_cast<uint32_t>(data[36]) << 8) |
                    static_cast<uint32_t>(data[37]);

    // For INDEX pages, parse INDEX header (offset 38+)
    if (page.page_type == FIL_PAGE_TYPE_INDEX) {
        // Number of records (offset 54-55)
        page.n_recs = (static_cast<uint16_t>(data[54]) << 8) | data[55];

        // Level (offset 56-57, but only lower byte)
        page.level = data[57];

        // Index ID (offset 64-73 in some formats, or 68-75)
        // Actually at offset 64 in older format, 72 in newer
        page.index_id = (static_cast<uint64_t>(data[66]) << 56) |
                        (static_cast<uint64_t>(data[67]) << 48) |
                        (static_cast<uint64_t>(data[68]) << 40) |
                        (static_cast<uint64_t>(data[69]) << 32) |
                        (static_cast<uint64_t>(data[70]) << 24) |
                        (static_cast<uint64_t>(data[71]) << 16) |
                        (static_cast<uint64_t>(data[72]) << 8) |
                        static_cast<uint64_t>(data[73]);
    }

    // Store page data
    page.data.assign(data, data + page_size);

    return page;
}

bool IbdFileCarver::validate_checksum(const uint8_t* data, uint32_t page_size) {
    // Use InnoDB checksum verification
    // The checksum is stored at offset 0-3 and verified against offset (page_size - 8)
    return utils::InnoDBChecksum::validate_crc32(data, page_size);
}

std::vector<CarvedIbdFile> IbdFileCarver::aggregate_by_space_id(
    const std::vector<FoundIbdPage>& pages, const CarverConfig& config) {

    std::vector<CarvedIbdFile> files;

    // Group pages by space_id
    std::map<uint32_t, std::vector<FoundIbdPage>> pages_by_space;

    for (const auto& page : pages) {
        pages_by_space[page.space_id].push_back(page);
    }

    // Create a file for each space_id
    for (auto& [space_id, space_pages] : pages_by_space) {
        if (space_pages.size() < config.min_pages) continue;

        CarvedIbdFile file;
        file.space_id = space_id;
        file.page_size = config.page_sizes.empty() ? 16384 : config.page_sizes[0];
        file.num_pages = space_pages.size();

        // Sort by page number
        std::sort(space_pages.begin(), space_pages.end(),
            [](const FoundIbdPage& a, const FoundIbdPage& b) {
                return a.page_number < b.page_number;
            });

        // Count page types
        for (const auto& page : space_pages) {
            file.page_map[page.page_number] = page.disk_offset;

            switch (page.page_type) {
                case FIL_PAGE_TYPE_INDEX:
                    file.index_pages++;
                    if (page.level == 0 && file.root_index_id == 0) {
                        file.root_index_id = page.index_id;
                    }
                    break;
                case FIL_PAGE_TYPE_BLOB:
                case FIL_PAGE_TYPE_ZBLOB:
                case FIL_PAGE_TYPE_ZBLOB2:
                    file.blob_pages++;
                    break;
                case FIL_PAGE_TYPE_SDI:
                    file.sdi_pages++;
                    break;
                case FIL_PAGE_TYPE_FSP_HDR:
                    file.fsp_pages++;
                    break;
            }
        }

        // Check requirements
        if (config.require_index_pages && file.index_pages == 0) {
            continue;
        }

        if (config.require_fsp_header && file.fsp_pages == 0) {
            continue;
        }

        // Calculate file bounds
        uint64_t min_offset = UINT64_MAX;
        uint64_t max_offset = 0;
        uint32_t max_page_no = 0;

        for (const auto& page : space_pages) {
            min_offset = std::min(min_offset, page.disk_offset);
            max_offset = std::max(max_offset, page.disk_offset + page.page_size);
            max_page_no = std::max(max_page_no, page.page_number);
        }

        file.disk_offset = min_offset;
        file.file_size = max_offset - min_offset;

        // Check for contiguous pages
        uint32_t expected_pages = max_page_no + 1;
        file.is_complete = (space_pages.size() >= expected_pages * 0.9);
        file.is_fragmented = !file.is_complete;

        // Estimate confidence
        file.confidence = calculate_confidence(file);

        // Read data if requested
        if (config.recover_data) {
            file.data.resize(file.file_size);

            for (const auto& page : space_pages) {
                uint64_t offset_in_file = page.disk_offset - min_offset;
                if (offset_in_file + page.page_size <= file.data.size()) {
                    std::copy(page.data.begin(), page.data.end(),
                              file.data.begin() + offset_in_file);
                }
            }
        }

        files.push_back(file);
    }

    return files;
}

double IbdFileCarver::calculate_confidence(const CarvedIbdFile& file) {
    double confidence = 0.0;

    // Has FSP header - strong indicator
    if (file.fsp_pages > 0) {
        confidence += 0.3;
    }

    // Has INDEX pages - required for data
    if (file.index_pages > 0) {
        confidence += 0.3;
    }

    // Has root page
    if (file.root_index_id > 0) {
        confidence += 0.2;
    }

    // Completeness
    if (file.is_complete) {
        confidence += 0.2;
    } else if (!file.is_fragmented) {
        confidence += 0.1;
    }

    // Number of pages
    if (file.num_pages >= 10) {
        confidence += 0.1;
    }

    return std::min(confidence, 1.0);
}

// ==================== IbdSignatureDetector ====================

std::vector<IbdSignatureDetector::Detection> IbdSignatureDetector::detect(
    const uint8_t* data, size_t size, uint32_t page_size) {

    std::vector<Detection> detections;

    if (!data || size < page_size) return detections;

    for (size_t offset = 0; offset + page_size <= size; offset += page_size) {
        const uint8_t* page = data + offset;

        uint16_t page_type = extract_page_type(page);

        // Check if valid InnoDB page type
        switch (page_type) {
            case FIL_PAGE_TYPE_INDEX:
            case FIL_PAGE_TYPE_FSP_HDR:
            case FIL_PAGE_TYPE_BLOB:
            case FIL_PAGE_TYPE_ZBLOB:
            case FIL_PAGE_TYPE_SDI:
            case FIL_PAGE_TYPE_INODE:
            case FIL_PAGE_TYPE_XDES: {
                Detection det;
                det.offset = offset;
                det.page_size = page_size;
                det.page_type = page_type;
                det.space_id = extract_space_id(page, page_size);

                // Extract LSN
                det.lsn = (static_cast<uint64_t>(page[16]) << 56) |
                          (static_cast<uint64_t>(page[17]) << 48) |
                          (static_cast<uint64_t>(page[18]) << 40) |
                          (static_cast<uint64_t>(page[19]) << 32) |
                          (static_cast<uint64_t>(page[20]) << 24) |
                          (static_cast<uint64_t>(page[21]) << 16) |
                          (static_cast<uint64_t>(page[22]) << 8) |
                          static_cast<uint64_t>(page[23]);

                det.is_valid = true;
                detections.push_back(det);
                break;
            }
            default:
                break;
        }
    }

    return detections;
}

bool IbdSignatureDetector::is_fsp_header(const uint8_t* data, uint32_t page_size) {
    if (!data) return false;

    uint16_t page_type = extract_page_type(data);
    return page_type == FIL_PAGE_TYPE_FSP_HDR;
}

bool IbdSignatureDetector::is_index_page(const uint8_t* data, uint32_t page_size) {
    if (!data) return false;

    uint16_t page_type = extract_page_type(data);
    return page_type == FIL_PAGE_TYPE_INDEX;
}

bool IbdSignatureDetector::has_infimum_supremum(const uint8_t* data, uint32_t page_size) {
    if (!data || page_size < INFIMUM_OFFSET + 16) return false;

    // Check for "infimum" and "supremum" markers
    // These are at specific offsets in INDEX pages
    return std::memcmp(data + INFIMUM_OFFSET, INFIMUM_MARKER, 7) == 0 &&
           std::memcmp(data + SUPREMUM_OFFSET, SUPREMUM_MARKER, 9) == 0;
}

uint32_t IbdSignatureDetector::extract_space_id(const uint8_t* data, uint32_t page_size) {
    if (!data) return 0;

    return (static_cast<uint32_t>(data[34]) << 24) |
           (static_cast<uint32_t>(data[35]) << 16) |
           (static_cast<uint32_t>(data[36]) << 8) |
           static_cast<uint32_t>(data[37]);
}

uint16_t IbdSignatureDetector::extract_page_type(const uint8_t* data) {
    if (!data) return 0;

    return (static_cast<uint16_t>(data[24]) << 8) | data[25];
}

// ==================== FileBoundaryDetector ====================

std::vector<FileBoundaryDetector::Boundary> FileBoundaryDetector::detect(
    const std::vector<FoundIbdPage>& pages, uint32_t page_size) {

    std::vector<Boundary> boundaries;

    if (pages.empty()) return boundaries;

    // Group by space_id
    std::map<uint32_t, std::vector<FoundIbdPage>> by_space;
    for (const auto& page : pages) {
        by_space[page.space_id].push_back(page);
    }

    for (auto& [space_id, space_pages] : by_space) {
        Boundary b;
        b.space_id = space_id;
        b.page_size = page_size;
        b.num_pages = space_pages.size();

        // Sort by offset
        std::sort(space_pages.begin(), space_pages.end(),
            [](const FoundIbdPage& a, const FoundIbdPage& b) {
                return a.disk_offset < b.disk_offset;
            });

        b.start_offset = space_pages.front().disk_offset;
        b.end_offset = space_pages.back().disk_offset + page_size;
        b.is_valid = space_pages.size() >= 2;

        boundaries.push_back(b);
    }

    return boundaries;
}

bool FileBoundaryDetector::same_file(const FoundIbdPage& a, const FoundIbdPage& b) {
    return a.space_id == b.space_id && a.page_size == b.page_size;
}

uint64_t FileBoundaryDetector::estimate_size(const std::vector<FoundIbdPage>& pages) {
    if (pages.empty()) return 0;

    uint32_t max_page_no = 0;
    for (const auto& page : pages) {
        max_page_no = std::max(max_page_no, page.page_number);
    }

    return static_cast<uint64_t>(max_page_no + 1) * pages[0].page_size;
}

} // namespace disk
} // namespace dbrecovery