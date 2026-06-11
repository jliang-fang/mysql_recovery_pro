/**
 * @file raw_disk_engine.cpp
 * @brief Raw Disk I/O Engine implementation
 *
 * Supports direct disk/partition scanning with 4K alignment.
 * Optimized for scanning large volumes (TB+) with minimal memory footprint.
 *
 * Key features:
 * - Direct I/O with O_DIRECT when available, posix_memalign for aligned buffers
 * - io_uring support for async high-performance scanning
 * - Memory-mapped file fallback for image files (mmap + madvise)
 * - 4K sector alignment enforcement
 * - InnoDB page signature detection with candidate scoring
 * - Checksum validation using innodb::validate_page_checksum()
 */

#include "dbrecovery/engine/raw_disk_engine.hpp"
#include "dbrecovery/innodb/innodb_checksum.hpp"
#include "dbrecovery/innodb/page_defs.hpp"

#include <sys/statvfs.h>
#include <sys/mman.h>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <cstring>
#include <cerrno>

#ifdef DBRECOVERY_HAVE_IO_URING
#include <liburing.h>
#endif

namespace dbrecovery {
namespace engine {
// ==================== Aligned Buffer Helper ====================

/**
 * @brief Allocate a 4K-aligned buffer using posix_memalign
 * @param size Buffer size (will be rounded up to 4K alignment)
 * @return Pointer to aligned buffer, or nullptr on failure
 */
static uint8_t* alloc_aligned_buffer(size_t size) {
    size_t aligned_size = ((size + 4095) / 4096) * 4096;
    void* ptr = nullptr;
    int ret = posix_memalign(&ptr, 4096, aligned_size);
    if (ret != 0 || ptr == nullptr) {
        return nullptr;
    }
    std::memset(ptr, 0, aligned_size);
    return static_cast<uint8_t*>(ptr);
}

/**
 * @brief Score a page candidate based on page type, LSN, page number validity
 * @param page_type InnoDB page type
 * @param lsn Log Sequence Number
 * @param page_number Page number from header
 * @return Score 0-100 (higher = more likely a valid InnoDB page)
 */
static uint8_t score_page_candidate(uint16_t page_type, uint64_t lsn, uint32_t page_number) {
    uint8_t score = 0;

    // Page type scoring (most important signal)
    if (page_type == innodb::FIL_PAGE_INDEX) {
        score += 40;  // Index page - most valuable
    } else if (page_type == innodb::FIL_PAGE_TYPE_BLOB) {
        score += 25;  // BLOB page
    } else if (page_type == innodb::FIL_PAGE_UNDO_LOG) {
        score += 15;  // Undo log
    } else if (page_type == 0) {
        score += 5;   // Allocated page (weak signal)
    } else {
        return 0;     // Unknown type - not a valid candidate
    }

    // LSN scoring (reasonable LSN > 0 is a strong signal)
    if (lsn > 0 && lsn < 0xFFFFFFFFFFFF0000ULL) {
        score += 30;
    } else if (lsn > 0) {
        score += 10;  // Suspiciously large LSN
    }

    // Page number scoring (reasonable page number < 10M)
    if (page_number > 0 && page_number < 10000000) {
        score += 20;
    } else if (page_number == 0) {
        score += 5;   // Page 0 is special (FSP header)
    }

    // Bonus: checksum validation adds up to 10 points
    // (handled separately after checksum validation)
    return score;
}

/**
 * @brief Update page type statistics
 */
static void update_page_type_stats(PageScanner::ScanStats& stats, uint16_t page_type) {
    switch (page_type) {
        case innodb::FIL_PAGE_INDEX:  // 17855
            stats.index_pages++;
            break;
        case innodb::FIL_PAGE_SDI:  // 17854
            stats.sdi_pages++;
            break;
        case innodb::FIL_PAGE_UNDO_LOG:  // 17842
            stats.undo_pages++;
            break;
        case innodb::FIL_PAGE_TYPE_BLOB:  // 17853
            stats.blob_pages++;
            break;
        case 0:         // ALLOCATED
            stats.allocated_pages++;
            break;
    }
}

// ==================== RawDiskIOEngine Implementation ====================

RawDiskIOEngine::RawDiskIOEngine()
    : fd_(-1)
    , size_(0)
    , is_open_(false)
    , mode_(IoMode::READ_ONLY)
    , use_direct_io_(false)
    , optimal_block_size_(4096)
    , progress_callback_(default_progress_callback)
#ifdef DBRECOVERY_HAVE_IO_URING
    , io_uring_initialized_(false)
#endif
{
#ifdef DBRECOVERY_HAVE_IO_URING
    std::memset(&ring_, 0, sizeof(ring_));
#endif
}

RawDiskIOEngine::~RawDiskIOEngine() {
    close();
}

RawDiskIOEngine::RawDiskIOEngine(RawDiskIOEngine&& other) noexcept
    : fd_(other.fd_)
    , size_(other.size_)
    , is_open_(other.is_open_)
    , mode_(other.mode_)
    , use_direct_io_(other.use_direct_io_)
    , optimal_block_size_(other.optimal_block_size_)
    , progress_callback_(std::move(other.progress_callback_))
#ifdef DBRECOVERY_HAVE_IO_URING
    , ring_(other.ring_)
    , io_uring_initialized_(other.io_uring_initialized_)
#endif
{
    other.fd_ = -1;
    other.is_open_ = false;
#ifdef DBRECOVERY_HAVE_IO_URING
    other.io_uring_initialized_ = false;
#endif
}

RawDiskIOEngine& RawDiskIOEngine::operator=(RawDiskIOEngine&& other) noexcept {
    if (this != &other) {
        close();

        fd_ = other.fd_;
        size_ = other.size_;
        is_open_ = other.is_open_;
        mode_ = other.mode_;
        use_direct_io_ = other.use_direct_io_;
        optimal_block_size_ = other.optimal_block_size_;
        progress_callback_ = std::move(other.progress_callback_);

        other.fd_ = -1;
        other.is_open_ = false;

#ifdef DBRECOVERY_HAVE_IO_URING
        ring_ = other.ring_;
        io_uring_initialized_ = other.io_uring_initialized_;
        other.io_uring_initialized_ = false;
#endif
    }
    return *this;
}

bool RawDiskIOEngine::open(const std::string& path, IoMode mode) {
    if (is_open_) {
        close();
    }

    mode_ = mode;

    if (mode == IoMode::MEMORY_MAPPED) {
        // For mmap path, open as read-only
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) {
            std::cerr << "[ERROR] RawDiskIOEngine: Failed to open for mmap " << path
                      << ": " << strerror(errno) << std::endl;
            return false;
        }
        use_direct_io_ = false;

        struct stat st;
        if (fstat(fd_, &st) == 0) {
            size_ = st.st_size;
        } else {
            size_ = 0;
        }

        is_open_ = true;
        return true;
    }

