/**
 * @file raw_disk_engine.hpp
 * @brief Raw Disk I/O Engine for MySQL 5.6 extreme recovery
 * 
 * Supports direct disk/partition scanning with 4K alignment
 * Optimized for scanning large volumes (TB+) with minimal memory footprint
 * 
 * Key features:
 * - Direct I/O with O_DIRECT when available
 * - io_uring support for async high-performance scanning
 * - 4K sector alignment enforcement
 * - Memory-mapped file fallback for read-only access
 */

#ifndef DBRECOVERY_ENGINE_RAW_DISK_ENGINE_HPP
#define DBRECOVERY_ENGINE_RAW_DISK_ENGINE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#ifdef DBRECOVERY_HAVE_IO_URING
#include <liburing.h>
#endif

namespace dbrecovery {
namespace engine {

// ==================== Alignment Constants ====================

/// @brief Standard disk sector size (512 bytes)
constexpr uint32_t SECTOR_SIZE = 512;

/// @brief Minimum alignment for disk I/O (4K page size)
constexpr uint32_t MIN_DISK_ALIGNMENT = 4096;

/// @brief Large buffer size for efficient scanning (64MB)
constexpr size_t LARGE_BUFFER_SIZE = 64 * 1024 * 1024;

/// @brief Default read chunk size (1MB)
constexpr size_t DEFAULT_CHUNK_SIZE = 1024 * 1024;

// ==================== I/O Mode Enum ====================

/// @brief I/O access mode
enum class IoMode {
    READ_ONLY,      ///< Read-only access ( safest for forensic )
    READ_WRITE,     ///< Read-write access (dangerous, use with caution)
    MEMORY_MAPPED   ///< Memory-mapped file
};

/// @brief Convert IoMode to string
inline std::string io_mode_to_string(IoMode mode) {
    switch (mode) {
        case IoMode::READ_ONLY:      return "READ_ONLY";
        case IoMode::READ_WRITE:     return "READ_WRITE";
        case IoMode::MEMORY_MAPPED:  return "MEMORY_MAPPED";
        default:                     return "UNKNOWN";
    }
}

// ==================== Scan Progress Callback ====================

/// @brief Callback type for scan progress
using ScanProgressCallback = std::function<void(uint64_t bytes_scanned, uint64_t total_bytes, double percent)>;

/// @brief Default progress callback (prints to stdout)
inline void default_progress_callback(uint64_t bytes_scanned, uint64_t total_bytes, double percent) {
    if (total_bytes > 0) {
        fprintf(stdout, "[PROGRESS] %.2f%% (%lu / %lu bytes)\n", percent, bytes_scanned, total_bytes);
        fflush(stdout);
    }
}

// ==================== RawDiskIOEngine ====================

/**
 * @brief Raw Disk I/O Engine for forensic data recovery
 * 
 * This engine provides low-level access to raw disk devices and image files.
 * It handles all alignment requirements and provides optimized I/O operations
 * for scanning large volumes in search of InnoDB data pages.
 * 
 * Key capabilities:
 * - Direct I/O with O_DIRECT flag when available
 * - io_uring support for async high-throughput scanning
 * - Memory-mapped file fallback
 * - 4K sector alignment enforcement
 * - Progress reporting for long-running operations
 */
class RawDiskIOEngine {
public:
    /// @brief Default constructor
    RawDiskIOEngine();
    
    /// @brief Destructor
    ~RawDiskIOEngine();

    // Disable copy
    RawDiskIOEngine(const RawDiskIOEngine&) = delete;
    RawDiskIOEngine& operator=(const RawDiskIOEngine&) = delete;

    /// @brief Move constructor
    RawDiskIOEngine(RawDiskIOEngine&& other) noexcept;
    
    /// @brief Move assignment
    RawDiskIOEngine& operator=(RawDiskIOEngine&& other) noexcept;

    /**
     * @brief Open a disk device or image file
     * 
     * @param path Path to the device (e.g., /dev/sdb) or image file
     * @param mode Access mode (default: READ_ONLY)
     * @return true if successful, false otherwise
     */
    bool open(const std::string& path, IoMode mode = IoMode::READ_ONLY);

    /**
     * @brief Close the current open device/file
     */
    void close();

    /**
     * @brief Read data from specified offset
     * 
     * Note: When using direct I/O, both offset and size must be 4K aligned.
     * This function handles alignment automatically by using buffered I/O as fallback.
     * 
     * @param offset Byte offset to read from
     * @param buffer Output buffer
     * @param size Number of bytes to read
     * @return Number of bytes actually read, or 0 on error
     */
    size_t read(uint64_t offset, uint8_t* buffer, size_t size);

    /**
     * @brief Write data to specified offset
     * 
     * @param offset Byte offset to write to
     * @param buffer Input buffer
     * @param size Number of bytes to write
     * @return Number of bytes actually written, or 0 on error
     */
    size_t write(uint64_t offset, const uint8_t* buffer, size_t size);

    /**
     * @brief Get the total size of the device/file
     * 
     * @return Total size in bytes, or 0 if not open
     */
    uint64_t get_size() const { return size_; }

    /**
     * @brief Check if device/file is currently open
     * 
     * @return true if open, false otherwise
     */
    bool is_open() const { return is_open_; }

    /**
     * @brief Get the open file descriptor
     * 
     * @return File descriptor, or -1 if not available
     */
    int get_fd() const { return fd_; }

    /**
     * @brief Get the I/O mode
     * 
     * @return Current I/O mode
     */
    IoMode get_mode() const { return mode_; }

    /**
     * @brief Check if direct I/O is available
     * 
     * @return true if direct I/O is supported
     */
    bool has_direct_io() const { return use_direct_io_; }

    /**
     * @brief Set the scan progress callback
     * 
     * @param callback Callback function
     */
    void set_progress_callback(ScanProgressCallback callback) {
        progress_callback_ = callback ? callback : default_progress_callback;
    }

    /**
     * @brief Scan the disk for data patterns
     * 
     * This function reads the disk in chunks and applies a callback to each chunk.
     * It handles progress reporting automatically.
     * 
     * @param callback Callback function for each chunk
     * @param chunk_size Size of each chunk (default: 1MB)
     * @param max_bytes Maximum bytes to scan (0 = scan all)
     * @return true if scan completed, false on error
     */
    bool scan(ScanProgressCallback callback, size_t chunk_size = DEFAULT_CHUNK_SIZE, uint64_t max_bytes = 0);

    /**
     * @brief Initialize io_uring for async I/O (Linux 5.1+)
     * 
     * @param depth Queue depth for io_uring
     * @return true if successfully initialized
     */
    bool init_io_uring(unsigned depth = 32);

    /**
     * @brief Cleanup io_uring resources
     */
    void cleanup_io_uring();

    /**
     * @brief Check if io_uring is available
     * 
     * @return true if io_uring is enabled
     */
    bool has_io_uring() const {
#ifdef DBRECOVERY_HAVE_IO_URING
        return io_uring_initialized_; 
#else
        return false;
#endif
    }

#ifdef DBRECOVERY_HAVE_IO_URING
    /**
     * @brief Get the underlying io_uring context
     * 
     * @return Pointer to io_uring context
     */
    struct io_uring* get_io_uring() { return &ring_; }
#endif

    /**
     * @brief Flush pending writes
     * 
     * @return true if successful
     */
    bool flush();

    /**
     * @brief Trim/discard range (for SSD devices)
     * 
     * @param offset Start offset
     * @param length Length to trim
     * @return true if successful
     */
    bool trim(uint64_t offset, uint64_t length);

    /**
     * @brief Get theblksize for optimal I/O
     * 
     * @return Optimal block size
     */
    uint32_t get_optimal_block_size() const { return optimal_block_size_; }

private:
    /// @brief Implementation details
    class Impl;
    std::unique_ptr<Impl> impl_;

    int fd_;
    uint64_t size_;
    bool is_open_;
    IoMode mode_;
    bool use_direct_io_;
    uint32_t optimal_block_size_;
    
    ScanProgressCallback progress_callback_;

#ifdef DBRECOVERY_HAVE_IO_URING
    struct io_uring ring_;
    bool io_uring_initialized_;
#endif
};

// ==================== PageScanner ====================

/**
 * @brief Disk page scanner for finding InnoDB pages
 * 
 * Scans raw disk devices for InnoDB page signatures and extracts page metadata.
 */
class PageScanner {
public:
    /// @brief Found page information
    struct FoundPage {
        uint64_t disk_offset;       ///< Offset in the disk/image
        uint32_t page_number;       ///< Page number from page header
        uint32_t space_id;          ///< Tablespace ID
        uint16_t page_type;         ///< Page type (FIL_PAGE_INDEX, etc.)
        uint32_t page_size;         ///< Page size
        uint64_t lsn;               ///< Log Sequence Number
        std::vector<uint8_t> data;  ///< Full page data
    };