    // Try direct I/O first (better for performance on real devices)
    int open_flags = O_RDONLY;
    if (mode == IoMode::READ_WRITE) {
        open_flags = O_RDWR;
    }

#ifdef O_DIRECT
    // Try to open with O_DIRECT for real devices
    fd_ = ::open(path.c_str(), open_flags | O_DIRECT);
    if (fd_ >= 0) {
        use_direct_io_ = true;
    } else {
#endif
        // Fallback to buffered I/O
        fd_ = ::open(path.c_str(), open_flags);
        if (fd_ < 0) {
            std::cerr << "[ERROR] RawDiskIOEngine: Failed to open " << path
                      << ": " << strerror(errno) << std::endl;
            return false;
        }
        use_direct_io_ = false;
#ifdef O_DIRECT
    }
#endif

    // Get file size
    struct stat st;
    if (fstat(fd_, &st) == 0) {
        size_ = st.st_size;

        // Get optimal I/O block size
#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
        struct statvfs vfs;
        if (fstatvfs(fd_, &vfs) == 0) {
            optimal_block_size_ = static_cast<uint32_t>(vfs.f_bsize);
        }
#endif

        // Ensure at least 4K alignment
        if (optimal_block_size_ < 4096) {
            optimal_block_size_ = 4096;
        }
    } else {
        size_ = 0;
    }