    /// @brief Page scan statistics
    struct ScanStats {
        uint64_t total_bytes_scanned;
        uint64_t total_pages_found;
        uint64_t index_pages;
        uint64_t sdi_pages;
        uint64_t undo_pages;
        uint64_t blob_pages;
        uint64_t allocated_pages;
        double scan_time_seconds;
    };

    /**
     * @brief Scan for InnoDB pages
     * 
     * @param engine Reference to the disk engine
     * @param callback Callback for each found page
     * @param page_sizes List of page sizes to scan for (default: all supported)
     * @param max_pages Maximum pages to find (0 = no limit)
     * @return Scan statistics
     */
    static ScanStats scan_engine(
        RawDiskIOEngine& engine,
        std::function<void(const FoundPage&)> callback,
        const std::vector<uint32_t>& page_sizes = {4096, 8192, 16384, 32768, 65536},
        uint64_t max_pages = 0);

    /**
     * @brief Scan for pages in a specific page size
     * 
     * @param engine Reference to the disk engine
     * @param page_size Page size to scan for
     * @param max_pages Maximum pages to find
     * @return Found pages
     */
    static std::vector<FoundPage> scan_page_size(
        RawDiskIOEngine& engine,
        uint32_t page_size,
        uint64_t max_pages = 0);

    /**
     * @brief Scan for pages using io_uring for parallel reads
     * 
     * @param engine Reference to the disk engine
     * @param page_size Page size to scan for
     * @param batch_size Number of pages to read in parallel
     * @param max_pages Maximum pages to find
     * @return Found pages
     */
    static std::vector<FoundPage> scan_page_size_async(
        RawDiskIOEngine& engine,
        uint32_t page_size,
        unsigned batch_size = 64,
        uint64_t max_pages = 0);

    /**
     * @brief Find page signatures in raw data
     * 
     * @param data Raw data buffer
     * @param size Buffer size
     * @param page_size Page size
     * @return Vector of (offset, page_type, page_number) tuples
     */
    static std::vector<std::tuple<uint64_t, uint16_t, uint32_t>> find_signatures(
        const uint8_t* data,
        size_t size,
        uint32_t page_size);

    /// @brief Check if page type is valid
    static bool is_valid_page_type(uint16_t type);

private:
    /// @brief Parse page header from raw data
    static bool parse_page_header(const uint8_t* data, uint32_t page_size,
                                   uint32_t& space_id, uint32_t& page_number,
                                   uint16_t& page_type, uint64_t& lsn);
};

// ==================== DiskImageMounter ====================

/**
 * @brief Mount disk images as virtual file systems
 * 
 * Provides filesystem-like access to disk images and raw devices,
 * useful for tools that expect file paths.
 */
class DiskImageMounter {
public:
    /// @brief Mount options
    struct MountOptions {
        std::string image_path;
        std::string mount_point;
        IoMode mode;
        uint64_t offset;      ///< Offset within the image (for partitions)
        uint64_t size;        ///< Size of the mount (0 = auto-detect)
    };

    /**
     * @brief Mount a disk image
     * 
     * @param options Mount options
     * @return true if successful
     */
    static bool mount(const MountOptions& options);

    /**
     * @brief Unmount current mount
     * 
     * @return true if successful
     */
    static bool unmount();

private:
    static std::string current_mount_point_;
    static int mount_count_;
};

// ==================== Implementation ====================

class RawDiskIOEngine::Impl {
public:
    Impl() : fd_(-1), size_(0), is_open_(false), 
             use_direct_io_(false), optimal_block_size_(4096) {
#ifdef DBRECOVERY_HAVE_IO_URING
        io_uring_init(&ring_);
#endif
    }

    ~Impl() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        is_open_ = false;
    }

    int fd_;
    uint64_t size_;
    bool is_open_;
    bool use_direct_io_;
    uint32_t optimal_block_size_;

#ifdef DBRECOVERY_HAVE_IO_URING
    struct io_uring ring_;
#endif
};

} // namespace engine
} // namespace dbrecovery

#endif // DBRECOVERY_ENGINE_RAW_DISK_ENGINE_HPP