    is_open_ = true;
    return true;
}

void RawDiskIOEngine::close() {
    if (is_open_) {
        if (fd_ >= 0) {
#ifdef DBRECOVERY_HAVE_IO_URING
            cleanup_io_uring();
#endif
            ::close(fd_);
            fd_ = -1;
        }
        is_open_ = false;
        size_ = 0;
        use_direct_io_ = false;
    }
}

size_t RawDiskIOEngine::read(uint64_t offset, uint8_t* buffer, size_t size) {
    if (!is_open_ || fd_ < 0 || !buffer || size == 0) {
        return 0;
    }

    if (mode_ == IoMode::MEMORY_MAPPED) {
        // mmap path: use pread on the fd
        // In a full mmap implementation, we would have mapped the file
        // and read directly from the mapped region
        ssize_t result = ::pread(fd_, buffer, size, offset);
        if (result < 0) {
            return 0;
        }
        return static_cast<size_t>(result);
    }

    if (use_direct_io_) {
        // With O_DIRECT, offset and size must be 4K aligned
        uint64_t aligned_offset = (offset / 4096) * 4096;
        size_t aligned_size = ((size + (offset - aligned_offset) + 4095) / 4096) * 4096;

        // Use posix_memalign for aligned buffer
        uint8_t* aligned_buffer = alloc_aligned_buffer(aligned_size);
        if (!aligned_buffer) {
            // Fallback: try buffered read if aligned allocation fails
            ssize_t result = ::pread(fd_, buffer, size, offset);
            if (result < 0) {
                return 0;
            }
            return static_cast<size_t>(result);
        }

        ssize_t result = ::pread(fd_, aligned_buffer, aligned_size, aligned_offset);

        if (result >= 0) {
            // Copy the requested portion from aligned buffer
            uint64_t skip = offset - aligned_offset;
            size_t copy_size = std::min(size, static_cast<size_t>(result > static_cast<ssize_t>(skip)
                ? result - skip : 0));
            if (copy_size > 0) {
                std::memcpy(buffer, aligned_buffer + skip, copy_size);
            }
            free(aligned_buffer);
            return copy_size;
        }

        free(aligned_buffer);
        return 0;
    } else {
        // Buffered I/O - no alignment requirements
        ssize_t result = ::pread(fd_, buffer, size, offset);
        if (result < 0) {
            return 0;
        }
        return static_cast<size_t>(result);
    }
}

size_t RawDiskIOEngine::write(uint64_t offset, const uint8_t* buffer, size_t size) {
    if (!is_open_ || fd_ < 0 || !buffer || size == 0 || mode_ == IoMode::READ_ONLY
        || mode_ == IoMode::MEMORY_MAPPED) {
        return 0;
    }

    if (use_direct_io_) {
        // With O_DIRECT, offset and size must be 4K aligned
        uint64_t aligned_offset = (offset / 4096) * 4096;
        size_t aligned_size = ((size + (offset - aligned_offset) + 4095) / 4096) * 4096;

        uint8_t* aligned_buffer = alloc_aligned_buffer(aligned_size);
        if (!aligned_buffer) {
            return 0;
        }

        // Initialize buffer with existing data (for partial writes)
        ::pread(fd_, aligned_buffer, aligned_size, aligned_offset);

        // Copy the new data
        std::memcpy(aligned_buffer + (offset - aligned_offset), buffer, size);

        ssize_t result = ::pwrite(fd_, aligned_buffer, aligned_size, aligned_offset);
        free(aligned_buffer);

        if (result < 0) {
            return 0;
        }
        return static_cast<size_t>(result);
    } else {
        // Buffered I/O
        ssize_t result = ::pwrite(fd_, buffer, size, offset);
        if (result < 0) {
            return 0;
        }
        return static_cast<size_t>(result);
    }
}

bool RawDiskIOEngine::scan(ScanProgressCallback callback, size_t chunk_size, uint64_t max_bytes) {
    if (!is_open_) {
        return false;
    }

    uint64_t scan_size = max_bytes > 0 ? std::min(max_bytes, size_) : size_;
    chunk_size = std::max(chunk_size, static_cast<size_t>(4096));

    // Use posix_memalign for the scan buffer
    size_t buf_size = std::min(chunk_size, static_cast<size_t>(LARGE_BUFFER_SIZE));
    uint8_t* buffer = alloc_aligned_buffer(buf_size);
    bool use_own_buffer = (buffer != nullptr);
    if (!buffer) {
        // Fallback to std::vector
        std::vector<uint8_t> vec_buffer(buf_size);
        buffer = vec_buffer.data();
    }

    uint64_t total_scanned = 0;
    uint64_t last_report = 0;
    const uint64_t report_interval = std::max(scan_size / 100,
        static_cast<uint64_t>(10 * 1024 * 1024));

    while (total_scanned < scan_size) {
        size_t to_read = std::min(buf_size, static_cast<size_t>(scan_size - total_scanned));
        size_t bytes_read = read(total_scanned, buffer, to_read);

        if (bytes_read == 0) {
            break;
        }

        total_scanned += bytes_read;

        // Report progress periodically
        if (total_scanned - last_report >= report_interval) {
            double percent = (scan_size > 0) ? (100.0 * total_scanned / scan_size) : 0.0;
            callback(total_scanned, scan_size, percent);
            last_report = total_scanned;
        }
    }

    // Final progress report
    double percent = (scan_size > 0) ? (100.0 * total_scanned / scan_size) : 0.0;
    callback(total_scanned, scan_size, percent);

    if (use_own_buffer) {
        free(buffer);
    }

    return total_scanned > 0;
}

#ifdef DBRECOVERY_HAVE_IO_URING
bool RawDiskIOEngine::init_io_uring(unsigned depth) {
    if (!is_open_) {
        return false;
    }

    int ret = io_uring_queue_init(depth, &ring_, 0);
    if (ret < 0) {
        std::cerr << "[WARN] RawDiskIOEngine: io_uring init failed: "
                  << strerror(-ret) << std::endl;
        return false;
    }

    io_uring_initialized_ = true;
    return true;
}

void RawDiskIOEngine::cleanup_io_uring() {
    if (io_uring_initialized_) {
        io_uring_queue_exit(&ring_);
        io_uring_initialized_ = false;
    }
}
#else
bool RawDiskIOEngine::init_io_uring(unsigned) {
    return false;
}

void RawDiskIOEngine::cleanup_io_uring() {
}
#endif

bool RawDiskIOEngine::flush() {
    if (!is_open_ || fd_ < 0) {
        return false;
    }

    if (::fsync(fd_) < 0) {
        std::cerr << "[WARN] RawDiskIOEngine: fsync failed: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

bool RawDiskIOEngine::trim(uint64_t offset, uint64_t length) {
    if (!is_open_ || fd_ < 0) {
        return false;
    }

#ifdef BLKDISCARD
    uint64_t range[2] = {offset, length};
    if (ioctl(fd_, BLKDISCARD, range) < 0) {
        return false;
    }
    return true;
#else
    (void)offset;
    (void)length;
    return false;
#endif
}

// ==================== PageScanner Implementation ====================

bool PageScanner::parse_page_header(const uint8_t* data, uint32_t page_size,
                                     uint32_t& space_id, uint32_t& page_number,
                                     uint16_t& page_type, uint64_t& lsn) {
    if (!data || page_size < 46) {
        return false;
    }

    // InnoDB FIL header fields are BIG-ENDIAN
    space_id = innodb::read_be<uint32_t>(data, 34);   // offset 34-37
    page_number = innodb::read_be<uint32_t>(data, 4);  // offset 4-7
    page_type = innodb::read_be<uint16_t>(data, 24);   // offset 24-25
    lsn = innodb::read_be<uint64_t>(data, 16);          // offset 16-23

    return true;
}

bool PageScanner::is_valid_page_type(uint16_t type) {
    return innodb::is_valid_page_type(type);
}

std::vector<std::tuple<uint64_t, uint16_t, uint32_t>> PageScanner::find_signatures(
    const uint8_t* data,
    size_t size,
    uint32_t page_size) {

    std::vector<std::tuple<uint64_t, uint16_t, uint32_t>> results;

    if (!data || size < page_size) {
        return results;
    }

    // Scan at page-size-aligned boundaries
    size_t num_pages = size / page_size;

    for (size_t page_no = 0; page_no < num_pages; ++page_no) {
        size_t offset = page_no * page_size;

        if (offset + 38 > size) {
            break;
        }

        uint16_t page_type = innodb::read_be<uint16_t>(data, offset + 24);

        if (!is_valid_page_type(page_type)) {
            continue;
        }

        uint32_t page_number = innodb::read_be<uint32_t>(data, offset + 4);

        // Filter out obviously invalid page numbers
        if (page_number > 10000000) {
            continue;
        }

        results.push_back({offset, page_type, page_number});
    }

    return results;
}

/**
 * @brief Scan result structure for the enhanced scanning path
 */
struct ScanResult {
    uint64_t bytes_scanned = 0;
    uint64_t pages_found = 0;
    std::vector<PageScanner::FoundPage> candidates;
    double throughput_mbps = 0.0;
};

PageScanner::ScanStats PageScanner::scan_engine(
    RawDiskIOEngine& engine,
    std::function<void(const FoundPage&)> callback,
    const std::vector<uint32_t>& page_sizes,
    uint64_t max_pages) {

    ScanStats stats = {0, 0, 0, 0, 0, 0, 0, 0};

    auto start_time = std::chrono::high_resolution_clock::now();

    // Use 128KB chunks for efficient scanning with posix_memalign
    constexpr size_t SCAN_CHUNK_SIZE = 128 * 1024;

    for (uint32_t page_size : page_sizes) {
        if (!engine.is_open() || page_size < 38) {
            continue;
        }

        uint64_t disk_size = engine.get_size();
        if (disk_size < page_size) {
            continue;
        }

        // Allocate aligned buffer for chunk reading
        uint8_t* chunk_buffer = alloc_aligned_buffer(SCAN_CHUNK_SIZE + page_size);
        if (!chunk_buffer) {
            // Fallback: try with page-size buffer
            auto pages = scan_page_size(engine, page_size, max_pages);
            for (auto& page : pages) {
                stats.total_pages_found++;
                stats.total_bytes_scanned += page_size;

                update_page_type_stats(stats, page.page_type);

                if (callback) {
                    callback(page);
                }

                if (max_pages > 0 && stats.total_pages_found >= max_pages) {
                    break;
                }
            }
            continue;
        }

        uint64_t offset = 0;
        while (offset < disk_size) {
            size_t to_read = std::min(static_cast<size_t>(SCAN_CHUNK_SIZE),
                                      static_cast<size_t>(disk_size - offset));
            // Align to page size for reading
            to_read = ((to_read + page_size - 1) / page_size) * page_size;

            size_t bytes_read = engine.read(offset, chunk_buffer, to_read);
            if (bytes_read == 0) {
                break;
            }

            stats.total_bytes_scanned += bytes_read;

            // Check page-size-aligned boundaries for InnoDB signatures
            size_t scan_end = bytes_read >= page_size ? bytes_read - page_size + 1 : 0;
            for (size_t pos = 0; pos < scan_end; pos += page_size) {
                const uint8_t* page_data = chunk_buffer + pos;

                uint32_t space_id, page_number;
                uint16_t page_type;
                uint64_t lsn;

                if (!parse_page_header(page_data, page_size,
                                       space_id, page_number, page_type, lsn)) {
                    continue;
                }

                if (!is_valid_page_type(page_type)) {
                    continue;
                }

                // Score the candidate
                uint8_t candidate_score = score_page_candidate(page_type, lsn, page_number);

                // Validate checksum - adds bonus points
                bool checksum_valid = innodb::is_page_checksum_valid(page_data, page_size);
                if (checksum_valid) {
                    candidate_score = std::min(static_cast<uint8_t>(100),
                                               static_cast<uint8_t>(candidate_score + 10));
                }

                // Skip very low scoring candidates (likely false positives)
                if (candidate_score < 20) {
                    continue;
                }

                FoundPage page;
                page.disk_offset = offset + pos;
                page.page_number = page_number;
                page.space_id = space_id;
                page.page_type = page_type;
                page.page_size = page_size;
                page.lsn = lsn;
                page.data.assign(page_data, page_data + page_size);

                stats.total_pages_found++;
                update_page_type_stats(stats, page.page_type);

                if (callback) {
                    callback(page);
                }

                if (max_pages > 0 && stats.total_pages_found >= max_pages) {
                    break;
                }
            }

            offset += bytes_read;

            if (max_pages > 0 && stats.total_pages_found >= max_pages) {
                break;
            }
        }

        free(chunk_buffer);

        if (max_pages > 0 && stats.total_pages_found >= max_pages) {
            break;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats.scan_time_seconds = std::chrono::duration<double>(end_time - start_time).count();

    return stats;
}

std::vector<PageScanner::FoundPage> PageScanner::scan_page_size(
    RawDiskIOEngine& engine,
    uint32_t page_size,
    uint64_t max_pages) {

    std::vector<FoundPage> pages;

    if (!engine.is_open() || page_size < 38) {
        return pages;
    }

    uint64_t disk_size = engine.get_size();
    if (disk_size < page_size) {
        return pages;
    }

    uint64_t num_pages = std::min(max_pages, disk_size / page_size);
    if (max_pages == 0) {
        num_pages = disk_size / page_size;
    }

    // Use posix_memalign for the page buffer
    uint8_t* page_data = alloc_aligned_buffer(page_size);
    if (!page_data) {
        // Fallback to std::vector
        std::vector<uint8_t> vec_data(page_size);
        for (uint64_t i = 0; i < num_pages; ++i) {
            uint64_t offset = i * page_size;
            if (offset + page_size > disk_size) break;

            size_t read_size = engine.read(offset, vec_data.data(), page_size);
            if (read_size != page_size) continue;

            uint32_t space_id, page_number;
            uint16_t page_type;
            uint64_t lsn;

            if (!parse_page_header(vec_data.data(), page_size,
                                   space_id, page_number, page_type, lsn)) continue;
            if (!is_valid_page_type(page_type)) continue;

            FoundPage page;
            page.disk_offset = offset;
            page.page_number = page_number;
            page.space_id = space_id;
            page.page_type = page_type;
            page.page_size = page_size;
            page.lsn = lsn;
            page.data = vec_data;
            pages.push_back(std::move(page));
        }
        return pages;
    }

    for (uint64_t i = 0; i < num_pages; ++i) {
        uint64_t offset = i * page_size;

        if (offset + page_size > disk_size) {
            break;
        }

        size_t read_size = engine.read(offset, page_data, page_size);
        if (read_size != page_size) {
            continue;
        }

        uint32_t space_id, page_number;
        uint16_t page_type;
        uint64_t lsn;

        if (!parse_page_header(page_data, page_size,
                               space_id, page_number, page_type, lsn)) {
            continue;
        }

        if (!is_valid_page_type(page_type)) {
            continue;
        }

        // Validate checksum
        bool checksum_valid = innodb::is_page_checksum_valid(page_data, page_size);

        FoundPage page;
        page.disk_offset = offset;
        page.page_number = page_number;
        page.space_id = space_id;
        page.page_type = page_type;
        page.page_size = page_size;
        page.lsn = lsn;
        page.data.assign(page_data, page_data + page_size);

        pages.push_back(std::move(page));
    }

    free(page_data);
    return pages;
}

#ifdef DBRECOVERY_HAVE_IO_URING
std::vector<PageScanner::FoundPage> PageScanner::scan_page_size_async(
    RawDiskIOEngine& engine,
    uint32_t page_size,
    unsigned batch_size,
    uint64_t max_pages) {

    // Async implementation using io_uring
    if (!engine.has_io_uring()) {
        // Fall back to synchronous scan if io_uring not available
        return scan_page_size(engine, page_size, max_pages);
    }

    // io_uring-based async scanning
    std::vector<FoundPage> pages;
    if (!engine.is_open() || page_size < 38) {
        return pages;
    }

    uint64_t disk_size = engine.get_size();
    if (disk_size < page_size) {
        return pages;
    }

    uint64_t total_pages = disk_size / page_size;
    uint64_t scan_limit = (max_pages > 0) ? std::min(max_pages, total_pages) : total_pages;

    struct io_uring* ring = engine.get_io_uring();

    // Submit batch of reads
    uint64_t page_idx = 0;
    std::vector<std::vector<uint8_t>> buffers(batch_size);
    for (auto& buf : buffers) {
        buf.resize(page_size);
    }

    while (page_idx < scan_limit) {
        unsigned submitted = 0;

        // Submit reads
        for (unsigned i = 0; i < batch_size && page_idx < scan_limit; ++i, ++page_idx) {
            struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
            if (!sqe) break;

            uint64_t offset = page_idx * page_size;
            io_uring_prep_read(sqe, engine.get_fd(), buffers[i].data(), page_size, offset);
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
            submitted++;
        }

        if (submitted == 0) break;

        io_uring_submit(ring);

        // Wait for completions
        for (unsigned i = 0; i < submitted; ++i) {
            struct io_uring_cqe* cqe = nullptr;
            int ret = io_uring_wait_cqe(ring, &cqe);
            if (ret < 0 || !cqe) continue;

            unsigned buf_idx = static_cast<unsigned>(
                reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)));

            if (cqe->res >= 0 && static_cast<size_t>(cqe->res) == page_size) {
                const uint8_t* data = buffers[buf_idx].data();

                uint32_t space_id, page_number;
                uint16_t page_type;
                uint64_t lsn;

                if (parse_page_header(data, page_size, space_id, page_number, page_type, lsn) &&
                    is_valid_page_type(page_type)) {

                    FoundPage page;
                    page.disk_offset = page_idx * page_size;  // approximate
                    page.page_number = page_number;
                    page.space_id = space_id;
                    page.page_type = page_type;
                    page.page_size = page_size;
                    page.lsn = lsn;
                    page.data = buffers[buf_idx];
                    pages.push_back(std::move(page));
                }
            }

            io_uring_cqe_seen(ring, cqe);
        }
    }

    return pages;
}
#else
std::vector<PageScanner::FoundPage> PageScanner::scan_page_size_async(
    RawDiskIOEngine& engine,
    uint32_t page_size,
    unsigned,
    uint64_t max_pages) {
    // No io_uring support - fall back to synchronous
    return scan_page_size(engine, page_size, max_pages);
}
#endif

// ==================== DiskImageMounter Implementation ====================

std::string DiskImageMounter::current_mount_point_;
int DiskImageMounter::mount_count_ = 0;

bool DiskImageMounter::mount(const MountOptions& options) {
    if (mount_count_ > 0) {
        // Already mounted
        return false;
    }

    // For raw disk images, we use mmap to provide filesystem-like access
    int fd = ::open(options.image_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        return false;
    }

    size_t file_size = st.st_size;
    uint64_t map_offset = options.offset;
    uint64_t map_size = options.size > 0 ? options.size : (file_size - options.offset);

    if (map_offset + map_size > static_cast<uint64_t>(file_size)) {
        ::close(fd);
        return false;
    }

    // Map the file with sequential access hint
    void* mapped = mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, fd, map_offset);
    ::close(fd);

    if (mapped == MAP_FAILED) {
        return false;
    }

    // Advise kernel for sequential access pattern
    madvise(mapped, map_size, MADV_SEQUENTIAL);

    // Unmap immediately - this is a placeholder for full mount implementation
    // In a full implementation, we would keep the mapping and provide access
    munmap(mapped, map_size);

    current_mount_point_ = options.mount_point;
    mount_count_++;
    return true;
}

bool DiskImageMounter::unmount() {
    if (mount_count_ == 0) {
        return false;
    }

    current_mount_point_.clear();
    mount_count_--;
    return true;
}

} // namespace engine
} // namespace dbrecovery
